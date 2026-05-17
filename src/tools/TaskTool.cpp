// src/tools/TaskTool.cpp
//
// Implementation of batbox::tools::TaskTool.
//
// Blueprint contract (task CPP 5.28):
//   TaskTool::run — resolves AgentSpec from args["subagent_type"], calls
//   AgentSupervisor::spawn(spec, prompt, ctx.agent_id, ctx.cancel_token),
//   returns agent_id in a JSON ToolResult body.
//
// Forward-declare note:
//   AgentSupervisor (CPP 6.5) is referenced here by reference only; its
//   full implementation is wired in when CPP 6.5 lands.  Until then, the
//   tool is compiled but the AgentSupervisor reference must be provided by
//   the caller (App::init or a test fixture).

#include <batbox/tools/TaskTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>
#include <batbox/agents/AgentSupervisor.hpp>
#include <batbox/agents/AgentSpec.hpp>
#include <batbox/core/Json.hpp>

#include <string>

namespace batbox::tools {

// =============================================================================
// Construction
// =============================================================================

TaskTool::TaskTool(batbox::agents::AgentSupervisor& supervisor)
    : supervisor_(supervisor)
{}

// =============================================================================
// ITool identity
// =============================================================================

std::string_view TaskTool::name() const {
    return "Task";
}

std::string_view TaskTool::description() const {
    return "Spawn a named sub-agent with an initial prompt and return its "
           "agent_id immediately; the agent runs asynchronously on its own thread.";
}

// =============================================================================
// OpenAI tool schema
// =============================================================================

Json TaskTool::schema_json() const {
    return Json{
        {"name",        "Task"},
        {"description", description()},
        {"parameters",  Json{
            {"type",       "object"},
            {"properties", Json{
                {"subagent_type", Json{
                    {"type",        "string"},
                    {"description", "Identifier for the agent spec to load "
                                    "(e.g. \"senior-dev\", \"junior-dev\", "
                                    "\"qa-dev\").  Resolved to "
                                    "~/.batbox/agents/<subagent_type>.md."}
                }},
                {"prompt", Json{
                    {"type",        "string"},
                    {"description", "Initial user-turn text delivered to the "
                                    "agent as its first message."}
                }},
                {"description", Json{
                    {"type",        "string"},
                    {"description", "Optional one-line label shown in the TUI "
                                    "sub-agent panel for this agent instance."}
                }}
            }},
            {"required",   Json::array({"subagent_type", "prompt"})}
        }}
    };
}

// =============================================================================
// Execution
// =============================================================================

ToolResult TaskTool::run(const Json& args, ToolContext& ctx) {
    // --- Cancellation check (fast path) ---
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    // --- Validate subagent_type ---
    if (!args.contains("subagent_type") || !args["subagent_type"].is_string()) {
        return ToolResult::error(
            "Task: missing or non-string 'subagent_type' argument");
    }

    const std::string subagent_type =
        args["subagent_type"].get<std::string>();

    if (subagent_type.empty()) {
        return ToolResult::error(
            "Task: 'subagent_type' must not be empty");
    }

    // --- Validate prompt ---
    if (!args.contains("prompt") || !args["prompt"].is_string()) {
        return ToolResult::error(
            "Task: missing or non-string 'prompt' argument");
    }

    const std::string prompt = args["prompt"].get<std::string>();

    if (prompt.empty()) {
        return ToolResult::error(
            "Task: 'prompt' must not be empty");
    }

    // --- Resolve AgentSpec from subagent_type ---
    // AgentSpec::from_type() resolves ~/.batbox/agents/<subagent_type>.md or
    // falls back to a generic spec when no file exists.
    const batbox::agents::AgentSpec spec =
        batbox::agents::AgentSpec::from_type(subagent_type);

    // --- Second cancellation check before blocking spawn ---
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    // --- Delegate to AgentSupervisor::spawn ---
    // Returns immediately; agent runs asynchronously.
    // Create a child cancel token linked to the parent context.
    // The child fires when the parent fires (cascade), but can also be
    // cancelled independently by TaskStop.
    auto [child_src, child_tok] = ctx.cancel_token.child();
    (void)child_src; // supervisor_ takes ownership of child_src internally

    const std::string agent_id =
        supervisor_.spawn(spec, prompt, ctx.agent_id, std::move(child_tok));

    // --- Build result ---
    Json payload = {
        {"agent_id",      agent_id},
        {"subagent_type", subagent_type},
        {"status",        "dispatched"}
    };

    return ToolResult::ok(payload.dump(2), std::move(payload));
}

} // namespace batbox::tools
