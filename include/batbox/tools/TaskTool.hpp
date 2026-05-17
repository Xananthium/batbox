// include/batbox/tools/TaskTool.hpp
//
// batbox::tools::TaskTool — spawn a sub-agent via AgentSupervisor.
//
// Contract (blueprints table, task CPP 5.28):
//
//   Tool name       : "Task"
//   Arguments:
//     subagent_type (string, required) — identifies the agent spec to load
//     prompt        (string, required) — initial user-turn text for the agent
//     description   (string, optional) — one-line label shown in the TUI panel
//
//   Returns JSON object:
//     {
//       "agent_id":      "<opaque-uuid>",
//       "subagent_type": "<echo of input>",
//       "status":        "dispatched"
//     }
//
//   Errors:
//     - missing or empty subagent_type → ToolResult::error(...)
//     - missing or empty prompt        → ToolResult::error(...)
//     - ctx.cancel_token fired         → ToolResult::error("cancelled")
//
//   Permission gate:
//     is_read_only()           = false  (spawns threads; has side-effects)
//     requires_confirmation()  = false  (orchestration primitive; no prompt)
//
// Blueprint contract: batbox::tools::TaskTool (blueprints rows 16662–16664)

#pragma once

#include <batbox/tools/ITool.hpp>
#include <batbox/core/Json.hpp>

namespace batbox::agents {
class AgentSupervisor;
} // namespace batbox::agents

namespace batbox::tools {

// =============================================================================
// TaskTool
// =============================================================================

/// Implements the "Task" tool: spawn a named sub-agent with an initial prompt
/// and return its agent_id immediately.  The agent runs asynchronously.
///
/// Delegates to AgentSupervisor::spawn() with the AgentSpec resolved from
/// args["subagent_type"].
///
/// Blueprint contract: batbox::tools::TaskTool (blueprints rows 16662–16664)
class TaskTool final : public ITool {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    /// Construct with a reference to the active AgentSupervisor.
    ///
    /// The supervisor must outlive the tool (it is owned by the App).
    explicit TaskTool(batbox::agents::AgentSupervisor& supervisor);

    // -------------------------------------------------------------------------
    // ITool identity
    // -------------------------------------------------------------------------

    /// Returns "Task".
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

    /// Resolve the AgentSpec from args["subagent_type"], call
    /// AgentSupervisor::spawn(spec, prompt, ctx.agent_id, ctx.cancel_token),
    /// and return ToolResult::ok with a JSON body containing the agent_id.
    ///
    /// Returns promptly; the agent runs asynchronously on its own jthread.
    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override;

    // -------------------------------------------------------------------------
    // Permission gate hooks
    // -------------------------------------------------------------------------

    /// false — spawning agents has permanent side-effects.
    [[nodiscard]] bool is_read_only() const override { return false; }

    /// false — orchestration primitive; no user confirmation required.
    [[nodiscard]] bool requires_confirmation() const override { return false; }

private:
    batbox::agents::AgentSupervisor& supervisor_;
};

} // namespace batbox::tools
