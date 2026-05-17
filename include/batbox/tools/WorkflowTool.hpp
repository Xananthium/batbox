// include/batbox/tools/WorkflowTool.hpp
//
// batbox::tools::WorkflowTool — ITool implementation that submits a DAG of
// agent steps to the Workflow engine via AgentSupervisor.
//
// Blueprint contract (task CPP 5.29, blueprints rows 16746–16748):
//   class WorkflowTool : public ITool
//   name() = "Workflow"
//   Accepts a steps JSON array describing the DAG; runs via Workflow::execute().
//   Returns workflow_id on success.
//
// Tool arguments (JSON object):
//   steps (array, required) — JSON array of step objects; each step has:
//     name       (string, required) — unique step name within the workflow
//     agent_name (string, required) — sub-agent type to spawn for this step
//     prompt     (string, required) — prompt delivered to the spawned agent;
//                                     may contain {{step_name.output}} placeholders
//     depends_on (array,  optional) — array of step names this step waits for
//   failure_policy (string, optional) — "stop_on_first" (default) | "continue_all"
//
// Returns JSON object on success:
//   {
//     "status":  "completed",
//     "outputs": { "<step_name>": "<output>", … }
//   }
//
// Returns ToolResult::error on cycle detection, unknown dependency, or step failure.
//
// Permission flags:
//   is_read_only()          = false  (spawns agent threads via AgentSupervisor)
//   requires_confirmation() = false  (orchestration primitive)

#pragma once

#include <batbox/tools/ITool.hpp>
#include <batbox/core/Json.hpp>

namespace batbox::agents {
class AgentSupervisor;
} // namespace batbox::agents

namespace batbox::tools {

// =============================================================================
// WorkflowTool
// =============================================================================

/// Implements the "Workflow" tool: builds a Workflow DAG from the supplied
/// steps specification and executes it through AgentSupervisor, respecting
/// the topological order and the 4-slot parallelism semaphore.
///
/// Blueprint contract: batbox::tools::WorkflowTool (blueprints rows 16746–16748)
class WorkflowTool final : public ITool {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    /// Construct with a reference to the active AgentSupervisor.
    explicit WorkflowTool(batbox::agents::AgentSupervisor& supervisor);

    // -------------------------------------------------------------------------
    // ITool identity
    // -------------------------------------------------------------------------

    /// Returns "Workflow".
    [[nodiscard]] std::string_view name() const override;

    /// Returns a one-sentence description for the OpenAI tool schema.
    [[nodiscard]] std::string_view description() const override;

    // -------------------------------------------------------------------------
    // OpenAI tool schema
    // -------------------------------------------------------------------------

    /// Returns the full tools[*].function JSON object.
    [[nodiscard]] Json schema_json() const override;

    // -------------------------------------------------------------------------
    // Execution
    // -------------------------------------------------------------------------

    /// Build and execute a Workflow DAG.
    ///
    /// args["steps"]          — required JSON array of step objects
    /// args["failure_policy"] — optional "stop_on_first" | "continue_all"
    ///
    /// @returns ToolResult::ok(json)   on success; body includes outputs map.
    ///          ToolResult::error(msg) on validation or execution failure.
    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override;

    // -------------------------------------------------------------------------
    // Permission gate hooks
    // -------------------------------------------------------------------------

    /// false — spawns agent threads.
    [[nodiscard]] bool is_read_only() const override { return false; }

    /// false — orchestration primitive; no user confirmation required.
    [[nodiscard]] bool requires_confirmation() const override { return false; }

private:
    batbox::agents::AgentSupervisor& supervisor_;
};

} // namespace batbox::tools
