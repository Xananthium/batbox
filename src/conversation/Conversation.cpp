// src/conversation/Conversation.cpp
// =============================================================================
// Implementation of batbox::conversation::Conversation (CPP 3.6 + CPP 3.7).
//
// CPP 3.6: user_message() + run_turn() no-tools path.
// CPP 3.7: run_turn() extended with the tool-call loop.
//          When registry_ and gate_ are both non-null, run_turn() drives a
//          ToolCallOrchestrator loop until finish_reason="stop" or the
//          k_max_tool_turns cap is reached.
// =============================================================================

#include <batbox/conversation/Conversation.hpp>
#include <batbox/conversation/PlanMode.hpp>
#include <batbox/conversation/SystemPrompt.hpp>
#include <batbox/conversation/ToolCallOrchestrator.hpp>

#include <batbox/conversation/Compactor.hpp>
#include <batbox/conversation/ContextWindow.hpp>
#include <batbox/conversation/Message.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Logging.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/inference/ChatRequest.hpp>
#include <batbox/inference/ChatResponse.hpp>
#include <batbox/inference/Provider.hpp>             // reasoning_tags_for_config (S10)
#include <batbox/inference/ReasoningAccumulator.hpp>  // unified reasoning channel (S10)
#include <batbox/permissions/PermissionGate.hpp>
#include <batbox/permissions/PermissionMode.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolRegistry.hpp>

#include <batbox/perf/PerfSnapshot.hpp>

#include <filesystem>
#include <mutex>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace batbox::conversation {

// =============================================================================
// Constants
// =============================================================================

/// Maximum number of tool-call loop iterations per run_turn() call.
/// Prevents infinite loops when a model repeatedly emits tool_calls.
/// Matches the acceptance criterion; can be made configurable via Config in
/// a future task.
static constexpr int k_max_tool_turns = 20;

// =============================================================================
// Constructor
// =============================================================================

Conversation::Conversation(batbox::inference::Client&                client,
                           batbox::session::SessionStore&            store,
                           const batbox::config::Config&             cfg,
                           std::filesystem::path                     working_dir,
                           std::function<void(std::string_view)>     on_delta_cb,
                           batbox::tools::ToolRegistry*              registry,
                           batbox::permissions::PermissionGate*      gate,
                           batbox::conversation::PlanMode*           plan_mode,
                           std::mutex*                               cfg_mutex)
    : client_(client)
    , store_(store)
    , cfg_(cfg)
    , cfg_mutex_(cfg_mutex)
    , registry_(registry)
    , gate_(gate)
    , plan_mode_(plan_mode)
    , working_dir_(working_dir.empty() ? std::filesystem::current_path()
                                       : std::move(working_dir))
    , session_id_()
    , messages_()
    , on_delta_cb_(std::move(on_delta_cb))
{}

// =============================================================================
// set_on_message_appended_cb()
// =============================================================================

void Conversation::set_on_message_appended_cb(
    std::function<void(std::string_view,
                       std::string_view,
                       std::string_view,
                       bool)> cb)
{
    on_message_appended_cb_ = std::move(cb);
}

// =============================================================================
// set_on_tool_running_cb()
// =============================================================================

void Conversation::set_on_tool_running_cb(
    std::function<void(std::string_view /*tool_name*/,
                       std::string_view /*args_summary*/,
                       int              /*tool_count*/)> cb)
{
    on_tool_running_cb_ = std::move(cb);
}

// =============================================================================
// set_on_tool_done_cb()
// =============================================================================

void Conversation::set_on_tool_done_cb(std::function<void()> cb)
{
    on_tool_done_cb_ = std::move(cb);
}

// =============================================================================
// set_on_reasoning_started_cb()
// =============================================================================

void Conversation::set_on_reasoning_started_cb(std::function<void()> cb)
{
    on_reasoning_started_cb_ = std::move(cb);
}

// =============================================================================
// set_on_reasoning_stopped_cb()
// =============================================================================

void Conversation::set_on_reasoning_stopped_cb(std::function<void()> cb)
{
    on_reasoning_stopped_cb_ = std::move(cb);
}

// =============================================================================
// set_on_usage_delta_cb()  (TUI-FIX-T6 / A3)
// =============================================================================

void Conversation::set_on_usage_delta_cb(
    std::function<void(uint32_t /*tokens*/, double /*cost_usd*/)> cb)
{
    on_usage_delta_cb_ = std::move(cb);
}

// =============================================================================
// user_message()
// =============================================================================

void Conversation::user_message(std::string_view text) {
    // Build the User message with auto-assigned UUID and timestamp.
    Message msg;
    msg.role    = Role::User;
    msg.content = std::string(text);

    // Ensure the session exists before we try to append to it.
    // We ignore the error here — if session creation fails the message is still
    // appended to the in-memory list.  Callers that need guaranteed persistence
    // should check run_turn()'s return value which propagates session errors.
    if (session_id_.empty()) {
        (void)ensure_session_started();
    }

    // Persist user message to session store (best-effort; error is silently
    // absorbed here — run_turn() is the authoritative error boundary).
    if (!session_id_.empty()) {
        (void)store_.append_message(session_id_, to_json(msg));
    }

    // TUI-FLOW-T3: record the submit timestamp so run_turn() can compute
    // first-token latency when the first streaming content delta arrives.
    batbox::perf::g_perf.set_first_token_ms(0);  // reset; run_turn sets it on first token
    submit_time_ = std::chrono::steady_clock::now();

    messages_.push_back(std::move(msg));
}

// =============================================================================
// run_turn()
// =============================================================================

Result<void> Conversation::run_turn(batbox::CancelToken ct) {
    auto logger = batbox::log::get("conversation");

    // ---- 0. Early cancellation check ----
    // Check before doing any work so that a pre-cancelled token returns
    // Err("cancelled") immediately without touching the session or inference.
    if (ct.is_cancelled()) {
        return batbox::Err(std::string("cancelled"));
    }

    // ---- PEXT2 3.4 (D-8): snapshot mutable Config fields under mutex ----
    //
    // cfg.api.default_model can be written on the FTXUI UI thread by /model
    // (via apply_live_model_mutation under cfg_mutex_) while run_turn() runs on
    // a detached worker thread.  cfg.api.api_key can also be mutated by ConfigTool.
    // Reading either field without the mutex is a data race under TSAN.
    //
    // Strategy: acquire cfg_mutex_ once here and copy the two fields we need for
    // the entire turn into const-string locals.  All downstream sites
    // (Compactor ctor, req.model, api_key in Client::stream_chat preflight overload)
    // use these snapshots instead of reading cfg_ directly.
    //
    // When cfg_mutex_ is nullptr (headless mode, tests) the snapshot reads cfg_
    // without locking — safe in single-threaded contexts.
    //
    // NOTE (coordination with PEXT2 3.1 / D-1): current_api_key is used directly
    // in the client_.stream_chat(preflight_body_str, current_api_key, ...) call
    // for iteration 0, satisfying both D-1 (single dump) and D-8 (race-free key).
    const std::string current_model = [&]() -> std::string {
        if (cfg_mutex_ != nullptr) {
            std::lock_guard<std::mutex> lk(*cfg_mutex_);
            return cfg_.api.default_model;
        }
        return cfg_.api.default_model;
    }();
    const std::string current_api_key = [&]() -> std::string {
        if (cfg_mutex_ != nullptr) {
            std::lock_guard<std::mutex> lk(*cfg_mutex_);
            return cfg_.api.api_key;
        }
        return cfg_.api.api_key;
    }();

    // ---- S10 (DIS-975): resolve the provider's inline reasoning-tag convention ----
    // Sourced from the S8 provider profile (reasoning_tags_for_config → the same
    // provider identity OpenAiCompatibleProvider::name() resolves).  The provider
    // identity does not change mid-turn, so resolve once.  Read under cfg_mutex_
    // for the same data-race reason as the model/api_key snapshots above.
    const batbox::inference::ReasoningTags reasoning_tags = [&]()
            -> batbox::inference::ReasoningTags {
        if (cfg_mutex_ != nullptr) {
            std::lock_guard<std::mutex> lk(*cfg_mutex_);
            return batbox::inference::reasoning_tags_for_config(cfg_);
        }
        return batbox::inference::reasoning_tags_for_config(cfg_);
    }();


    // ---- 1. Auto-compact pre-flight check (bytes/4 estimator, G9) ----
    //
    // Token estimation uses the bytes/4 heuristic keyed off the serialized
    // ChatRequest body size.  This is the same estimator used at run-time;
    // using the actual serialized request means tool schemas, the system prompt,
    // and message content are all included in the estimate.
    //
    // The context limit is resolved ONCE at Config::load() time and stored in
    // cfg_.api.default_model_ctx_len.  If it is still at its initial value (4096)
    // AND the model matches a known entry in ContextWindow, we use the table value.
    // This covers the case where the user did not set any BATBOX_CTX_LEN_* env vars.
    //
    // Effective threshold = ceil(ctx_len * auto_compact_at_pct / 100).
    // If post-compact estimate still exceeds threshold → Err (verbatim message).
    //
    // PEXT2 3.1 (D-1): the dump produced here for the bytes/4 estimate is
    // captured in preflight_body_str and reused as the wire request body for
    // tool-call loop iteration 0.  This eliminates the second Json::dump() that
    // Client::stream_chat previously performed inside the retry loop.
    // After compaction the post-compact dump is stored instead.
    std::string preflight_body_str;
    {
        // Determine the resolved context length for this turn.
        const std::size_t resolved_ctx_len = [&]() -> std::size_t {
            // Use the env-resolved value if it is non-trivial (not the 4096 fallback
            // that means "not set via env").  We distinguish by checking whether the
            // env override was set — this is encoded in whether default_model_ctx_len
            // differs from the ContextWindow built-in table.
            // Simpler approach: always use cfg_.api.default_model_ctx_len if > 0,
            // and trust that resolve_model_ctx_len() in Config.cpp already applied
            // the correct priority (per-model env → default env → 4096).
            // If it is exactly 4096, fall through to ContextWindow for the built-in table.
            const std::size_t env_len = cfg_.api.default_model_ctx_len;
            if (env_len > 4096) {
                return env_len;  // env override was applied
            }
            // Try the built-in model table via ContextWindow.
            const std::size_t table_len =
                batbox::conversation::ContextWindow::context_limit_for_model(
                    current_model);
            if (table_len > 0) {
                return table_len;
            }
            // env_len is the 4096 fallback; use it.
            return env_len;
        }();

        // Effective compact threshold (tokens).
        const int pct = cfg_.compact.auto_compact_at_pct;
        const std::size_t threshold =
            (resolved_ctx_len * static_cast<std::size_t>(pct) + 99) / 100; // ceil div

        // Build the ChatRequest now (also needed for step 2 below) and estimate
        // via bytes/4.  This is the G9-adopted path: serialize the full request
        // once and divide byte count by 4.  The ±20% slack is why the default
        // threshold is 80% not 95%.
        const bool is_planning = (plan_mode_ != nullptr) && plan_mode_->is_planning();
        const std::string sys_prompt_preflight =
            batbox::conversation::compose_system_prompt(is_planning, working_dir_);

        batbox::inference::ChatRequest preflight_req =
            build_chat_request(messages_, cfg_, registry_, sys_prompt_preflight);
        // PEXT2 3.4: use snapshotted model to avoid race with /model switch.
        preflight_req.model = current_model;
        // Serialize the request as JSON for the bytes/4 estimate.
        // PEXT2 4.1 (D-3): use to_wire_string() to skip the intermediate
        // nlohmann tree — avoids ~2000 allocations per preflight call.
        // PEXT2 3.1 (D-1): capture dump result; reused as wire body in tool loop iter 0.
        preflight_body_str.clear();
        preflight_body_str.reserve(4096);
        batbox::inference::to_wire_string(preflight_req, preflight_body_str);
        const std::size_t est = preflight_body_str.size() / 4;

        auto logger_cw = batbox::log::get("conversation");
        logger_cw->debug("ctx-budget: est={}tok threshold={}tok ctx_len={}tok pct={}%",
                         est, threshold, resolved_ctx_len, pct);

        if (est >= threshold) {
            // Attempt compaction.
            Compactor compactor{cfg_.compact.keep_last_n_turns_verbatim};
            auto [child_src, child_tok] = ct.child();
            try {
                auto compact_res = compactor.compact(messages_, client_,
                                                      std::move(child_tok));
                if (!compact_res) {
                    return batbox::Err(compact_res.error());
                }
                messages_ = std::move(compact_res.value());
            } catch (const batbox::CancelledException&) {
                return batbox::Err(std::string("cancelled"));
            }

            // Rebuild preflight_body_str after compaction so iteration 0 uses the compacted wire body.
            batbox::inference::ChatRequest post_compact_req =
                build_chat_request(messages_, cfg_, registry_, sys_prompt_preflight);
            // PEXT2 3.4: use snapshotted model after compaction rebuild.
            post_compact_req.model = current_model;
            // Re-serialize after compaction.
            // PEXT2 4.1 (D-3): use to_wire_string() here too — consistent with
            // the pre-compact path above; avoids an extra nlohmann tree build.
            // PEXT2 3.1 (D-1): after compaction, update preflight_body_str
            // so the wire body for iteration 0 reflects the compacted messages.
            preflight_body_str.clear();
            preflight_body_str.reserve(4096);
            batbox::inference::to_wire_string(post_compact_req, preflight_body_str);
        }
    }

    // ---- Ensure session started before the loop ----
    if (session_id_.empty()) {
        auto sess_res = ensure_session_started();
        if (!sess_res) {
            return batbox::Err(sess_res.error());
        }
    }

    // These variables are declared outside the loop so the final iteration's
    // values are available after the loop exits (for the stop-branch assistant
    // message construction).
    std::string                   accumulated_content;
    std::string                   finish_reason;
    batbox::inference::UsageDelta usage;

    // ---- Tool-call loop ----
    // Iterates until finish_reason != "tool_calls", or k_max_tool_turns reached.
    for (int tool_turn = 0; tool_turn <= k_max_tool_turns; ++tool_turn) {

        // -- Cancellation check at top of each iteration --
        if (ct.is_cancelled()) {
            return batbox::Err(std::string("cancelled"));
        }

        // Reset per-iteration accumulators.
        accumulated_content.clear();
        finish_reason.clear();
        usage = batbox::inference::UsageDelta{};
        // TUI-FLOW-T3: reset first-token guard for this loop iteration.
        first_token_recorded_ = false;
        // Track the first tool name and arg-summary seen in this streaming
        // turn for the on_tool_running_cb_ indicator.
        std::string first_tool_name_for_turn;
        std::string first_tool_args_buf;   // raw args JSON for first call
        int         tool_call_count = 0;   // total distinct tool calls accumulating
        // Reasoning-phase tracking for the thinking indicator (TUI-T15).
        // reasoning_phase_active: set when the first reasoning_content chunk arrives.
        // reasoning_stopped_fired: set when on_reasoning_stopped_cb_ has been fired
        //   to ensure it fires at most once per iteration.
        bool reasoning_phase_active = false;
        bool reasoning_stopped_fired = false;

        // S10 (DIS-975): per-iteration unified reasoning channel.  Separates the
        // visible (reasoning-free) text from the reasoning stream, merging the
        // structured reasoning_content field with inline <think>…</think> text
        // pulled out of content.  Constructed fresh each iteration because the
        // ThinkSplitter it owns is stateful and not reusable across streams.
        batbox::inference::ReasoningAccumulator reasoning_acc(reasoning_tags);

        // ---- 2. Build ChatRequest ----
        // Compose the system prompt for this turn.  The plan-mode prefix is
        // included only when the PlanMode state machine is in Planning state.
        // When plan_mode_ is nullptr (headless / sub-agent callers that do not
        // wire a PlanMode), the system prompt is always composed without the
        // plan-mode prefix.
        const bool is_planning = (plan_mode_ != nullptr) && plan_mode_->is_planning();
        const std::string sys_prompt =
            batbox::conversation::compose_system_prompt(is_planning, working_dir_);

        batbox::inference::ChatRequest req =
            build_chat_request(messages_, cfg_, registry_, sys_prompt);
        // PEXT2 3.4: override req.model with the snapshotted model (race-free).
        req.model = current_model;

        // Instantiate ToolCallOrchestrator for this streaming turn (only when
        // the registry and gate are configured).
        std::unique_ptr<ToolCallOrchestrator> orch;
        if (registry_ != nullptr && gate_ != nullptr) {
            orch = std::make_unique<ToolCallOrchestrator>(*registry_, *gate_);
        }

        // ---- 3. Stream chat ----
        // Vend a child token for this streaming call so the CancelSource is
        // kept alive for the duration of the call.  The child fires whenever
        // the parent ct fires, giving us linked cancellation.
        auto [stream_src, stream_tok] = ct.child();

        // PEXT2 3.1 (D-1): for iteration 0 reuse the body string captured by the
        // pre-flight token-estimate dump (preflight_body_str), eliminating the
        // second Json::dump() inside Client::stream_chat.  The pre-flight body is
        // byte-identical to what Client would build (same messages, same sys_prompt,
        // same stream=true + stream_options_include_usage=true defaults).
        // Provider quirk handling (vllm/ollama/llama-cpp) is delegated to the
        // string overload of stream_chat, which applies quirks internally if needed.
        //
        // For tool-call follow-up iterations (tool_turn > 0) the messages_ vector
        // has grown with tool results; use the ChatRequest overload which rebuilds
        // and serialises the body from the updated req.
        auto on_delta_fn = [&](const batbox::inference::StreamDelta& delta) {
                // S10 (DIS-975): route the delta through the unified reasoning
                // channel.  This isolates reasoning whether the model emitted it
                // as the structured reasoning_content field OR inline
                // <think>…</think> inside content, and returns ONLY the
                // reasoning-free visible text.  The wire layer (Client/SSE/retry)
                // is untouched — this is a transform over the delta stream.
                const std::size_t reasoning_before = reasoning_acc.reasoning().size();
                const std::string visible = reasoning_acc.accumulate(delta);
                const bool reasoning_grew =
                    reasoning_acc.reasoning().size() > reasoning_before;

                // TUI-T15: Detect the start of the reasoning phase.  Reasoning now
                // starts on the first reasoning text from EITHER source (structured
                // field or an inline think block) so the "· thinking..." indicator
                // fires for inline-tag models too.
                if (reasoning_grew && !reasoning_phase_active) {
                    reasoning_phase_active = true;
                    if (on_reasoning_started_cb_) {
                        on_reasoning_started_cb_();
                    }
                }

                // Emit the visible (reasoning-free) text.
                if (!visible.empty()) {
                    // TUI-T15: First VISIBLE content ends the reasoning phase.
                    // Fire on_reasoning_stopped_cb_ exactly once per iteration.
                    if (reasoning_phase_active && !reasoning_stopped_fired) {
                        reasoning_stopped_fired = true;
                        if (on_reasoning_stopped_cb_) {
                            on_reasoning_stopped_cb_();
                        }
                    }
                    // TUI-FLOW-T3: record first-token latency on the very first
                    // visible chunk of each streaming turn.
                    if (accumulated_content.empty() && !first_token_recorded_) {
                        first_token_recorded_ = true;
                        auto now = std::chrono::steady_clock::now();
                        int ms = static_cast<int>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                now - submit_time_).count());
                        batbox::perf::g_perf.set_first_token_ms(ms);
                        logger->info("[perf] first_token={}ms", ms);
                    }
                    accumulated_content += visible;
                    if (on_delta_cb_) {
                        on_delta_cb_(visible);
                    }
                }
                // Feed tool_call deltas to the orchestrator.
                // Capture the first tool name as it arrives for the running indicator.
                if (delta.tool_calls.has_value() && orch != nullptr) {
                    for (const auto& tc_delta : *delta.tool_calls) {
                        orch->accumulate(tc_delta);
                        // Capture first non-empty tool name for the status indicator.
                        if (first_tool_name_for_turn.empty() &&
                            tc_delta.name.has_value() && !tc_delta.name->empty()) {
                            first_tool_name_for_turn = *tc_delta.name;
                        }
                        // Count distinct tool-call indices (each distinct index is
                        // a separate tool call in the batch).  The index field is
                        // always present in the first delta for each call.
                        if (tc_delta.name.has_value() && !tc_delta.name->empty()) {
                            ++tool_call_count;
                        }
                        // Accumulate args for the first tool call (for preview line).
                        if (first_tool_name_for_turn == (tc_delta.name.has_value()
                                ? *tc_delta.name : first_tool_name_for_turn) &&
                            tc_delta.arguments_fragment.has_value() &&
                            first_tool_args_buf.size() < 200) {
                            first_tool_args_buf += *tc_delta.arguments_fragment;
                        }
                    }
                }
                // Capture finish reason.
                if (delta.finish_reason.has_value()) {
                    finish_reason = *delta.finish_reason;
                    // TUI-T15: If finish_reason arrives while still in the reasoning
                    // phase (model ended without emitting visible content), fire the
                    // stopped callback now so the indicator is always cleared.
                    if (reasoning_phase_active && !reasoning_stopped_fired) {
                        reasoning_stopped_fired = true;
                        if (on_reasoning_stopped_cb_) {
                            on_reasoning_stopped_cb_();
                        }
                    }
                }
                // Capture usage from the terminal chunk.
                if (delta.usage.has_value()) {
                    usage = *delta.usage;
                }
        };

        // PEXT2 3.1 (D-1): dispatch to the appropriate stream_chat overload.
        // Iteration 0 uses the pre-serialised preflight_body_str (captured from
        // the pre-flight token-estimate dump) to avoid a redundant Json::dump().
        // Subsequent iterations (tool_turn > 0) use the ChatRequest overload
        // which serialises the updated request body (new tool-result messages).
        auto stream_res =
            (tool_turn == 0)
                ? client_.stream_chat(preflight_body_str, current_api_key,
                                      on_delta_fn, std::move(stream_tok))
                : client_.stream_chat(req, on_delta_fn, std::move(stream_tok));
        // stream_src goes out of scope here; streaming is complete.

        if (!stream_res) {
            return batbox::Err(stream_res.error());
        }

        // S10 (DIS-975): flush the reasoning channel at end of stream.  Any
        // buffered partial-tag tail is drained — an UNCLOSED inline <think> block
        // becomes reasoning; trailing look-alike text becomes visible.  Emit any
        // final visible text the splitter was holding back, then expose the
        // turn's isolated reasoning for downstream consumers (notepad / future
        // distillation).  Last writer (final tool-turn) wins.
        {
            const std::string tail_visible = reasoning_acc.finish();
            if (!tail_visible.empty()) {
                if (reasoning_phase_active && !reasoning_stopped_fired) {
                    reasoning_stopped_fired = true;
                    if (on_reasoning_stopped_cb_) {
                        on_reasoning_stopped_cb_();
                    }
                }
                accumulated_content += tail_visible;
                if (on_delta_cb_) {
                    on_delta_cb_(tail_visible);
                }
            }
            if (reasoning_acc.has_reasoning()) {
                last_reasoning_ = reasoning_acc.reasoning();
            }
        }

        // Prefer usage from the streaming return value.
        usage = stream_res.value();

        // A3 / TUI-FIX-T6: accumulate session usage and fire callback on the
        // terminal UsageDelta (once per sub-turn / streaming call).
        // The callback crosses to the UI thread via post_event — never calls
        // InputBar::set_usage() directly (UI-thread only by contract).
        if ((usage.total_tokens > 0 || usage.prompt_tokens > 0) && on_usage_delta_cb_) {
            session_tokens_   += static_cast<uint32_t>(
                (usage.total_tokens > 0) ? usage.total_tokens
                                         : usage.prompt_tokens + usage.completion_tokens);
            session_cost_usd_ += usage.cost_usd;
            on_usage_delta_cb_(session_tokens_, session_cost_usd_);
        }

        // TUI-FLOW-T3: log all three perf counters at INFO after each streaming
        // turn completes.  stream_to_paint_ms and frame_ms are updated by
        // ChatView::OnRender() concurrently; reads here are relaxed / best-effort.
        {
            auto snap = batbox::perf::g_perf.snapshot();
            logger->info("[perf] first_token={}ms stream_to_paint={}ms frame={}ms",
                         snap.first_token_ms, snap.stream_to_paint_ms, snap.frame_ms);
        }

        // ---- 4. Check finish_reason ----
        if (finish_reason == "tool_calls") {
            // ---- 4a. Guard: no registry/gate ----
            if (registry_ == nullptr || gate_ == nullptr || orch == nullptr) {
                return batbox::Err(std::string(
                    "tool_calls: no registry/gate configured"));
            }

            // ---- 4b. Check loop cap ----
            if (tool_turn >= k_max_tool_turns) {
                logger->warn("Conversation::run_turn: tool-call loop cap ({}) "
                             "reached; forcing stop after last assistant chunk",
                             k_max_tool_turns);
                // Exit loop — will fall through to stop-branch finalisation.
                break;
            }

            // ---- 4c. Build ToolContext ----
            // Vend a child cancel token for the dispatch context so tools
            // respect the same cancellation signal.
            auto [ctx_src, ctx_tok] = ct.child();

            batbox::tools::ToolContext ctx;
            ctx.cwd          = working_dir_;
            // PEXT3 1.6: read live permission mode from gate_ so nuclear mode
            // is correctly reflected in ctx.mode.  PermissionGate::ask() uses
            // ctx.mode as authoritative (overrides mode_ at line 119 of ask()).
            ctx.mode         = (gate_ != nullptr)
                                   ? gate_->current_mode()
                                   : batbox::permissions::PermissionMode::Default;
            ctx.session_id   = session_id_;
            ctx.cancel_token = std::move(ctx_tok);

            // ---- 4d. Emit tool-running indicator ----
            // Notify the UI that a tool is about to run so the status row can
            // update to show "running: <tool>".  first_tool_name_ was captured
            // from the first ToolCallDelta with a non-empty name during streaming.
            if (on_tool_running_cb_) {
                // Extract a one-line args preview from the first tool's JSON buffer.
                // We do a lightweight scan: look for the first string value in the
                // JSON object (the primary path/command argument).
                std::string args_preview;
                {
                    const std::string& buf = first_tool_args_buf;
                    // Find first '"' after the first ':' in the JSON object.
                    auto colon = buf.find(':');
                    if (colon != std::string::npos) {
                        auto q1 = buf.find('"', colon + 1);
                        if (q1 != std::string::npos) {
                            auto q2 = buf.find('"', q1 + 1);
                            if (q2 != std::string::npos && q2 > q1) {
                                args_preview = buf.substr(q1 + 1, q2 - q1 - 1);
                                // Truncate to 80 chars.
                                if (args_preview.size() > 80) {
                                    args_preview = args_preview.substr(0, 77) + "...";
                                }
                            }
                        }
                    }
                }
                int final_count = (tool_call_count > 0) ? tool_call_count : 1;
                on_tool_running_cb_(first_tool_name_for_turn,
                                    args_preview,
                                    final_count);
            }

            // ---- 4d. Dispatch all accumulated tool calls ----
            std::vector<Message> tool_messages = orch->dispatch_all(ctx);
            // ctx_src goes out of scope here; dispatch is complete.

            // ---- 4d2. Clear tool-running indicator ----
            if (on_tool_done_cb_) {
                on_tool_done_cb_();
            }

            // ---- 4e. Build assistant Message with tool_calls ----
            // Reconstruct the tool_calls vector from the returned Tool messages
            // correlation ids and names (these mirror the original calls).
            Message asst_tc;
            asst_tc.role    = Role::Assistant;
            asst_tc.content = accumulated_content; // often empty for tool-call turns

            if (!tool_messages.empty()) {
                std::vector<ToolCall> tcs;
                tcs.reserve(tool_messages.size());
                for (const auto& tm : tool_messages) {
                    ToolCall tc;
                    tc.id        = tm.tool_call_id.value_or("");
                    tc.name      = tm.tool_name.value_or("");
                    // Arguments are not reflected back on Tool messages;
                    // use an empty JSON object — the wire format allows this
                    // for the round-trip assistant message.
                    tc.arguments = batbox::Json::object();
                    tcs.push_back(std::move(tc));
                }
                asst_tc.tool_calls = std::move(tcs);
            }

            // Attach usage delta to the sub-turn assistant message.
            if (usage.total_tokens > 0 || usage.prompt_tokens > 0) {
                batbox::conversation::UsageDelta ud;
                ud.prompt_tokens     = usage.prompt_tokens;
                ud.completion_tokens = usage.completion_tokens;
                ud.total_tokens      = usage.total_tokens;
                ud.cost_usd          = usage.cost_usd;
                asst_tc.usage = ud;
            }

            // ---- 4f. Persist assistant message ----
            auto append_asst = store_.append_message(session_id_, to_json(asst_tc));
            if (!append_asst) {
                return batbox::Err(append_asst.error());
            }
            messages_.push_back(std::move(asst_tc));

            // Notify UI: a tool-call message was appended.
            if (on_message_appended_cb_) {
                const Message& appended_asst = messages_.back();
                // Build a summary of tool names for the content field.
                std::string tool_names_summary;
                if (appended_asst.tool_calls.has_value()) {
                    for (const auto& tc : *appended_asst.tool_calls) {
                        if (!tool_names_summary.empty()) tool_names_summary += ", ";
                        tool_names_summary += tc.name;
                    }
                }
                on_message_appended_cb_("assistant",
                                        tool_names_summary,
                                        appended_asst.content,
                                        /*is_error=*/false);
            }

            // ---- 4g. Persist and append each Tool message ----
            for (auto& tm : tool_messages) {
                auto append_tool = store_.append_message(session_id_, to_json(tm));
                if (!append_tool) {
                    return batbox::Err(append_tool.error());
                }
                messages_.push_back(std::move(tm));

                // Notify UI: a tool-result message was appended.
                if (on_message_appended_cb_) {
                    const Message& appended_tm = messages_.back();
                    const bool     tm_is_err   = appended_tm.is_error.value_or(false);
                    on_message_appended_cb_("tool",
                                            appended_tm.tool_name.value_or(""),
                                            appended_tm.content,
                                            tm_is_err);
                }
            }

            logger->debug("Conversation::run_turn: tool-call turn {} / {} "
                          "complete, looping back",
                          tool_turn + 1, k_max_tool_turns);

            // Loop back to the next inference request.
            continue;
        }

        // finish_reason is "stop", "length", "content_filter", or empty.
        // Normal stop — exit the loop.
        break;

    } // end tool-call loop

    // ---- 5. Finalise assistant message (stop branch) ----
    Message asst;
    asst.role    = Role::Assistant;
    asst.content = std::move(accumulated_content);
    if (usage.total_tokens > 0 || usage.prompt_tokens > 0) {
        batbox::conversation::UsageDelta ud;
        ud.prompt_tokens     = usage.prompt_tokens;
        ud.completion_tokens = usage.completion_tokens;
        ud.total_tokens      = usage.total_tokens;
        ud.cost_usd          = usage.cost_usd;
        asst.usage = ud;
    }

    // ---- 6. Persist assistant message ----
    auto append_res = store_.append_message(session_id_, to_json(asst));
    if (!append_res) {
        return batbox::Err(append_res.error());
    }

    messages_.push_back(std::move(asst));
    return {};  // Ok(void)
}

// =============================================================================
// restore()
// =============================================================================

Result<void> Conversation::restore(const batbox::session::SessionFile& sf) {
    messages_.clear();
    messages_.reserve(sf.messages.size());

    for (const auto& j : sf.messages) {
        try {
            messages_.push_back(from_json(j));
        } catch (const std::exception& e) {
            return batbox::Err(std::string("restore: failed to parse message: ")
                               + e.what());
        }
    }

    working_dir_ = sf.working_dir;
    session_id_  = sf.id.to_string();
    return {};
}

// =============================================================================
// Private helpers
// =============================================================================

Result<void> Conversation::ensure_session_started() {
    if (!session_id_.empty()) {
        return {};
    }
    auto res = store_.new_session(cfg_.api.default_model, working_dir_);
    if (!res) {
        return batbox::Err(res.error());
    }
    session_id_ = std::move(res.value());
    return {};
}

// =============================================================================
// start_session() — public eager session initialisation (TUI-T10 / UI-D11)
// =============================================================================
Result<void> Conversation::start_session() {
    return ensure_session_started();
}

batbox::inference::ChatRequest
Conversation::build_chat_request(const std::vector<Message>&      messages,
                                  const batbox::config::Config&    cfg,
                                  batbox::tools::ToolRegistry*     registry,
                                  const std::string&               system_prompt) {
    batbox::inference::ChatRequest req;
    req.model = cfg.api.default_model;

    if (cfg.api.max_tokens > 0) {
        req.max_tokens = cfg.api.max_tokens;
    }
    if (cfg.api.temperature >= 0.0) {
        req.temperature = cfg.api.temperature;
    }
    if (cfg.api.top_p > 0.0 && cfg.api.top_p <= 1.0) {
        req.top_p = cfg.api.top_p;
    }

    // Prepend the system prompt as the first WireMessage when provided.
    // The system message must be the first entry in the messages array so that
    // the model sees it before any conversation history.
    if (!system_prompt.empty()) {
        batbox::inference::WireMessage sys_msg;
        sys_msg.role    = "system";
        sys_msg.content = system_prompt;
        req.messages.push_back(std::move(sys_msg));
    }

    req.messages.reserve(req.messages.size() + messages.size());
    for (const auto& m : messages) {
        batbox::inference::WireMessage wm;
        wm.role = std::string(to_wire_role(m.role));

        // Content: present for most roles; assistant tool-call turns may
        // have empty content.
        if (!m.content.empty()) {
            wm.content = m.content;
        } else {
            wm.content = std::nullopt;
        }

        // Pass through tool_calls for assistant messages.
        if (m.tool_calls.has_value() && !m.tool_calls->empty()) {
            std::vector<batbox::inference::WireToolCall> wire_tcs;
            wire_tcs.reserve(m.tool_calls->size());
            for (const auto& tc : *m.tool_calls) {
                batbox::inference::WireToolCall wtc;
                wtc.id                 = tc.id;
                wtc.function.name      = tc.name;
                wtc.function.arguments = tc.arguments.dump();
                wire_tcs.push_back(std::move(wtc));
            }
            wm.tool_calls = std::move(wire_tcs);
        }

        // Correlation id for tool-result messages.
        if (m.tool_call_id.has_value()) {
            wm.tool_call_id = m.tool_call_id;
        }
        // Tool name for tool-result messages (goes into the "name" wire field).
        if (m.tool_name.has_value()) {
            wm.name = m.tool_name;
        }

        req.messages.push_back(std::move(wm));
    }

    // Populate tools from registry when available.
    if (registry != nullptr) {
        // available_tool_schemas() returns vector<Json> where each element is:
        //   { "type": "function", "function": { "name": "...", "description":
        //     "...", "parameters": {...} } }
        // We parse each into a ToolDef for the ChatRequest.
        const auto schemas = registry->available_tool_schemas();
        req.tools.reserve(schemas.size());
        for (const auto& schema_json : schemas) {
            batbox::inference::ToolDef td;
            td.type = schema_json.value("type", std::string("function"));
            if (schema_json.contains("function")) {
                const auto& fn = schema_json["function"];
                td.name        = fn.value("name", std::string{});
                td.description = fn.value("description", std::string{});
                if (fn.contains("parameters")) {
                    td.schema = fn["parameters"];
                }
            }
            req.tools.push_back(std::move(td));
        }
    }

    // When tools are present, explicitly request auto tool-choice so that
    // models like DeepSeek-V4-Flash, Qwen3, and Kimi-K2 use OpenAI-standard
    // tool_calls rather than falling back to delimiter-style output that
    // BatBox cannot parse.  When no tools are registered, omit the field
    // entirely — some servers reject an explicit "none" or empty object.
    if (!req.tools.empty()) {
        req.tool_choice = std::string{"auto"};
    }

    req.stream                       = true;
    req.stream_options_include_usage = true;

    return req;
}

// =============================================================================
// compose_system_prompt()
// =============================================================================

std::string Conversation::compose_system_prompt(bool plan_mode) const {
    return batbox::conversation::compose_system_prompt(plan_mode, working_dir_);
}

// =============================================================================
// clear_messages() — TUI-T3 (UI-D5/UI-D6)
// =============================================================================

void Conversation::clear_messages() noexcept {
    messages_.clear();
}

} // namespace batbox::conversation
