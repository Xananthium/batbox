// include/batbox/tools/TaskOutputTool.hpp
//
// batbox::tools::TaskOutputTool — poll a running sub-agent for its current state.
//
// Contract (blueprints table, task CPP 5.28):
//
//   Tool name       : "TaskOutput"
//   Arguments:
//     agent_id (string, required) — the id returned by the Task tool
//
//   Returns JSON object:
//     {
//       "agent_id":     "<echo of input>",
//       "status":       "<queued|running|completed|cancelled|errored|unknown>",
//       "current_step": "<step label or empty string>",
//       "last_lines":   ["<line0>", ..., "<line4>"]   // up to 5 entries
//     }
//
//   Errors:
//     - missing or empty agent_id → ToolResult::error(...)
//     - ctx.cancel_token fired    → ToolResult::error("cancelled")
//
//   When agent_id is not found in the supervisor, status = "unknown" and
//   last_lines is empty; this is NOT an error — it allows polling after the
//   agent has been cleaned up.
//
//   Permission gate:
//     is_read_only()           = true   (read-only query; no side-effects)
//     requires_confirmation()  = false  (informational; no prompt)
//
// Blueprint contract: batbox::tools::TaskOutputTool (blueprints rows 16665–16667)

#pragma once

#include <batbox/tools/ITool.hpp>
#include <batbox/core/Json.hpp>

namespace batbox::agents {
class AgentSupervisor;
} // namespace batbox::agents

namespace batbox::tools {

// =============================================================================
// TaskOutputTool
// =============================================================================

/// Implements the "TaskOutput" tool: snapshot the current state and last 5
/// output lines of a running sub-agent by agent_id.
///
/// Delegates to AgentSupervisor::snapshot(), finds the entry matching the
/// requested agent_id, and returns a JSON summary.
///
/// Blueprint contract: batbox::tools::TaskOutputTool (blueprints rows 16665–16667)
class TaskOutputTool final : public ITool {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    /// Construct with a reference to the active AgentSupervisor.
    explicit TaskOutputTool(batbox::agents::AgentSupervisor& supervisor);

    // -------------------------------------------------------------------------
    // ITool identity
    // -------------------------------------------------------------------------

    /// Returns "TaskOutput".
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

    /// Call AgentSupervisor::snapshot(), find the entry whose id matches
    /// args["agent_id"], and return ToolResult::ok with a JSON body containing
    /// status + current_step + last_lines (up to 5).
    ///
    /// If no matching agent is found, returns status="unknown" (not an error).
    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override;

    // -------------------------------------------------------------------------
    // Permission gate hooks
    // -------------------------------------------------------------------------

    /// true — purely reads agent state; no mutation.
    [[nodiscard]] bool is_read_only() const override { return true; }

    /// false — informational query; no confirmation required.
    [[nodiscard]] bool requires_confirmation() const override { return false; }

private:
    batbox::agents::AgentSupervisor& supervisor_;
};

} // namespace batbox::tools
