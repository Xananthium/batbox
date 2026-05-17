// src/tools/VerifyPlanExecutionTool.cpp
//
// Implementation of batbox::tools::VerifyPlanExecutionTool.
//
// Advisory-only: inspects PlanMode state and compares the caller-supplied
// steps_completed list against the approved plan text.  Never mutates state.
//
// Blueprint contract: batbox::tools::VerifyPlanExecutionTool (task CPP 5.18)

#include <batbox/tools/VerifyPlanExecutionTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>
#include <batbox/conversation/PlanMode.hpp>
#include <batbox/core/Json.hpp>

#include <sstream>
#include <string>
#include <vector>

namespace batbox::tools {

// =============================================================================
// Construction
// =============================================================================

VerifyPlanExecutionTool::VerifyPlanExecutionTool(
    batbox::conversation::PlanMode& plan_mode)
    : plan_mode_(plan_mode)
{}

// =============================================================================
// ITool identity
// =============================================================================

std::string_view VerifyPlanExecutionTool::name() const {
    return "VerifyPlanExecution";
}

std::string_view VerifyPlanExecutionTool::description() const {
    return "Advisory post-execution check: compares the steps the assistant "
           "completed against the approved plan and returns a markdown checklist "
           "summary; read-only, never modifies plan state.";
}

// =============================================================================
// OpenAI tool schema
// =============================================================================

Json VerifyPlanExecutionTool::schema_json() const {
    return Json{
        {"name",        "VerifyPlanExecution"},
        {"description", "Advisory post-execution check: compares the steps the assistant "
                        "completed against the approved plan and returns a markdown checklist "
                        "summary; read-only, never modifies plan state."},
        {"parameters",  Json{
            {"type",       "object"},
            {"properties", Json{
                {"steps_completed", Json{
                    {"type",        "array"},
                    {"description", "List of plan step descriptions the assistant believes "
                                    "it has completed during this execution turn."},
                    {"items",       Json{{"type", "string"}}},
                    {"default",     Json::array()}
                }}
            }},
            {"required",   Json::array()}
        }}
    };
}

// =============================================================================
// Execution
// =============================================================================

ToolResult VerifyPlanExecutionTool::run(const Json& args, ToolContext& ctx) {
    // --- Cancellation check ---
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    // --- Read steps_completed from args (optional) ---
    std::vector<std::string> steps_completed;
    if (args.contains("steps_completed") && args["steps_completed"].is_array()) {
        for (const auto& item : args["steps_completed"]) {
            if (item.is_string()) {
                steps_completed.push_back(item.get<std::string>());
            }
        }
    }

    // --- Inspect PlanMode state ---
    const auto plan_state   = plan_mode_.state();
    const auto plan_id      = plan_mode_.plan_id();
    const auto& plan_text   = plan_mode_.plan_text();

    const std::string state_name{
        batbox::conversation::plan_state_name(plan_state)
    };

    // Build structured payload (always present regardless of state).
    Json payload;
    payload["plan_state"]       = state_name;
    payload["steps_completed"]  = steps_completed;

    if (plan_id > 0) {
        payload["plan_id"] = plan_id;
    } else {
        payload["plan_id"] = nullptr;
    }

    // --- Handle non-Approved states with advisory messages ---
    if (plan_state == batbox::conversation::PlanState::Planning) {
        const std::string advisory =
            "Advisory: plan not yet approved. Call ExitPlanMode to obtain user "
            "approval before verifying execution.";
        payload["advisory"] = advisory;
        return ToolResult::ok(advisory, std::move(payload));
    }

    if (plan_state == batbox::conversation::PlanState::Inactive) {
        const std::string advisory =
            "Advisory: no active plan. Call EnterPlanMode and ExitPlanMode to "
            "establish a plan before verifying execution.";
        payload["advisory"] = advisory;
        return ToolResult::ok(advisory, std::move(payload));
    }

    // --- Approved state: build checklist ---
    std::ostringstream body;
    body << "Plan execution check (plan_id=" << plan_id << "):\n";

    // Mark each step provided by the assistant as completed.
    for (const auto& step : steps_completed) {
        body << "  [x] " << step << "\n";
    }

    // If the plan_text contains content beyond the steps listed, note it.
    if (!plan_text.empty() && steps_completed.empty()) {
        body << "  [ ] (no steps provided — review plan text for remaining items)\n";
    }

    // Advisory summary line.
    std::string advisory;
    if (steps_completed.empty()) {
        advisory = "Advisory: no steps reported as completed; review the approved plan.";
    } else {
        advisory = "Advisory: " + std::to_string(steps_completed.size()) +
                   " step(s) marked complete. Review plan text for any remaining items.";
    }

    body << advisory;

    payload["advisory"]   = advisory;
    payload["plan_text"]  = plan_text;

    return ToolResult::ok(body.str(), std::move(payload));
}

} // namespace batbox::tools
