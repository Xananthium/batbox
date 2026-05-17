// include/batbox/tools/EnterPlanModeTool.hpp
//
// batbox::tools::EnterPlanModeTool — transition the conversation to Planning state.
//
// Contract (CPP 5.18 blueprint):
//
//   Tool name       : "EnterPlanMode"
//   Arguments       : (none — empty object)
//   is_read_only()  : true  (safe to call while already in Plan mode)
//   requires_confirmation() : false
//
//   Behaviour:
//     1. If state == Inactive: calls plan_mode_.enter_plan() → state becomes Planning.
//        Write-side tools (Write, Edit, Bash, PowerShell, TodoWrite) are gated by
//        PlanMode::is_tool_allowed() from this point on.
//     2. If state == Planning: noop — returns a confirmation that Planning is active.
//     3. If state == Approved: returns ToolResult::error explaining that the user
//        must complete or advance the current approved plan first.
//
//   Return body (success):
//     "Plan mode active. Write-side tools are now blocked until the plan is
//      approved by the user with ExitPlanMode."
//
//   Return body (already Planning):
//     "Plan mode is already active."
//
//   Return body (error — Approved state):
//     ToolResult::error(
//         "Cannot enter plan mode: an approved plan is already in progress. "
//         "Complete the approved plan turn first.")
//
// TUI integration:
//   PlanMode is injected via the constructor. The Conversation Engine (C3) holds
//   the canonical PlanMode instance; tools receive a reference to it.
//
// Blueprint contract: batbox::tools::EnterPlanModeTool (task CPP 5.18)

#pragma once

#include <batbox/tools/ITool.hpp>
#include <batbox/conversation/PlanMode.hpp>

namespace batbox::tools {

// =============================================================================
// EnterPlanModeTool
// =============================================================================

/// Implements the "EnterPlanMode" tool: transitions the conversation's PlanMode
/// state machine from Inactive to Planning.  In Planning state, write-side tools
/// are blocked by the ToolRegistry dispatch gate until the plan is approved via
/// ExitPlanMode.
class EnterPlanModeTool final : public ITool {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    /// Construct with a reference to the conversation's PlanMode state machine.
    /// The reference must remain valid for the lifetime of this tool object.
    /// @param plan_mode  The PlanMode instance owned by the Conversation Engine.
    explicit EnterPlanModeTool(batbox::conversation::PlanMode& plan_mode);

    // -------------------------------------------------------------------------
    // ITool identity
    // -------------------------------------------------------------------------

    /// Returns "EnterPlanMode".
    [[nodiscard]] std::string_view name() const override;

    /// Returns a one-sentence description surfaced to the model in the schema.
    [[nodiscard]] std::string_view description() const override;

    // -------------------------------------------------------------------------
    // OpenAI tool schema
    // -------------------------------------------------------------------------

    /// Returns the OpenAI tools[*].function JSON object (no parameters required).
    [[nodiscard]] Json schema_json() const override;

    // -------------------------------------------------------------------------
    // Execution
    // -------------------------------------------------------------------------

    /// Transition the conversation to Planning state.
    ///
    /// @param args  Ignored (tool takes no arguments).
    /// @param ctx   Per-dispatch context; cancel_token is polled before work.
    ///
    /// @returns ToolResult::ok(message)   when state is Inactive (now Planning) or
    ///                                    already Planning (noop, informational).
    ///          ToolResult::error(reason) when state is Approved (cannot re-enter).
    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override;

    // -------------------------------------------------------------------------
    // Permission gate hooks
    // -------------------------------------------------------------------------

    /// true — EnterPlanMode is a planning-intent signal; it is safe to call
    /// even while already in Plan mode and never mutates file/process state.
    [[nodiscard]] bool is_read_only() const override { return true; }

    /// false — no confirmation prompt; the tool itself triggers plan mode entry.
    [[nodiscard]] bool requires_confirmation() const override { return false; }

private:
    batbox::conversation::PlanMode& plan_mode_;
};

} // namespace batbox::tools
