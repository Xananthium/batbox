// include/batbox/tools/ExitPlanModeTool.hpp
//
// batbox::tools::ExitPlanModeTool — present the assembled plan to the user for
// approval, then transition PlanMode from Planning to Approved (or stay in
// Planning if the user rejects).
//
// Contract (CPP 5.18 blueprint):
//
//   Tool name       : "ExitPlanMode"
//   Arguments:
//     plan (string, required) — the full plan text to present to the user for
//                               review.  Markdown is accepted and displayed as-is.
//   is_read_only()  : false  (transitions state; side-effect on agent flow)
//   requires_confirmation() : false  (the tool IS the confirmation mechanism)
//
//   Behaviour:
//     1. Validates that state == Planning. If Inactive or Approved, returns error.
//     2. Presents the plan text to the user via ConfirmFn (TUI modal) or an
//        interactive stdin prompt (TTY fallback).
//     3a. User approves:
//         - Calls plan_mode_.approve(plan_text) → state transitions to Approved.
//         - Returns ToolResult::ok(confirmation) with plan_id in structured_payload.
//     3b. User rejects:
//         - Calls plan_mode_.reject() → state transitions back to Inactive.
//         - Returns ToolResult::ok(rejection_notice) — not an error, but signals
//           the model should revise its plan and call ExitPlanMode again.
//
//   Return body (approved):
//     "Plan approved (plan_id=N). Write-side tools are re-enabled for the next
//      execution turn."
//
//   Return body (rejected):
//     "Plan rejected by user. Revise the plan and call ExitPlanMode again with
//      an updated plan."
//
//   Return body (wrong state — Inactive):
//     ToolResult::error("ExitPlanMode: not in Planning state (currently Inactive). "
//                       "Call EnterPlanMode first.")
//
//   Return body (wrong state — Approved):
//     ToolResult::error("ExitPlanMode: not in Planning state (currently Approved). "
//                       "The plan is already approved; advance the turn to proceed.")
//
//   Structured payload (on approve):
//     { "plan_id": <uint32_t>, "plan_text": "<the approved plan text>" }
//
// TUI integration:
//   The constructor accepts an optional ConfirmFn callback:
//     using ConfirmFn = std::function<bool(const std::string& plan_text)>;
//   When wired, it is called instead of the stdin prompt to display a modal.
//   When null (default), the tool falls back to an interactive stdin Y/N prompt.
//
// Blueprint contract: batbox::tools::ExitPlanModeTool (task CPP 5.18)

#pragma once

#include <batbox/tools/ITool.hpp>
#include <batbox/conversation/PlanMode.hpp>

#include <functional>
#include <string>

namespace batbox::tools {

// =============================================================================
// ExitPlanModeTool
// =============================================================================

/// Implements the "ExitPlanMode" tool: takes a plan text argument, presents it
/// to the user for approval via a TUI modal or stdin prompt, and transitions
/// PlanMode to Approved (on accept) or Inactive (on reject).
class ExitPlanModeTool final : public ITool {
public:
    // -------------------------------------------------------------------------
    // ConfirmFn — TUI callback type
    // -------------------------------------------------------------------------

    /// Callback invoked to show the plan to the user and collect their decision.
    ///
    /// @param plan_text  The full plan text string.
    /// @returns true     if the user approved, false if they rejected.
    ///
    /// The callback MUST NOT throw; return false on any error or cancellation.
    using ConfirmFn = std::function<bool(const std::string& plan_text)>;

    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    /// Construct with a reference to the conversation's PlanMode instance.
    /// Uses stdin fallback for the confirmation prompt (errors in headless).
    /// @param plan_mode  The PlanMode instance owned by the Conversation Engine.
    explicit ExitPlanModeTool(batbox::conversation::PlanMode& plan_mode);

    /// Construct with PlanMode reference and a custom TUI confirmation callback.
    /// @param plan_mode   The PlanMode instance owned by the Conversation Engine.
    /// @param confirm_fn  TUI/test callback; must not be null when supplied.
    ExitPlanModeTool(batbox::conversation::PlanMode& plan_mode, ConfirmFn confirm_fn);

    // -------------------------------------------------------------------------
    // ITool identity
    // -------------------------------------------------------------------------

    /// Returns "ExitPlanMode".
    [[nodiscard]] std::string_view name() const override;

    /// Returns a one-sentence description surfaced to the model in the schema.
    [[nodiscard]] std::string_view description() const override;

    // -------------------------------------------------------------------------
    // OpenAI tool schema
    // -------------------------------------------------------------------------

    /// Returns the OpenAI tools[*].function JSON object for this tool.
    [[nodiscard]] Json schema_json() const override;

    // -------------------------------------------------------------------------
    // Execution
    // -------------------------------------------------------------------------

    /// Present the plan to the user and transition PlanMode based on their decision.
    ///
    /// @param args  Must contain "plan" (string): the full plan text.
    /// @param ctx   Per-dispatch context; cancel_token is polled before blocking.
    ///
    /// @returns ToolResult::ok(message)   on approve or reject (both are non-error;
    ///                                    see body strings in the contract above).
    ///          ToolResult::error(reason) on wrong state, missing args, or headless
    ///                                    mode without a wired ConfirmFn.
    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override;

    // -------------------------------------------------------------------------
    // Permission gate hooks
    // -------------------------------------------------------------------------

    /// false — ExitPlanMode transitions PlanMode state (side-effect on agent flow).
    [[nodiscard]] bool is_read_only() const override { return false; }

    /// false — this tool IS the user confirmation/approval mechanism; no outer
    /// confirmation prompt should gate it.
    [[nodiscard]] bool requires_confirmation() const override { return false; }

private:
    batbox::conversation::PlanMode& plan_mode_;
    ConfirmFn                        confirm_fn_;

    /// Prompt via stdin (terminal fallback). Returns true = approved, false = rejected.
    /// Caller must have verified stdin is a TTY before invoking.
    [[nodiscard]] static bool prompt_via_stdin(const std::string& plan_text);
};

} // namespace batbox::tools
