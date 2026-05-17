// src/tools/EnterPlanModeTool.cpp
//
// Implementation of batbox::tools::EnterPlanModeTool.
//
// State transitions performed:
//   Inactive  → Planning   via plan_mode_.enter_plan()
//   Planning  → Planning   noop (enter_plan is idempotent)
//   Approved  → error      cannot re-enter while a plan is approved
//
// Blueprint contract: batbox::tools::EnterPlanModeTool (task CPP 5.18)

#include <batbox/tools/EnterPlanModeTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>
#include <batbox/conversation/PlanMode.hpp>
#include <batbox/core/Json.hpp>

#include <string>

namespace batbox::tools {

// =============================================================================
// Construction
// =============================================================================

EnterPlanModeTool::EnterPlanModeTool(batbox::conversation::PlanMode& plan_mode)
    : plan_mode_(plan_mode)
{}

// =============================================================================
// ITool identity
// =============================================================================

std::string_view EnterPlanModeTool::name() const {
    return "EnterPlanMode";
}

std::string_view EnterPlanModeTool::description() const {
    return "Enters plan mode: transitions the conversation to Planning state, "
           "blocking all write-side tools (Write, Edit, Bash, PowerShell, "
           "TodoWrite) until the plan is reviewed and approved via ExitPlanMode.";
}

// =============================================================================
// OpenAI tool schema
// =============================================================================

Json EnterPlanModeTool::schema_json() const {
    return Json{
        {"name",        "EnterPlanMode"},
        {"description", "Enters plan mode: transitions the conversation to Planning state, "
                        "blocking all write-side tools (Write, Edit, Bash, PowerShell, "
                        "TodoWrite) until the plan is reviewed and approved via ExitPlanMode."},
        {"parameters",  Json{
            {"type",       "object"},
            {"properties", Json::object()},
            {"required",   Json::array()}
        }}
    };
}

// =============================================================================
// Execution
// =============================================================================

ToolResult EnterPlanModeTool::run(const Json& /*args*/, ToolContext& ctx) {
    // --- Cancellation check ---
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    const auto current_state = plan_mode_.state();

    // Approved state: cannot re-enter planning while a plan is in progress.
    if (current_state == batbox::conversation::PlanState::Approved) {
        return ToolResult::error(
            "Cannot enter plan mode: an approved plan is already in progress. "
            "Complete the approved plan turn first.");
    }

    // Planning state: already active — noop, return informational message.
    if (current_state == batbox::conversation::PlanState::Planning) {
        return ToolResult::ok("Plan mode is already active.");
    }

    // Inactive state: transition to Planning.
    // enter_plan() may throw PlanModeError, but should not reach Approved branch
    // since we checked above. Wrap any unexpected exception defensively.
    try {
        plan_mode_.enter_plan();
    } catch (const batbox::conversation::PlanModeError& e) {
        return ToolResult::error(
            std::string("EnterPlanMode: state transition failed: ") + e.what());
    } catch (...) {
        return ToolResult::error("EnterPlanMode: unexpected error during state transition.");
    }

    return ToolResult::ok(
        "Plan mode active. Write-side tools are now blocked until the plan is "
        "approved by the user with ExitPlanMode.");
}

} // namespace batbox::tools
