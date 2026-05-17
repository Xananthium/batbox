// include/batbox/tools/SendMessageTool.hpp
//
// batbox::tools::SendMessageTool — enqueue a peer message into a running agent.
//
// Contract (blueprints table, task CPP 5.28):
//
//   Tool name       : "SendMessage"
//   Arguments:
//     agent_id (string, required) — target agent id (from Task tool)
//     message  (string, required) — UTF-8 text to deliver
//
//   Returns JSON object:
//     {
//       "agent_id": "<echo of input>",
//       "status":   "message_enqueued"
//     }
//
//   Errors:
//     - missing or empty agent_id  → ToolResult::error(...)
//     - missing or empty message   → ToolResult::error(...)
//     - ctx.cancel_token fired     → ToolResult::error("cancelled")
//
//   Delivery semantics:
//     The message is queued on the target agent's input queue.  It is
//     delivered as a new user-turn on the target agent's next turn boundary
//     (after its current inference call returns).  If the target agent has
//     already terminated, the call is a no-op and still returns
//     status="message_enqueued" (not an error — the caller cannot always
//     observe race conditions).
//
//   Permission gate:
//     is_read_only()           = false  (modifies agent input queue)
//     requires_confirmation()  = false  (orchestration primitive; no prompt)
//
// Blueprint contract: batbox::tools::SendMessageTool (blueprints rows 16671–16673)

#pragma once

#include <batbox/tools/ITool.hpp>
#include <batbox/core/Json.hpp>

namespace batbox::agents {
class AgentSupervisor;
} // namespace batbox::agents

namespace batbox::tools {

// =============================================================================
// SendMessageTool
// =============================================================================

/// Implements the "SendMessage" tool: enqueue a UTF-8 message into the input
/// queue of the identified sub-agent for delivery on its next turn boundary.
///
/// Delegates to AgentSupervisor::enqueue_message(agent_id, message).
///
/// Blueprint contract: batbox::tools::SendMessageTool (blueprints rows 16671–16673)
class SendMessageTool final : public ITool {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    /// Construct with a reference to the active AgentSupervisor.
    explicit SendMessageTool(batbox::agents::AgentSupervisor& supervisor);

    // -------------------------------------------------------------------------
    // ITool identity
    // -------------------------------------------------------------------------

    /// Returns "SendMessage".
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

    /// Call AgentSupervisor::enqueue_message(args["agent_id"], args["message"])
    /// and return ToolResult::ok with status="message_enqueued".
    ///
    /// Polls ctx.cancel_token before the call.  The enqueue is non-blocking.
    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override;

    // -------------------------------------------------------------------------
    // Permission gate hooks
    // -------------------------------------------------------------------------

    /// false — modifies the target agent's input queue.
    [[nodiscard]] bool is_read_only() const override { return false; }

    /// false — orchestration primitive; no user confirmation required.
    [[nodiscard]] bool requires_confirmation() const override { return false; }

private:
    batbox::agents::AgentSupervisor& supervisor_;
};

} // namespace batbox::tools
