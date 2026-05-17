// include/batbox/tui/Events.hpp
//
// BatBox custom ftxui::Event subtypes.
//
// Design
// ------
// ftxui::Event carries identity in its input_ string and does not provide a
// generic payload slot.  BatBox uses Event::Special(<name>) for identity and
// stores payloads in a thread-safe static registry keyed by a unique token
// embedded in the special-string.  Factory functions create an event and
// register its payload atomically; receivers extract the payload with the
// matching getter.
//
// Event name constants (used by component OnEvent handlers for equality tests):
//   kEvToken            "batbox.token"
//   kEvAgentsDirty      "batbox.agents-dirty"
//   kEvDemonDirty       "batbox.demon-dirty"
//   kEvStatusUpdate     "batbox.status-update"
//   kEvModalShow        "batbox.modal-show"
//   kEvModalHide        "batbox.modal-hide"
//   kEvUserMessage      "batbox.user-message"
//   kEvStreamDone       "batbox.stream-done"
//   kEvMessageAppended  "batbox.message-appended"
//   kEvToolRunning         "batbox.tool-running"
//   kEvToolDone            "batbox.tool-done"
//   kEvThinkingStarted     "batbox.thinking-started"
//   kEvThinkingStopped     "batbox.thinking-stopped"
//   kEvPlanApprovalShow    "batbox.plan-approval-show"
//   kEvQuestionShow        "batbox.question-show"
//   kEvQuestionResolved    "batbox.question-resolved"
//
// Factory / extractor pairs (see below for full signatures).
//
// Thread safety
// -------------
// Factories may be called from background threads (SSE reader, sidecar monitor,
// agent orchestrator).  The payload registry is protected by a std::mutex.
// Once the event is dispatched via ScreenInteractive::PostEvent, the main-loop
// thread calls the extractor under the same lock, then erases the entry.
//
// Usage (background thread -> main loop)
// ----------------------------------------
//   // Background:
//   screen.PostEvent(batbox::tui::make_token_event("hello "));
//
//   // Component::OnEvent (main-loop thread):
//   if (event == batbox::tui::Events::Token) {
//       auto p = batbox::tui::extract_token(event);
//       append_text(p.text);
//       return true;
//   }

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <ftxui/component/event.hpp>

namespace batbox::tui {

// =============================================================================
// Event name string constants
//
// Components compare against these with operator==:
//   if (event == Events::Token) { ... }
// =============================================================================
struct Events {
    static const ftxui::Event Token;         ///< New SSE token arrived
    static const ftxui::Event AgentsDirty;   ///< Sub-agent panel needs redraw
    static const ftxui::Event DemonDirty;    ///< Demon panel needs redraw
    static const ftxui::Event StatusUpdate;  ///< Sidecar lifecycle change
    static const ftxui::Event ModalShow;     ///< Show a modal card
    static const ftxui::Event ModalHide;     ///< Dismiss the current modal card
    static const ftxui::Event UserMessage;   ///< User submitted a message to the chat
    static const ftxui::Event StreamDone;    ///< Assistant turn completed (success or error)
    static const ftxui::Event MessageAppended; ///< A complete message (tool call or tool result) was appended to history
    static const ftxui::Event ToolRunning;       ///< A tool call is about to be dispatched (status row indicator)
    static const ftxui::Event ToolDone;           ///< Tool dispatch finished; status row indicator should be cleared
    static const ftxui::Event ThinkingStarted;    ///< First reasoning_content chunk arrived — show "thinking..." in status row
    static const ftxui::Event ThinkingStopped;    ///< First content chunk arrived (or stream ended) — clear thinking indicator
    static const ftxui::Event SpinnerTick;         ///< 1Hz tick from the spinner timer thread — advances elapsed counter in ChatView
    static const ftxui::Event PlanApprovalShow;  ///< Show the PlanApprovalCard modal (render-wake trigger for ExitPlanMode)
    static const ftxui::Event QuestionShow;      ///< Post from worker thread: request an AskUserQuestion card (render-wake trigger)
    static const ftxui::Event QuestionResolved;  ///< Post from UI thread: user resolved an AskUserQuestion card
};

// =============================================================================
// Payload structs
// =============================================================================

/// Payload for Events::Token — one SSE text fragment from the assistant.
struct TokenPayload {
    std::string text;       ///< Raw UTF-8 fragment (may be multi-char or empty)
    std::string agent_id;   ///< Which agent produced the token ("" = primary)
};

/// Sidecar lifecycle state for Events::StatusUpdate.
enum class SidecarState : uint8_t {
    Cold      = 0,
    Starting  = 1,
    Running   = 2,
    Crashed   = 3,
};

/// Payload for Events::StatusUpdate — sidecar lifecycle change.
///
/// Extended (TUI-FIX-T6 / A3): when tokens and cost_usd are present the
/// payload carries a cumulative usage update for the status row.  Workers
/// post usage-bearing events via make_status_update_event_with_usage();
/// sidecar lifecycle changes use the existing make_status_update_event().
struct StatusUpdatePayload {
    SidecarState            state{SidecarState::Cold};
    std::string             detail;    ///< Optional human-readable detail (e.g. exit code)
    std::optional<uint32_t> tokens;    ///< Cumulative session token count (A3)
    std::optional<double>   cost_usd;  ///< Cumulative session cost in USD (A3)
};

/// Payload for Events::AgentsDirty — which sub-agent changed.
struct AgentsDirtyPayload {
    std::string agent_id;   ///< Affected agent ("" = all agents changed)
    uint32_t    step{0};    ///< Current step counter for the agent
    uint32_t    tokens{0};  ///< Running token count
    std::string status;     ///< Short status string ("running", "done", "error")
};

/// Permission / modal result values.
enum class ModalResult : uint8_t {
    Deny        = 0,
    Allow       = 1,
    AlwaysAllow = 2,
};

/// Payload for Events::ModalShow — request to display a confirmation card.
struct ModalShowPayload {
    std::string title;
    std::string body;       ///< Markdown-formatted description
    std::string tool_name;  ///< Tool requesting permission (empty = generic)
    /// Called on the main loop thread when the user dismisses the modal.
    std::function<void(ModalResult)> callback;
};

/// Payload for Events::ModalHide — result from a dismissed modal.
struct ModalHidePayload {
    ModalResult result{ModalResult::Deny};
};

/// Payload for Events::DemonDirty — demon panel tick / update.
struct DemonDirtyPayload {
    std::string demon_id;   ///< Which demon changed ("" = full refresh)
};

/// Payload for Events::UserMessage — a message the user just submitted.
struct UserMessagePayload {
    std::string text;   ///< The user's submitted text (non-empty guaranteed by factory)
};

/// Payload for Events::StreamDone — signals that the assistant turn has ended.
struct StreamDonePayload {
    std::string role;       ///< Always "assistant"; reserved for future roles
    bool        had_error;  ///< true if run_turn returned an error result
};

/// Payload for Events::MessageAppended — a completed message appended to history.
///
/// Emitted by Conversation::run_turn() (worker thread) after each tool-call
/// assistant message and each tool-result message is persisted.  ChatView
/// handles this by calling append_message() to display the message inline.
struct MessageAppendedPayload {
    /// The role string: "assistant" (for tool-call messages) or "tool".
    std::string role;
    /// The tool name — set when role=="tool" (from Message::tool_name).
    std::string tool_name;
    /// The message content body (tool result text or empty for pure tool-call messages).
    std::string content;
    /// Whether the tool call raised an error (from Message::is_error).
    bool is_error{false};
};

/// Payload for Events::ToolRunning — a tool call is about to be dispatched.
///
/// Emitted by Conversation::run_turn() (worker thread) before each
/// ToolCallOrchestrator::dispatch_all() call.  InputBar handles this by
/// calling set_running_tool() to show the indicator in the status row.
struct ToolRunningPayload {
    /// The name of the first tool being dispatched (e.g. "Bash", "Read").
    std::string tool_name;
    /// One-line preview of the first tool's primary argument, truncated to ~80
    /// chars.  Used by ChatView to populate the "L <arg>" preview line.
    /// Empty when no meaningful first argument is available.
    std::string args_summary;
    /// Total number of tool calls accumulating in this batch (1-based).
    /// Used by ChatView to build "Reading 1 file, writing 1 file, …" summaries.
    int tool_count{1};
};

/// Payload for Events::ToolDone — all tool calls for this iteration completed.
///
/// Emitted by Conversation::run_turn() after dispatch_all() returns.
/// InputBar handles this by clearing the running_tool indicator.
struct ToolDonePayload {};

/// Payload for Events::ThinkingStarted — first reasoning_content chunk arrived.
///
/// Emitted by Conversation::run_turn() (worker thread) the first time a
/// delta.reasoning_content chunk arrives in a streaming turn.  InputBar
/// handles this by setting thinking_ = true to show "· thinking..." in the
/// status row.
struct ThinkingStartedPayload {};

/// Payload for Events::ThinkingStopped — reasoning phase ended.
///
/// Emitted by Conversation::run_turn() when the first content chunk arrives
/// after reasoning_content, or when finish_reason is seen while still in the
/// reasoning phase.  InputBar handles this by clearing thinking_ = false.
struct ThinkingStoppedPayload {};

/// Payload for Events::QuestionShow — request to display an AskUserQuestion card.
///
/// Posted from the worker thread (inside AskUserQuestion::run()) before blocking.
/// The UI thread's QuestionCard::OnEvent resolves the question and fires a
/// QuestionResolved event; it then invokes callback on the UI thread.
struct QuestionShowPayload {
    /// Short chip label shown at the top of the card (max ~12 chars).
    std::string header;
    /// Bold question text displayed as the card heading.
    std::string question;
    /// When true the user may select more than one option.
    bool multi_select{false};
    /// Option labels shown in the selection list (2–4 items typical).
    std::vector<std::string> labels;
    /// Parallel dim-text descriptions for each label (may be empty or shorter
    /// than labels — unmatched positions are rendered as empty).
    std::vector<std::string> descriptions;
    /// When true an extra "Type something." free-form entry is appended.
    bool allow_freeform{false};
    /// When true an extra "Chat about this" escape-hatch entry is appended.
    bool allow_escape_hatch{false};
    /// Invoked on the UI thread when the user dismisses the card (resolved or
    /// cancelled).  Must not be null.
    std::function<void(const struct QuestionResolvedPayload&)> callback;
};

/// Payload for Events::QuestionResolved — result from a dismissed QuestionCard.
///
/// Posted by the UI thread (QuestionCard::OnEvent) when the user makes a
/// selection or presses Esc.  The worker thread waiting in
/// AskUserQuestion::run() receives this via the callback stored in
/// QuestionShowPayload::callback.
struct QuestionResolvedPayload {
    /// Labels the user selected (empty if cancelled == true).
    std::vector<std::string> chosen_labels;
    /// Free-form text entered when the user picked "Type something."; empty
    /// when allow_freeform was false or that option was not chosen.
    std::string freeform_text;
    /// True when the user picked the "Chat about this" escape-hatch entry.
    bool escape_hatch{false};
    /// True when the user pressed Esc without making a selection.
    bool cancelled{false};
};

/// Payload for Events::SpinnerTick — 1Hz wall-clock tick.
///
/// Posted by the spinner timer thread inside ChatView at 1Hz while a turn
/// is in flight.  ChatView::OnEvent handles this by incrementing
/// spinner_elapsed_s_ (UI thread only — no lock needed).
struct SpinnerTickPayload {};


// =============================================================================
// Factory functions  (call from ANY thread — thread-safe)
// =============================================================================

/// Create a Token event carrying @p text produced by @p agent_id.
[[nodiscard]] ftxui::Event make_token_event(std::string text,
                                             std::string agent_id = "");

/// Create an AgentsDirty event signalling that @p agent_id changed.
[[nodiscard]] ftxui::Event make_agents_dirty_event(std::string agent_id = "",
                                                    uint32_t    step     = 0,
                                                    uint32_t    tokens   = 0,
                                                    std::string status   = "");

/// Create a DemonDirty event for @p demon_id.
[[nodiscard]] ftxui::Event make_demon_dirty_event(std::string demon_id = "");

/// Create a StatusUpdate event reflecting a sidecar @p state change.
[[nodiscard]] ftxui::Event make_status_update_event(SidecarState state,
                                                     std::string  detail = "");

/// Create a StatusUpdate event carrying cumulative usage counters (TUI-FIX-T6 / A3).
///
/// Call from the worker thread after each terminal-chunk UsageDelta.  The UI
/// thread's InputBar::OnEvent extracts the payload and calls set_usage() —
/// never call set_usage() directly from the worker thread.
///
/// @param tokens    Cumulative session token count (prompt + completion, all turns).
/// @param cost_usd  Cumulative session cost in USD.
[[nodiscard]] ftxui::Event make_status_update_event_with_usage(uint32_t tokens,
                                                                double   cost_usd);

/// Create a ModalShow event that will invoke @p callback when dismissed.
[[nodiscard]] ftxui::Event make_modal_show_event(std::string title,
                                                  std::string body,
                                                  std::string tool_name,
                                                  std::function<void(ModalResult)> callback);

/// Create a ModalHide event carrying the user's @p result.
[[nodiscard]] ftxui::Event make_modal_hide_event(ModalResult result);

/// Create a UserMessage event carrying the text the user just submitted.
///
/// Post this from the UI thread (on_submit) before spawning the worker thread
/// so the chat view shows the user's message immediately.
[[nodiscard]] ftxui::Event make_user_message_event(std::string text);

/// Create a MessageAppended event carrying a completed message.
///
/// Post this from the worker thread (inside run_turn()) immediately after a
/// tool-call or tool-result message is appended to messages_ and persisted.
/// ChatView::OnEvent will call append_message() so the message is visible.
[[nodiscard]] ftxui::Event make_message_appended_event(std::string role,
                                                         std::string tool_name,
                                                         std::string content,
                                                         bool        is_error = false);

/// Create a StreamDone event signalling the assistant turn has finished.
///
/// Post this from the worker thread after run_turn() returns (success or error).
/// ChatView::OnEvent will commit the accumulated streaming buffer to history
/// and clear the streaming tail.
[[nodiscard]] ftxui::Event make_stream_done_event(bool had_error = false);

/// Create a ToolRunning event signalling that a tool call is about to be dispatched.
///
/// Post this from the worker thread (inside run_turn()) immediately before
/// ToolCallOrchestrator::dispatch_all() is called.  InputBar::OnEvent will
/// call set_running_tool(tool_name) to show "running: <tool>" in the status row.
/// @param tool_name   Name of the first tool (e.g. "Read", "Bash").
/// @param args_summary One-line preview of the first arg (path, command, etc.).
/// @param tool_count  Total number of tool calls in this batch.
[[nodiscard]] ftxui::Event make_tool_running_event(std::string tool_name,
                                                    std::string args_summary = "",
                                                    int         tool_count = 1);

/// Create a ToolDone event signalling that tool dispatch has completed.
///
/// Post this from the worker thread (inside run_turn()) immediately after
/// ToolCallOrchestrator::dispatch_all() returns.  InputBar::OnEvent will
/// call set_running_tool(std::nullopt) to clear the status row indicator.
[[nodiscard]] ftxui::Event make_tool_done_event();

/// Create a ThinkingStarted event signalling the first reasoning_content chunk.
///
/// Post this from the worker thread (inside run_turn()) the first time
/// delta.reasoning_content has a value in a streaming turn.  InputBar::OnEvent
/// will set thinking_ = true to show "· thinking..." in the status row.
[[nodiscard]] ftxui::Event make_thinking_started_event();

/// Create a ThinkingStopped event signalling the end of the reasoning phase.
///
/// Post this from the worker thread (inside run_turn()) when the first content
/// chunk arrives after reasoning, or when finish_reason is seen while still in
/// the reasoning phase.  InputBar::OnEvent will clear thinking_ = false.
[[nodiscard]] ftxui::Event make_thinking_stopped_event();

/// Create a SpinnerTick event for the 1Hz elapsed-counter tick.
///
/// Posted from the spinner timer thread (inside ChatView) once per second
/// while a turn is in flight.  ChatView::OnEvent will increment
/// spinner_elapsed_s_ so the elapsed display ticks even before tokens arrive.
[[nodiscard]] ftxui::Event make_spinner_tick_event();

/// Create a QuestionShow event carrying the full @p payload.
///
/// Post this from the worker thread (inside AskUserQuestion::run()) immediately
/// before blocking on the condition variable.  The UI thread's QuestionCard
/// will extract the payload via extract_question_show() and render the card.
/// @param payload  Must have a non-null callback; labels must be non-empty.
[[nodiscard]] ftxui::Event make_question_show_event(QuestionShowPayload payload);

/// Create a QuestionResolved event carrying the user's @p payload.
///
/// Post this from the UI thread (inside QuestionCard::OnEvent) when the user
/// makes a selection or presses Esc.  Workers waiting in
/// AskUserQuestion::run() listen for this event via the stored callback.
[[nodiscard]] ftxui::Event make_question_resolved_event(QuestionResolvedPayload payload);

// =============================================================================
// Extractor functions  (call from MAIN LOOP thread inside OnEvent)
//
// Each extractor returns std::nullopt if the event is not the expected type
// or if the payload has already been consumed.  Payloads are erased after
// extraction to avoid leaking memory on missed events.
// =============================================================================

[[nodiscard]] std::optional<TokenPayload>
    extract_token(const ftxui::Event& ev);

[[nodiscard]] std::optional<AgentsDirtyPayload>
    extract_agents_dirty(const ftxui::Event& ev);

[[nodiscard]] std::optional<DemonDirtyPayload>
    extract_demon_dirty(const ftxui::Event& ev);

[[nodiscard]] std::optional<StatusUpdatePayload>
    extract_status_update(const ftxui::Event& ev);

[[nodiscard]] std::optional<ModalShowPayload>
    extract_modal_show(const ftxui::Event& ev);

[[nodiscard]] std::optional<ModalHidePayload>
    extract_modal_hide(const ftxui::Event& ev);

[[nodiscard]] std::optional<UserMessagePayload>
    extract_user_message(const ftxui::Event& ev);

[[nodiscard]] std::optional<StreamDonePayload>
    extract_stream_done(const ftxui::Event& ev);

[[nodiscard]] std::optional<MessageAppendedPayload>
    extract_message_appended(const ftxui::Event& ev);

[[nodiscard]] std::optional<ToolRunningPayload>
    extract_tool_running(const ftxui::Event& ev);

[[nodiscard]] std::optional<ToolDonePayload>
    extract_tool_done(const ftxui::Event& ev);

[[nodiscard]] std::optional<ThinkingStartedPayload>
    extract_thinking_started(const ftxui::Event& ev);

[[nodiscard]] std::optional<ThinkingStoppedPayload>
    extract_thinking_stopped(const ftxui::Event& ev);

[[nodiscard]] std::optional<SpinnerTickPayload>
    extract_spinner_tick(const ftxui::Event& ev);

[[nodiscard]] std::optional<QuestionShowPayload>
    extract_question_show(const ftxui::Event& ev);

[[nodiscard]] std::optional<QuestionResolvedPayload>
    extract_question_resolved(const ftxui::Event& ev);

} // namespace batbox::tui
