// include/batbox/tools/TaskStopTool.hpp
//
// batbox::tools::TaskStopTool — cancel a running sub-agent by agent_id.
//
// Contract (blueprints table, task CPP 5.28):
//
//   Tool name       : "TaskStop"
//   Arguments:
//     agent_id (string, required) — the id returned by the Task tool
//     reason   (string, optional) — human-readable reason for cancellation
//
//   Returns JSON object:
//     {
//       "agent_id": "<echo of input>",
//       "status":   "cancel_requested"
//     }
//
//   Errors:
//     - missing or empty agent_id → ToolResult::error(...)
//     - ctx.cancel_token fired    → ToolResult::error("cancelled")
//
//   Cancellation is cooperative: the agent exits on its next checkpoint.
//   If the agent is already completed, the call is a no-op and still returns
//   status="cancel_requested" (not an error).
//
//   Permission gate:
//     is_read_only()           = false  (signals stop_token; has side-effects)
//     requires_confirmation()  = false  (orchestration primitive; no prompt)
//
// Blueprint contract: batbox::tools::TaskStopTool (blueprints rows 16668–16670)

#pragma once

#include <batbox/tools/ITool.hpp>
#include <batbox/core/Json.hpp>

namespace batbox::agents {
class AgentSupervisor;
} // namespace batbox::agents

namespace batbox::tools {

// =============================================================================
// TaskStopTool
// =============================================================================

/// Implements the "TaskStop" tool: request cooperative cancellation of a
/// named sub-agent by calling AgentSupervisor::cancel(agent_id).
///
/// Blueprint contract: batbox::tools::TaskStopTool (blueprints rows 16668–16670)
class TaskStopTool final : public ITool {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    /// Construct with a reference to the active AgentSupervisor.
    explicit TaskStopTool(batbox::agents::AgentSupervisor& supervisor);

    // -------------------------------------------------------------------------
    // ITool identity
    // -------------------------------------------------------------------------

    /// Returns "TaskStop".
    [[nodiscard]] std::string_view name() const override;

    /// Returns a one-sentence description for the OpenAI tool schema.
    [[nodiscard]] std::string_view description() const override;

    // -------------------------------------------------------------------------
    // OpenAI tool schema
    // -------------------------------------------------------------------------

    /// Returns the full OpenAI tools[*].function JSON object.
    [[nodiscard]] Json schema_json() const override;

    // -------------------------------------------------------------------------
    // Execution
    // -------------------------------------------------------------------------

    /// Call AgentSupervisor::cancel(args["agent_id"]) and return
    /// ToolResult::ok with status="cancel_requested".
    ///
    /// Polls ctx.cancel_token before the call.  The operation itself is
    /// non-blocking — it signals the agent's stop_source and returns.
    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override;

    // -------------------------------------------------------------------------
    // Permission gate hooks
    // -------------------------------------------------------------------------

    /// false — cancellation signals a stop_token; has side-effects.
    [[nodiscard]] bool is_read_only() const override { return false; }

    /// false — orchestration primitive; no user confirmation required.
    [[nodiscard]] bool requires_confirmation() const override { return false; }

private:
    batbox::agents::AgentSupervisor& supervisor_;
};

} // namespace batbox::tools
