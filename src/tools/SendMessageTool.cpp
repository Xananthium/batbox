// src/tools/SendMessageTool.cpp
//
// Implementation of batbox::tools::SendMessageTool.
//
// Blueprint contract (task CPP 5.28):
//   SendMessageTool::run — calls AgentSupervisor::enqueue_message(agent_id,
//   message) to queue a peer message for delivery on the target agent's next
//   turn boundary.
//
// Delivery semantics:
//   The message is queued atomically in the target agent's input queue.
//   It is delivered as a new user-turn the next time the agent's inference
//   loop returns and checks for pending input.
//   If the target agent has already terminated, the call is a no-op.

#include <batbox/tools/SendMessageTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>
#include <batbox/agents/AgentSupervisor.hpp>
#include <batbox/core/Json.hpp>

#include <string>

namespace batbox::tools {

// =============================================================================
// Construction
// =============================================================================

SendMessageTool::SendMessageTool(batbox::agents::AgentSupervisor& supervisor)
    : supervisor_(supervisor)
{}

// =============================================================================
// ITool identity
// =============================================================================

std::string_view SendMessageTool::name() const {
    return "SendMessage";
}

std::string_view SendMessageTool::description() const {
    return "Enqueue a UTF-8 message into a running sub-agent's input queue "
           "for delivery on its next turn boundary; enables peer-to-peer "
           "communication between agents.";
}

// =============================================================================
// OpenAI tool schema
// =============================================================================

Json SendMessageTool::schema_json() const {
    return Json{
        {"name",        "SendMessage"},
        {"description", description()},
        {"parameters",  Json{
            {"type",       "object"},
            {"properties", Json{
                {"agent_id", Json{
                    {"type",        "string"},
                    {"description", "The agent_id returned by the Task tool "
                                    "for the target agent to message."}
                }},
                {"message", Json{
                    {"type",        "string"},
                    {"description", "UTF-8 text to deliver to the target agent "
                                    "as a new user-turn message."}
                }}
            }},
            {"required",   Json::array({"agent_id", "message"})}
        }}
    };
}

// =============================================================================
// Execution
// =============================================================================

ToolResult SendMessageTool::run(const Json& args, ToolContext& ctx) {
    // --- Cancellation check (fast path) ---
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    // --- Validate agent_id ---
    if (!args.contains("agent_id") || !args["agent_id"].is_string()) {
        return ToolResult::error(
            "SendMessage: missing or non-string 'agent_id' argument");
    }

    const std::string agent_id = args["agent_id"].get<std::string>();

    if (agent_id.empty()) {
        return ToolResult::error(
            "SendMessage: 'agent_id' must not be empty");
    }

    // --- Validate message ---
    if (!args.contains("message") || !args["message"].is_string()) {
        return ToolResult::error(
            "SendMessage: missing or non-string 'message' argument");
    }

    const std::string message = args["message"].get<std::string>();

    if (message.empty()) {
        return ToolResult::error(
            "SendMessage: 'message' must not be empty");
    }

    // --- Second cancellation check before enqueue ---
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    // --- Delegate to AgentSupervisor::enqueue_message ---
    // Non-blocking: appends to the target agent's input queue and returns.
    // If the agent_id is unknown or the agent has terminated, this is a no-op.
    supervisor_.enqueue_message(agent_id, message);

    // --- Build result ---
    Json payload = {
        {"agent_id", agent_id},
        {"status",   "message_enqueued"}
    };

    return ToolResult::ok(payload.dump(2), std::move(payload));
}

} // namespace batbox::tools
