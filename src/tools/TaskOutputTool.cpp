// src/tools/TaskOutputTool.cpp
//
// Implementation of batbox::tools::TaskOutputTool.
//
// Blueprint contract (task CPP 5.28):
//   TaskOutputTool::run — calls AgentSupervisor::snapshot(), finds the entry
//   matching args["agent_id"], and returns current status + last 5 output lines.
//
// When no matching agent is found (agent_id unknown or already cleaned up),
// returns status="unknown" with empty last_lines — this is not an error.

#include <batbox/tools/TaskOutputTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>
#include <batbox/agents/AgentSupervisor.hpp>
#include <batbox/core/Json.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace batbox::tools {

// =============================================================================
// Construction
// =============================================================================

TaskOutputTool::TaskOutputTool(batbox::agents::AgentSupervisor& supervisor)
    : supervisor_(supervisor)
{}

// =============================================================================
// ITool identity
// =============================================================================

std::string_view TaskOutputTool::name() const {
    return "TaskOutput";
}

std::string_view TaskOutputTool::description() const {
    return "Poll the current status and last output lines of a running sub-agent "
           "by agent_id; returns status, current step, and up to 5 recent output lines.";
}

// =============================================================================
// OpenAI tool schema
// =============================================================================

Json TaskOutputTool::schema_json() const {
    return Json{
        {"name",        "TaskOutput"},
        {"description", description()},
        {"parameters",  Json{
            {"type",       "object"},
            {"properties", Json{
                {"agent_id", Json{
                    {"type",        "string"},
                    {"description", "The agent_id returned by the Task tool "
                                    "for the agent to query."}
                }}
            }},
            {"required",   Json::array({"agent_id"})}
        }}
    };
}

// =============================================================================
// Execution
// =============================================================================

ToolResult TaskOutputTool::run(const Json& args, ToolContext& ctx) {
    // --- Cancellation check (fast path) ---
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    // --- Validate agent_id ---
    if (!args.contains("agent_id") || !args["agent_id"].is_string()) {
        return ToolResult::error(
            "TaskOutput: missing or non-string 'agent_id' argument");
    }

    const std::string agent_id = args["agent_id"].get<std::string>();

    if (agent_id.empty()) {
        return ToolResult::error(
            "TaskOutput: 'agent_id' must not be empty");
    }

    // --- Query supervisor for a snapshot of all agents ---
    const std::vector<batbox::agents::AgentSnapshot> snapshots =
        supervisor_.snapshot();

    // --- Find the entry matching agent_id ---
    // Use a linear scan — there are at most a few dozen agents at any time.
    const batbox::agents::AgentSnapshot* found = nullptr;
    for (const auto& snap : snapshots) {
        if (snap.id == agent_id) {
            found = &snap;
            break;
        }
    }

    if (found == nullptr) {
        // Agent not found — may have been cleaned up or never existed.
        // Return status="unknown" rather than an error so the model can reason.
        Json payload = {
            {"agent_id",     agent_id},
            {"status",       "unknown"},
            {"current_step", ""},
            {"last_lines",   Json::array()}
        };
        return ToolResult::ok(payload.dump(2), std::move(payload));
    }

    // --- Build last_lines array (at most 5 entries) ---
    Json last_lines = Json::array();
    const std::size_t max_lines = 5;
    const std::size_t line_count =
        std::min(found->last_5_lines.size(), max_lines);

    for (std::size_t i = 0; i < line_count; ++i) {
        last_lines.push_back(found->last_5_lines[i]);
    }

    // --- Build result payload ---
    Json payload = {
        {"agent_id",     found->id},
        {"status",       found->status},
        {"current_step", found->current_step},
        {"last_lines",   std::move(last_lines)}
    };

    return ToolResult::ok(payload.dump(2), std::move(payload));
}

} // namespace batbox::tools
