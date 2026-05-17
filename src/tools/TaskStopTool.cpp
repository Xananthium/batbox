// src/tools/TaskStopTool.cpp
//
// Implementation of batbox::tools::TaskStopTool.
//
// Blueprint contract (task CPP 5.28):
//   TaskStopTool::run — calls AgentSupervisor::cancel(agent_id) to signal
//   cooperative cancellation of the identified sub-agent.
//
// Cancellation is cooperative: the agent exits on its next checkpoint.
// If the agent has already terminated, the call is a no-op.  In both cases
// the tool returns status="cancel_requested" — not an error.

#include <batbox/tools/TaskStopTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>
#include <batbox/agents/AgentSupervisor.hpp>
#include <batbox/core/Json.hpp>

#include <string>

namespace batbox::tools {

// =============================================================================
// Construction
// =============================================================================

TaskStopTool::TaskStopTool(batbox::agents::AgentSupervisor& supervisor)
    : supervisor_(supervisor)
{}

// =============================================================================
// ITool identity
// =============================================================================

std::string_view TaskStopTool::name() const {
    return "TaskStop";
}

std::string_view TaskStopTool::description() const {
    return "Request cooperative cancellation of a running sub-agent by agent_id; "
           "the agent exits on its next cancellation checkpoint.";
}

// =============================================================================
// OpenAI tool schema
// =============================================================================

Json TaskStopTool::schema_json() const {
    return Json{
        {"name",        "TaskStop"},
        {"description", description()},
        {"parameters",  Json{
            {"type",       "object"},
            {"properties", Json{
                {"agent_id", Json{
                    {"type",        "string"},
                    {"description", "The agent_id returned by the Task tool "
                                    "for the agent to cancel."}
                }},
                {"reason", Json{
                    {"type",        "string"},
                    {"description", "Optional human-readable reason for "
                                    "cancellation (logged with the agent event)."}
                }}
            }},
            {"required",   Json::array({"agent_id"})}
        }}
    };
}

// =============================================================================
// Execution
// =============================================================================

ToolResult TaskStopTool::run(const Json& args, ToolContext& ctx) {
    // --- Cancellation check (fast path) ---
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    // --- Validate agent_id ---
    if (!args.contains("agent_id") || !args["agent_id"].is_string()) {
        return ToolResult::error(
            "TaskStop: missing or non-string 'agent_id' argument");
    }

    const std::string agent_id = args["agent_id"].get<std::string>();

    if (agent_id.empty()) {
        return ToolResult::error(
            "TaskStop: 'agent_id' must not be empty");
    }

    // --- Delegate to AgentSupervisor::cancel ---
    // Non-blocking: signals the stop_source and returns immediately.
    // If the agent has already terminated, this is a no-op.
    supervisor_.cancel(agent_id);

    // --- Build result ---
    Json payload = {
        {"agent_id", agent_id},
        {"status",   "cancel_requested"}
    };

    return ToolResult::ok(payload.dump(2), std::move(payload));
}

} // namespace batbox::tools
