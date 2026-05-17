// src/tools/WorkflowTool.cpp
//
// Implementation of batbox::tools::WorkflowTool.
//
// Blueprint contract (task CPP 5.29, blueprints rows 16746–16748):
//   WorkflowTool::run — parses the steps JSON array, constructs a
//   batbox::agents::Workflow, sets the failure policy, and calls
//   Workflow::execute(supervisor, cancel_token).

#include <batbox/tools/WorkflowTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>
#include <batbox/agents/Workflow.hpp>
#include <batbox/agents/AgentSupervisor.hpp>
#include <batbox/core/Json.hpp>

#include <map>
#include <string>
#include <vector>

namespace batbox::tools {

// =============================================================================
// Construction
// =============================================================================

WorkflowTool::WorkflowTool(batbox::agents::AgentSupervisor& supervisor)
    : supervisor_(supervisor)
{}

// =============================================================================
// ITool identity
// =============================================================================

std::string_view WorkflowTool::name() const {
    return "Workflow";
}

std::string_view WorkflowTool::description() const {
    return "Execute a DAG of agent steps through the workflow engine; steps "
           "run in topological order with independent steps fanned out in parallel "
           "under the 4-slot agent semaphore.";
}

// =============================================================================
// OpenAI tool schema
// =============================================================================

Json WorkflowTool::schema_json() const {
    return Json{
        {"name",        "Workflow"},
        {"description", description()},
        {"parameters",  Json{
            {"type",       "object"},
            {"properties", Json{
                {"steps", Json{
                    {"type",        "array"},
                    {"description", "Ordered list of DAG step definitions."},
                    {"items", Json{
                        {"type", "object"},
                        {"properties", Json{
                            {"name",       Json{{"type", "string"},
                                               {"description", "Unique step name."}}},
                            {"agent_name", Json{{"type", "string"},
                                               {"description", "Sub-agent type to spawn."}}},
                            {"prompt",     Json{{"type", "string"},
                                               {"description", "Initial prompt; may contain "
                                                               "{{step_name.output}} tokens."}}},
                            {"depends_on", Json{{"type",  "array"},
                                               {"items", Json{{"type", "string"}}},
                                               {"description", "Names of prerequisite steps."}}}
                        }},
                        {"required", Json::array({"name", "agent_name", "prompt"})}
                    }}
                }},
                {"failure_policy", Json{
                    {"type",        "string"},
                    {"enum",        Json::array({"stop_on_first", "continue_all"})},
                    {"description", "Controls what happens when a step fails: "
                                    "'stop_on_first' (default) cancels remaining steps; "
                                    "'continue_all' runs all independent steps and collects errors."}
                }}
            }},
            {"required", Json::array({"steps"})}
        }}
    };
}

// =============================================================================
// Execution
// =============================================================================

ToolResult WorkflowTool::run(const Json& args, ToolContext& ctx) {
    // --- Cancellation check ---
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    // --- Validate steps array ---
    if (!args.contains("steps") || !args["steps"].is_array()) {
        return ToolResult::error(
            "Workflow: missing or non-array 'steps' argument");
    }

    const auto& steps_json = args["steps"];

    // --- Parse failure_policy ---
    batbox::agents::FailurePolicy policy = batbox::agents::FailurePolicy::StopOnFirst;
    if (args.contains("failure_policy") && args["failure_policy"].is_string()) {
        const std::string policy_str = args["failure_policy"].get<std::string>();
        if (policy_str == "continue_all") {
            policy = batbox::agents::FailurePolicy::ContinueAll;
        } else if (policy_str != "stop_on_first") {
            return ToolResult::error(
                "Workflow: 'failure_policy' must be \"stop_on_first\" or \"continue_all\"");
        }
    }

    // --- Build Workflow ---
    batbox::agents::Workflow wf(policy);

    for (std::size_t i = 0; i < steps_json.size(); ++i) {
        const auto& step_obj = steps_json[i];

        if (!step_obj.is_object()) {
            return ToolResult::error(
                "Workflow: each element of 'steps' must be a JSON object");
        }

        // name
        if (!step_obj.contains("name") || !step_obj["name"].is_string()) {
            return ToolResult::error(
                "Workflow: step at index " + std::to_string(i) +
                " is missing required string field 'name'");
        }
        const std::string step_name = step_obj["name"].get<std::string>();

        // agent_name
        if (!step_obj.contains("agent_name") || !step_obj["agent_name"].is_string()) {
            return ToolResult::error(
                "Workflow: step '" + step_name +
                "' is missing required string field 'agent_name'");
        }
        const std::string agent_name = step_obj["agent_name"].get<std::string>();

        // prompt
        if (!step_obj.contains("prompt") || !step_obj["prompt"].is_string()) {
            return ToolResult::error(
                "Workflow: step '" + step_name +
                "' is missing required string field 'prompt'");
        }
        const std::string prompt = step_obj["prompt"].get<std::string>();

        // depends_on (optional)
        std::vector<std::string> depends_on;
        if (step_obj.contains("depends_on")) {
            const auto& deps = step_obj["depends_on"];
            if (!deps.is_array()) {
                return ToolResult::error(
                    "Workflow: step '" + step_name +
                    "' field 'depends_on' must be a JSON array of strings");
            }
            for (const auto& dep : deps) {
                if (!dep.is_string()) {
                    return ToolResult::error(
                        "Workflow: step '" + step_name +
                        "' — each depends_on entry must be a string");
                }
                depends_on.push_back(dep.get<std::string>());
            }
        }

        wf.add_step(step_name, agent_name, prompt, std::move(depends_on));
    }

    // --- Second cancellation check before execution ---
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    // --- Execute ---
    // CancelToken is move-only; create a child token linked to ctx.cancel_token
    // so that cancellation of the parent propagates to the workflow's agents.
    auto [child_src, child_tok] = ctx.cancel_token.child();
    (void)child_src;  // source kept alive; child_tok fires when parent fires
    auto result = wf.execute(supervisor_, std::move(child_tok));

    if (!result.has_value()) {
        return ToolResult::error(
            "Workflow execution failed: " + result.error());
    }

    // --- Build success payload ---
    Json outputs_json = Json::object();
    for (const auto& [step_name, output] : result.value()) {
        outputs_json[step_name] = output;
    }

    Json payload = {
        {"status",  "completed"},
        {"outputs", std::move(outputs_json)}
    };

    return ToolResult::ok(payload.dump(2), std::move(payload));
}

} // namespace batbox::tools
