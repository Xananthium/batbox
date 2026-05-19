// include/batbox/tui/TuiCallbacks.hpp
// =============================================================================
// TuiCallbacks — POD bundling the 6 TUI-side callbacks wired in App::run().
//
// Design (Mike Acton / Karla-K4 directive):
//   Data abstraction over OO.  Six free-floating std::function fields that
//   previously travelled as individual constructor/set_on_* parameters are
//   grouped into one struct.  No constructor, no methods — aggregate init only.
//   Pass by const-reference; never copy.
//
// Lifetime contract:
//   Constructed AFTER screen_mgr + all card shared_ptrs are live in App::run().
//   Installed into Conversation BEFORE screen_mgr.run() enters the event loop.
//   All captured references (screen_mgr, plan_approval_card, question_card) must
//   outlive the Conversation and any worker threads that call run_turn().
//
// Thread safety:
//   The callbacks are invoked on worker threads (the same thread that calls
//   run_turn()).  Each callback must be thread-safe with respect to the objects
//   it captures.  ScreenManager::post_event() and ::post_token() are documented
//   as thread-safe.
//
// PEXT3 1.5/1.6 integration point:
//   plan_confirm_fn and askq_prompt_fn are the two slots that PEXT3 1.5/1.6
//   will replace with nuclear-short-circuit closures.  The POD shape is the
//   final installation target.
// =============================================================================

#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Forward-declare QuestionSpec so TuiCallbacks.hpp stays lightweight —
// callers that need the full type include AskUserQuestionTool.hpp themselves.
namespace batbox::tools { struct QuestionSpec; }

namespace batbox::tui {

/// POD grouping the 6 TUI-side callbacks wired in App::run().
/// All fields are default-constructible (empty std::function = callable check).
/// Use aggregate initialisation: TuiCallbacks cbs{ .plan_confirm_fn = ..., ... };
struct TuiCallbacks {
    // -------------------------------------------------------------------------
    // plan_confirm_fn
    //
    // Called by ExitPlanModeTool when the model completes a plan.
    // Returns true  → plan approved.
    // Returns false → plan rejected.
    //
    // In normal TUI mode: posts PlanApprovalShow, blocks on card, returns result.
    // PEXT3 1.5 nuclear: replaced with a closure that returns true immediately.
    // -------------------------------------------------------------------------
    std::function<bool(const std::string& /*plan_text*/)> plan_confirm_fn;

    // -------------------------------------------------------------------------
    // askq_prompt_fn
    //
    // Called by AskUserQuestionTool to present a multi-choice question.
    // Returns chosen label(s); empty = cancelled / no answer.
    //
    // In normal TUI mode: posts QuestionShow, blocks on card, returns choices.
    // PEXT3 1.6 nuclear: replaced with a closure that returns {} immediately.
    // -------------------------------------------------------------------------
    std::function<std::vector<std::string>(const batbox::tools::QuestionSpec& /*spec*/)> askq_prompt_fn;

    // -------------------------------------------------------------------------
    // on_delta_cb
    //
    // Called on the worker thread for each content token during streaming.
    // Forwards to ScreenManager::post_token() so ChatView updates in real time.
    // -------------------------------------------------------------------------
    std::function<void(std::string_view /*chunk*/)> on_delta_cb;

    // -------------------------------------------------------------------------
    // on_message_appended_cb
    //
    // Called on the worker thread after each tool-call or tool-result message
    // is appended during run_turn().  Forwards to make_message_appended_event.
    // -------------------------------------------------------------------------
    std::function<void(std::string_view /*role*/,
                       std::string_view /*tool_name*/,
                       std::string_view /*content*/,
                       bool             /*is_error*/)> on_message_appended_cb;

    // -------------------------------------------------------------------------
    // on_tool_running_cb
    //
    // Called immediately before ToolCallOrchestrator::dispatch_all() in run_turn().
    // Forwards to make_tool_running_event so InputBar shows the running indicator.
    // -------------------------------------------------------------------------
    std::function<void(std::string_view /*tool_name*/,
                       std::string_view /*args_summary*/,
                       int              /*tool_count*/)> on_tool_running_cb;

    // -------------------------------------------------------------------------
    // on_stream_terminal_cb
    //
    // Called immediately after ToolCallOrchestrator::dispatch_all() returns.
    // Clears the "tool running" indicator (posts make_tool_done_event).
    // Named on_stream_terminal_cb to match the PEXT3 1.4 blueprint contract;
    // internally wired via Conversation::set_on_tool_done_cb().
    // -------------------------------------------------------------------------
    std::function<void()> on_stream_terminal_cb;
};

} // namespace batbox::tui
