// include/batbox/conversation/Conversation.hpp
// =============================================================================
// batbox::conversation::Conversation — one-session conversation driver (CPP 3.6).
//
// Responsibilities:
//   - Own messages_ (the canonical internal conversation history)
//   - user_message(text) : append a User-role Message and persist to SessionStore
//   - run_turn(ct)       : build ChatRequest, call Client::stream_chat, accumulate
//                          assistant message, check auto-compact, persist, return
//   - restore(sf)        : reload messages and working_dir from a SessionFile
//
// Tool-call loop (CPP 3.7):
//   When registry_ and gate_ are non-null and the model responds with
//   finish_reason="tool_calls", run_turn drives a tool-call loop:
//     1. Accumulate ToolCallDelta fragments during streaming.
//     2. On finish_reason="tool_calls": ToolCallOrchestrator::dispatch_all().
//     3. Append assistant (with tool_calls) + Tool messages to messages_.
//     4. Persist all new messages to SessionStore.
//     5. Loop back to a new inference request.
//     6. Stop when finish_reason="stop" or MAX_TOOL_TURNS is reached.
//   Cancellation is checked before every stream_chat call and propagated
//   from dispatch_all (via ToolCallOrchestrator).
//
// No-tools path (CPP 3.6):
//   When registry_ == nullptr || gate_ == nullptr, the tool-call branch is
//   disabled.  If the model requests tool_calls in this configuration,
//   run_turn returns Err("tool_calls: no registry/gate configured").
//
// Session lifecycle:
//   The session is created lazily on the first call to user_message() via an
//   internal ensure_session_started() helper.  All subsequent appends reference
//   the same session id.
//
// Auto-compact:
//   Before building the ChatRequest, run_turn checks ContextWindow::needs_compact().
//   When true it calls Compactor::compact() to summarise the head and keeps the
//   last keep_last_n_turns_verbatim turns intact, then proceeds with the
//   compacted message list.
//
// Streaming:
//   The on_delta_cb passed to the constructor is called on the calling thread
//   for each content token delivered by the SSE stream.  It is invoked
//   synchronously inside the stream_chat write-callback.
//
// Thread safety:
//   Conversation is NOT thread-safe.  All public methods must be called from
//   a single thread.
//
// Build standalone (no CMake, from repo root):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/integration/test_conversation_basic.cpp \
//       src/conversation/Conversation.cpp \
//       src/conversation/Message.cpp \
//       src/conversation/ContextWindow.cpp \
//       src/conversation/Compactor.cpp \
//       src/conversation/ToolCallOrchestrator.cpp \
//       src/inference/Client.cpp \
//       src/inference/ChatRequest.cpp \
//       src/inference/SseParser.cpp \
//       src/inference/ToolCallAccumulator.cpp \
//       src/session/SessionStore.cpp \
//       src/session/SessionFile.cpp \
//       src/session/SessionIndex.cpp \
//       src/session/SessionRecovery.cpp \
//       src/tools/ToolRegistry.cpp \
//       src/permissions/PermissionGate.cpp \
//       src/permissions/PermissionMode.cpp \
//       src/permissions/PermissionRule.cpp \
//       src/permissions/PatternMatcher.cpp \
//       src/permissions/PermissionStore.cpp \
//       src/config/SettingsLoader.cpp \
//       src/core/Uuid.cpp src/core/CancelToken.cpp \
//       src/core/Logging.cpp src/core/Paths.cpp src/core/Json.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libcpr.a \
//       build/vcpkg_installed/arm64-osx/lib/libcurl.a \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       build/vcpkg_installed/arm64-osx/lib/libz.a \
//       build/vcpkg_installed/arm64-osx/lib/libspdlog.a \
//       build/vcpkg_installed/arm64-osx/lib/libfmt.a \
//       -o /tmp/test_conversation_basic && /tmp/test_conversation_basic
// =============================================================================

#pragma once

#include <batbox/config/Config.hpp>
#include <batbox/conversation/Compactor.hpp>
#include <batbox/conversation/ContextWindow.hpp>
#include <batbox/conversation/Message.hpp>
#include <batbox/conversation/PlanMode.hpp>
#include <batbox/conversation/SystemPrompt.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/inference/Client.hpp>
#include <batbox/session/SessionFile.hpp>
#include <batbox/session/SessionStore.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Forward declarations — avoid pulling in full headers for optional dependencies.
namespace batbox::tools    { class ToolRegistry; }
namespace batbox::permissions { class PermissionGate; }

namespace batbox::conversation {

// =============================================================================
// Conversation — owns messages_, drives one turn
// =============================================================================

class Conversation {
public:
    // -------------------------------------------------------------------------
    // Constructor
    //
    // client       — inference client; must outlive this Conversation.
    // store        — session store for persistence; must outlive this Conversation.
    // cfg          — runtime config (model, compact settings, etc.); const-ref.
    // working_dir  — working directory stored in the session file and used for
    //               resume matching.  Defaults to std::filesystem::current_path()
    //               when the empty-path default is passed.
    // on_delta_cb  — optional callback invoked for each content token during
    //               streaming.  Called on the calling thread.  May be nullptr.
    // registry     — optional tool registry; when non-null (with gate), enables
    //               the tool-call loop in run_turn().  Must outlive Conversation.
    // gate         — optional permission gate; required together with registry
    //               to enable tool dispatch.  Must outlive Conversation.
    // -------------------------------------------------------------------------
    Conversation(batbox::inference::Client&                client,
                 batbox::session::SessionStore&            store,
                 const batbox::config::Config&             cfg,
                 std::filesystem::path                     working_dir  = {},
                 std::function<void(std::string_view)>     on_delta_cb  = nullptr,
                 batbox::tools::ToolRegistry*              registry     = nullptr,
                 batbox::permissions::PermissionGate*      gate         = nullptr,
                 batbox::conversation::PlanMode*           plan_mode    = nullptr);

    // Non-copyable, non-movable (holds references).
    Conversation(const Conversation&)            = delete;
    Conversation& operator=(const Conversation&) = delete;
    Conversation(Conversation&&)                 = delete;
    Conversation& operator=(Conversation&&)      = delete;

    ~Conversation() = default;

    // -------------------------------------------------------------------------
    // user_message()
    //
    // Appends a User-role Message to messages_ with a freshly-generated UUID
    // and the current timestamp.  Lazily starts a new session via SessionStore
    // if this is the first call (ensure_session_started()).  Then persists the
    // message via SessionStore::append_message().
    //
    // Blueprint contract:
    //   void user_message(std::string_view text)
    // -------------------------------------------------------------------------
    void user_message(std::string_view text);

    // -------------------------------------------------------------------------
    // run_turn()
    //
    // Drives one complete inference turn including the tool-call loop:
    //   1. Check ContextWindow::needs_compact(); if true, call Compactor::compact().
    //   2. Build ChatRequest from messages_ using cfg_.api.default_model.
    //      When registry_ is non-null, includes tool schemas from the registry.
    //   3. Call Client::stream_chat() with an on_delta lambda that:
    //        a. Appends content fragments to the pending assistant message.
    //        b. Fires on_delta_cb_(token) for each non-empty content fragment.
    //        c. Accumulates ToolCallDelta fragments via ToolCallOrchestrator.
    //        d. Captures finish_reason and usage from terminal chunks.
    //   4. If finish_reason == "tool_calls" and registry_+gate_ are set:
    //        a. Build assistant Message with tool_calls from accumulator state.
    //        b. Call ToolCallOrchestrator::dispatch_all(ctx) → Tool messages.
    //        c. Append assistant + tool messages to messages_, persist all.
    //        d. Loop back to step 2 (no re-compaction; bounded by MAX_TOOL_TURNS).
    //   5. If finish_reason == "stop" (or loop limit reached):
    //        a. Finalise the assistant message (set usage if available).
    //        b. Ensure the session is started (lazy create if needed).
    //        c. Persist the assistant message via SessionStore::append_message().
    //        d. Return Ok(void).
    //
    // Tool-call loop is bounded at MAX_TOOL_TURNS (20) to prevent infinite loops.
    // When the limit is reached the last assistant message is persisted and the
    // function returns Ok(void) — the caller sees a complete (if truncated) turn.
    //
    // Returns Err when:
    //   - ct is pre-cancelled (Err("cancelled") before any work).
    //   - stream_chat returns Err.
    //   - compaction returns Err.
    //   - session persistence returns Err.
    //   - finish_reason == "tool_calls" and no registry/gate configured.
    //
    // @param ct  Cancellation token.  When fired, stream_chat aborts and
    //            run_turn returns Err("cancelled").
    //
    // Blueprint contract:
    //   Result<void> run_turn(CancelToken ct)
    // -------------------------------------------------------------------------
    [[nodiscard]] Result<void> run_turn(batbox::CancelToken ct);

    // -------------------------------------------------------------------------
    // restore()
    //
    // Restores the conversation state from a previously loaded SessionFile.
    // Clears messages_, then parses each Json in sf.messages via
    // batbox::conversation::from_json().  Sets working_dir_ from sf.working_dir.
    // Sets session_id_ to sf.id.to_string() so subsequent appends go to the
    // correct session file.
    //
    // Returns Ok(void) on success.
    // Returns Err(msg) if any message in sf.messages fails to parse.
    //
    // Blueprint contract:
    //   Result<void> restore(const SessionFile& sf)
    // -------------------------------------------------------------------------
    [[nodiscard]] Result<void> restore(const batbox::session::SessionFile& sf);

    // -------------------------------------------------------------------------
    // Accessors (for testing and inspection)
    // -------------------------------------------------------------------------

    /// Read-only view of the current message history.
    [[nodiscard]] const std::vector<Message>& messages() const noexcept {
        return messages_;
    }

    /// Returns the current session id (empty string if no session started yet).
    [[nodiscard]] const std::string& session_id() const noexcept {
        return session_id_;
    }

    // -------------------------------------------------------------------------
    // clear_messages()
    //
    // Discards all in-memory conversation turns without touching the session
    // store.  Intended for /clear dispatch from the TUI path (App.cpp
    // TuiConvAdapter::reset_messages).  The session file is NOT rewritten —
    // the cleared state only affects the current in-process run_turn() calls.
    //
    // Safe to call from the FTXUI UI thread; must NOT be called concurrently
    // with a run_turn() on a worker thread (the caller holds the UI thread lock
    // before dispatching on_submit, so the worker thread has already detached
    // for any previous turn before Enter is pressed again).
    //
    // Blueprint contract (TUI-T3):
    //   void clear_messages()
    // -------------------------------------------------------------------------
    void clear_messages() noexcept;

    // -------------------------------------------------------------------------
    // start_session()
    //
    // Eagerly starts a new session in the SessionStore without requiring a
    // user message.  Idempotent: if the session is already started this is a
    // no-op and returns Ok(void).
    //
    // Intended for callers (e.g. App::run TUI path) that need the session UUID
    // available at startup so it can be logged before the first user turn.
    //
    // Returns Ok(void) on success.  Returns Err(message) if the SessionStore
    // fails to create the session file.
    //
    // Blueprint contract:
    //   Result<void> start_session()
    // -------------------------------------------------------------------------
    [[nodiscard]] Result<void> start_session();

    // -------------------------------------------------------------------------
    // set_on_message_appended_cb()
    //
    // Register a callback invoked from the worker thread each time a tool-call
    // assistant message or tool-result message is appended to messages_ inside
    // run_turn().  Use this to forward these messages to the TUI (e.g. post a
    // make_message_appended_event to ScreenManager).
    //
    // The callback receives:
    //   role      — "assistant" (for the tool-call message) or "tool"
    //   tool_name — tool name string when role=="tool"; empty for "assistant"
    //   content   — message body (tool result text, or empty for tool-call msgs)
    //   is_error  — true when the tool result represents an error
    //
    // Thread safety: this setter MUST be called before any background thread
    // calls run_turn().  The callback itself is invoked on the worker thread
    // (the same thread that calls run_turn()), so it must be thread-safe.
    //
    // Blueprint contract:
    //   void set_on_message_appended_cb(std::function<void(std::string_view role,
    //       std::string_view tool_name, std::string_view content, bool is_error)>)
    // -------------------------------------------------------------------------
    void set_on_message_appended_cb(
        std::function<void(std::string_view /*role*/,
                           std::string_view /*tool_name*/,
                           std::string_view /*content*/,
                           bool             /*is_error*/)> cb);

    // -------------------------------------------------------------------------
    // set_on_tool_running_cb()
    //
    // Register a callback invoked from the worker thread immediately before
    // ToolCallOrchestrator::dispatch_all() is called in run_turn().
    //
    // The callback receives:
    //   tool_name    — name of the first tool in the dispatch batch (e.g. "Bash")
    //   args_summary — one-line preview of the first tool's primary argument,
    //                  truncated to ~80 chars (e.g. "manifest.json" for read_file)
    //   tool_count   — total number of tool calls in this dispatch batch
    //
    // Use this to forward a "tool running" indicator to the TUI (e.g. post a
    // make_tool_running_event to ScreenManager so InputBar shows the status).
    //
    // Thread safety: this setter MUST be called before any background thread
    // calls run_turn().  The callback itself is invoked on the worker thread.
    //
    // Blueprint contract:
    //   void set_on_tool_running_cb(std::function<void(std::string_view, std::string_view, int)>)
    // -------------------------------------------------------------------------
    void set_on_tool_running_cb(
        std::function<void(std::string_view /*tool_name*/,
                           std::string_view /*args_summary*/,
                           int              /*tool_count*/)> cb);

    // -------------------------------------------------------------------------
    // set_on_tool_done_cb()
    //
    // Register a callback invoked from the worker thread immediately after
    // ToolCallOrchestrator::dispatch_all() returns in run_turn().
    //
    // Use this to clear the "tool running" indicator in the TUI (e.g. post a
    // make_tool_done_event to ScreenManager so InputBar resets the status row).
    //
    // Thread safety: this setter MUST be called before any background thread
    // calls run_turn().  The callback itself is invoked on the worker thread.
    //
    // Blueprint contract:
    //   void set_on_tool_done_cb(std::function<void()>)
    // -------------------------------------------------------------------------
    void set_on_tool_done_cb(std::function<void()> cb);

    // -------------------------------------------------------------------------
    // set_on_reasoning_started_cb()
    //
    // Register a callback invoked from the worker thread the FIRST time a
    // delta.reasoning_content chunk arrives in a streaming turn.
    //
    // Use this to post a make_thinking_started_event to ScreenManager so
    // InputBar shows "· thinking..." in the status row while Magistral (or any
    // other reasoning model) is in its chain-of-thought phase.
    //
    // The callback fires at most once per tool-call loop iteration.  It is
    // reset at the start of each iteration (alongside accumulated_content).
    //
    // Thread safety: this setter MUST be called before any background thread
    // calls run_turn().  The callback itself is invoked on the worker thread.
    //
    // Blueprint contract:
    //   void set_on_reasoning_started_cb(std::function<void()>)
    // -------------------------------------------------------------------------
    void set_on_reasoning_started_cb(std::function<void()> cb);

    // -------------------------------------------------------------------------
    // set_on_reasoning_stopped_cb()
    //
    // Register a callback invoked from the worker thread when the reasoning
    // phase ends.  This occurs on the FIRST of:
    //   a. The first non-empty content delta arrives after reasoning_content.
    //   b. finish_reason is seen while still in the reasoning phase (e.g. a
    //      model that ends with only reasoning and no visible content).
    //
    // Use this to post a make_thinking_stopped_event to ScreenManager so
    // InputBar clears the "· thinking..." indicator.
    //
    // The callback fires at most once per tool-call loop iteration.
    //
    // Thread safety: this setter MUST be called before any background thread
    // calls run_turn().  The callback itself is invoked on the worker thread.
    //
    // Blueprint contract:
    //   void set_on_reasoning_stopped_cb(std::function<void()>)
    // -------------------------------------------------------------------------
    void set_on_reasoning_stopped_cb(std::function<void()> cb);

    // -------------------------------------------------------------------------
    // set_on_usage_delta_cb()
    //
    // Register a callback invoked from the worker thread after each terminal
    // UsageDelta is received (once per sub-turn / streaming call).  The
    // callback receives CUMULATIVE session totals (all turns combined).
    //
    // Use this to forward usage to the TUI status row via the event system:
    //   conv->set_on_usage_delta_cb([&screen_mgr](uint32_t tok, double cost) {
    //       screen_mgr.post_event(
    //           batbox::tui::make_status_update_event_with_usage(tok, cost));
    //   });
    //
    // Thread safety: this setter MUST be called before any background thread
    // calls run_turn().  The callback is invoked on the worker thread; it must
    // NOT call InputBar::set_usage() directly (UI-thread only).  Use
    // make_status_update_event_with_usage() → ScreenManager::post_event() instead.
    //
    // Blueprint contract (TUI-FIX-T6 / A3):
    //   void set_on_usage_delta_cb(std::function<void(uint32_t, double)>)
    // -------------------------------------------------------------------------
    void set_on_usage_delta_cb(std::function<void(uint32_t /*tokens*/,
                                                   double   /*cost_usd*/)> cb);

    // -------------------------------------------------------------------------
    // compose_system_prompt()
    //
    // Composes the final system prompt for one inference turn by merging:
    //   1. Plan-mode read-only prefix (when plan_mode is active)
    //   2. Base BatBox system instructions
    //   3. User BATBOX.md (~/.batbox/BATBOX.md, if present)
    //   4. Project BATBOX.md (walked up from working_dir_, if present)
    //
    // Delegates to batbox::conversation::compose_system_prompt(plan_mode,
    // working_dir_).  This method is provided to satisfy the blueprint
    // contract; call the free function directly when no Conversation instance
    // is available.
    //
    // @param plan_mode  true when the PlanMode state machine is in Planning or
    //                   Approved state (i.e. write-side tools are blocked).
    //
    // Blueprint contract:
    //   std::string compose_system_prompt() const
    //
    // Note: the blueprint contract signature takes no parameters.  The plan_mode
    // flag is obtained from the caller's PlanMode instance and passed in to keep
    // Conversation decoupled from PlanMode.
    // -------------------------------------------------------------------------
    [[nodiscard]] std::string compose_system_prompt(bool plan_mode = false) const;

private:
    // Injected dependencies (all non-owning references / pointers).
    batbox::inference::Client&            client_;
    batbox::session::SessionStore&        store_;
    const batbox::config::Config&         cfg_;

    // Optional tool infrastructure (nullptr = no-tools mode).
    batbox::tools::ToolRegistry*          registry_;   ///< Non-owning; may be null.
    batbox::permissions::PermissionGate*  gate_;       ///< Non-owning; may be null.

    // Optional plan-mode state (nullptr = no plan-mode tracking).
    // When non-null, run_turn() queries plan_mode_->is_planning() to decide
    // whether to include the plan-mode prefix in the system prompt.
    batbox::conversation::PlanMode*       plan_mode_;  ///< Non-owning; may be null.

    // Session state.
    std::filesystem::path working_dir_;
    std::string           session_id_;   ///< Empty until first ensure_session_started().

    // Conversation history (internal representation).
    std::vector<Message> messages_;

    // Token streaming callback (may be nullptr).
    std::function<void(std::string_view)> on_delta_cb_;

    // Message-appended callback (may be nullptr).
    // Called from the worker thread after each tool-call or tool-result
    // message is appended to messages_ during run_turn().
    std::function<void(std::string_view /*role*/,
                       std::string_view /*tool_name*/,
                       std::string_view /*content*/,
                       bool             /*is_error*/)> on_message_appended_cb_;

    // Tool-running callback (may be nullptr).
    // Called from the worker thread immediately before dispatch_all() in run_turn().
    // Receives the name of the first tool in the dispatch batch.
    std::function<void(std::string_view /*tool_name*/,
                           std::string_view /*args_summary*/,
                           int              /*tool_count*/)> on_tool_running_cb_;

    // Tool-done callback (may be nullptr).
    // Called from the worker thread immediately after dispatch_all() returns.
    std::function<void()> on_tool_done_cb_;

    // Reasoning-started callback (may be nullptr).
    // Called from the worker thread the first time delta.reasoning_content has
    // a value in a streaming turn.  Fires at most once per tool-call iteration.
    std::function<void()> on_reasoning_started_cb_;

    // Reasoning-stopped callback (may be nullptr).
    // Called from the worker thread when the reasoning phase ends (first content
    // chunk arrives, or finish_reason seen while still in reasoning phase).
    // Fires at most once per tool-call iteration.
    std::function<void()> on_reasoning_stopped_cb_;

    // Usage-delta callback (A3 / TUI-FIX-T6 — may be nullptr).
    // Called from the worker thread after each terminal UsageDelta in run_turn().
    // Receives CUMULATIVE session token count + cost (accumulated across all turns).
    std::function<void(uint32_t /*tokens*/, double /*cost_usd*/)> on_usage_delta_cb_;

    // Cumulative session usage accumulators (A3).
    // Incremented on every terminal UsageDelta in run_turn().
    // Reset only when a new Conversation object is created (not on /clear).
    uint32_t session_tokens_{0};
    double   session_cost_usd_{0.0};

    // -------------------------------------------------------------------------
    // Private helpers
    // -------------------------------------------------------------------------

    /// Lazily creates a new session in the store if session_id_ is empty.
    /// On success sets session_id_.  Returns Err on store failure.
    [[nodiscard]] Result<void> ensure_session_started();

    // -------------------------------------------------------------------------
    // TUI-FLOW-T3 — first-token latency instrumentation
    // -------------------------------------------------------------------------

    /// Timestamp captured in user_message() when the user submits.
    /// Used in the streaming lambda in run_turn() to compute first_token_ms.
    std::chrono::steady_clock::time_point submit_time_{};

    /// Guards so the first-token latency is only recorded once per tool-call
    /// loop iteration (reset at the top of each iteration, set on first delta).
    bool first_token_recorded_{false};

    /// Convert messages_ to the wire format (WireMessage[]) for ChatRequest.
    /// When registry is non-null, populates req.tools with available schemas.
    [[nodiscard]] static batbox::inference::ChatRequest
    build_chat_request(const std::vector<Message>&       messages,
                       const batbox::config::Config&     cfg,
                       batbox::tools::ToolRegistry*      registry = nullptr,
                       const std::string&                system_prompt = {});
};

} // namespace batbox::conversation
