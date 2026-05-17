// include/batbox/tools/VerifyPlanExecutionTool.hpp
//
// batbox::tools::VerifyPlanExecutionTool — post-execution advisor that checks
// whether the assistant followed the approved plan.
//
// Contract (CPP 5.18 blueprint):
//
//   Tool name       : "VerifyPlanExecution"
//   Arguments:
//     steps_completed (array of strings, optional) — list of plan step
//         descriptions the assistant believes it has executed.  When omitted
//         or empty, the tool reports based solely on PlanMode state.
//   is_read_only()  : true  (advisory only; never mutates state)
//   requires_confirmation() : false
//
//   Behaviour:
//     The tool is purely advisory.  It inspects PlanMode::plan_text() and
//     compares it against the caller-supplied steps_completed list to produce
//     a human-readable checklist.  It never throws and never modifies state.
//
//     State cases:
//       Approved — normal post-plan check.  Compares steps_completed against
//                  the approved plan_text and returns a checklist.
//       Planning — advisory: plan not yet approved; returns an advisory notice
//                  rather than an error.
//       Inactive — no plan is active; returns an advisory notice.
//
//   Return body (Approved, steps provided):
//     A markdown-style checklist string:
//       "Plan execution check (plan_id=N):\n"
//       "  [x] Step: <step>\n"   for each step_completed entry
//       "  [ ] (no steps provided for remaining plan items)\n"
//         (if steps_completed is empty or plan_text has additional content)
//     Followed by: "Advisory: all listed steps marked complete."
//     OR:          "Advisory: N step(s) completed; review plan for remaining items."
//
//   Return body (Planning state):
//     "Advisory: plan not yet approved. Call ExitPlanMode to obtain user approval
//      before verifying execution."
//
//   Return body (Inactive state):
//     "Advisory: no active plan. Call EnterPlanMode and ExitPlanMode to establish
//      a plan before verifying execution."
//
//   Structured payload (always present):
//     {
//       "plan_id":         <uint32_t | null>,
//       "plan_state":      "Inactive" | "Planning" | "Approved",
//       "steps_completed": [<strings>],
//       "advisory":        "<summary string>"
//     }
//
// Blueprint contract: batbox::tools::VerifyPlanExecutionTool (task CPP 5.18)

#pragma once

#include <batbox/tools/ITool.hpp>
#include <batbox/conversation/PlanMode.hpp>

namespace batbox::tools {

// =============================================================================
// VerifyPlanExecutionTool
// =============================================================================

/// Implements the "VerifyPlanExecution" tool: a read-only post-execution advisor
/// that inspects the approved plan text and the caller-supplied completion list
/// to produce a markdown checklist advisory for the model.
class VerifyPlanExecutionTool final : public ITool {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    /// Construct with a reference to the conversation's PlanMode instance.
    /// The reference must remain valid for the lifetime of this tool object.
    /// @param plan_mode  The PlanMode instance owned by the Conversation Engine.
    explicit VerifyPlanExecutionTool(batbox::conversation::PlanMode& plan_mode);

    // -------------------------------------------------------------------------
    // ITool identity
    // -------------------------------------------------------------------------

    /// Returns "VerifyPlanExecution".
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

    /// Inspect PlanMode state and produce an advisory checklist.
    ///
    /// @param args  May contain "steps_completed" (array of strings).
    /// @param ctx   Per-dispatch context; cancel_token is polled at entry.
    ///
    /// @returns ToolResult::ok(advisory_body, payload) always — this tool never
    ///          returns ToolResult::error; wrong-state and missing-plan cases
    ///          are reported as advisory text in the ok body.
    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override;

    // -------------------------------------------------------------------------
    // Permission gate hooks
    // -------------------------------------------------------------------------

    /// true — VerifyPlanExecution is purely read-only; no state is mutated.
    [[nodiscard]] bool is_read_only() const override { return true; }

    /// false — no confirmation prompt needed for a read-only advisory tool.
    [[nodiscard]] bool requires_confirmation() const override { return false; }

private:
    batbox::conversation::PlanMode& plan_mode_;
};

} // namespace batbox::tools
