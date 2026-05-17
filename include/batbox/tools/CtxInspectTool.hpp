// include/batbox/tools/CtxInspectTool.hpp
//
// batbox::tools::CtxInspectTool — read-only context introspection tool.
//
// Tool name  : "CtxInspect"
// Slash alias: /context
//
// Returns a JSON snapshot of the current conversation context state so the
// model can introspect its own operating conditions without any side effects.
//
// Response fields (all present, never omitted):
//   message_count      (int)    — number of messages passed in via args
//   estimated_tokens   (int)    — caller-supplied token estimate for the window
//   model_context_limit(int)    — hard token limit for the model (from args)
//   pct_used           (double) — estimated_tokens / model_context_limit * 100.0
//                                 (clamped to [0.0, 100.0])
//   tool_call_count    (int)    — number of assistant messages with tool_calls
//   tools_available    (array)  — list of tool name strings passed in via args
//   cwd                (string) — ctx.cwd as a UTF-8 string
//   permission_mode    (string) — canonical lowercase name (default/plan/acceptedits/nuclear)
//   session_id         (string) — ctx.session_id (may be empty string)
//   agent_id           (string) — ctx.agent_id   (empty string = root conversation)
//
// Argument schema (all optional — missing fields default to 0 / empty):
//   "message_count"       (int)    — total messages in the active window
//   "estimated_tokens"    (int)    — current token estimate
//   "model_context_limit" (int)    — model's maximum context window (tokens)
//   "tool_call_count"     (int)    — assistant messages that invoked tools
//   "tools_available"     (array)  — tool name strings currently registered
//
// Plan-mode  : is_read_only() == true  — allowed unconditionally in Plan mode.
// Confirmation: requires_confirmation() == false — purely informational.
// Side effects: none.
//
// Blueprint contract: batbox::tools::CtxInspectTool (task CPP 5.24)

#pragma once

#include <batbox/tools/ITool.hpp>

namespace batbox::tools {

// =============================================================================
// CtxInspectTool
// =============================================================================

/// Implements the "CtxInspect" tool: returns a JSON snapshot of the current
/// conversation context state (token counts, model limits, message count, tool
/// call count, permission mode, session/agent identity) with no side effects.
class CtxInspectTool final : public ITool {
public:
    CtxInspectTool() = default;

    // -------------------------------------------------------------------------
    // ITool identity
    // -------------------------------------------------------------------------

    /// Returns "CtxInspect" — the stable tool name registered in ToolRegistry.
    [[nodiscard]] std::string_view name() const override;

    /// Returns a one-sentence description surfaced to the model in the schema.
    [[nodiscard]] std::string_view description() const override;

    // -------------------------------------------------------------------------
    // OpenAI tool schema
    // -------------------------------------------------------------------------

    /// Returns the full OpenAI tools[*].function JSON object for this tool.
    [[nodiscard]] Json schema_json() const override;

    // -------------------------------------------------------------------------
    // Execution
    // -------------------------------------------------------------------------

    /// Collect context state and return it as a JSON object.
    ///
    /// Reads from args:
    ///   "message_count"       (int, default 0)    — messages in the active window
    ///   "estimated_tokens"    (int, default 0)    — current token estimate
    ///   "model_context_limit" (int, default 0)    — model hard context limit
    ///   "tool_call_count"     (int, default 0)    — tool-calling assistant messages
    ///   "tools_available"     (array, default []) — registered tool names
    ///
    /// Reads from ctx:
    ///   cwd, mode, session_id, agent_id
    ///
    /// @returns ToolResult::ok(json_body) with body == compact JSON string and
    ///          structured_payload == the same data as a parsed Json object.
    ///          Never returns ToolResult::error (all inputs are optional with
    ///          safe defaults; ctx fields are always valid).
    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override;

    // -------------------------------------------------------------------------
    // Permission gate hooks
    // -------------------------------------------------------------------------

    /// true — CtxInspect is purely read-only; no state is mutated.
    [[nodiscard]] bool is_read_only() const override { return true; }

    /// false — no confirmation prompt needed for a read-only informational tool.
    [[nodiscard]] bool requires_confirmation() const override { return false; }
};

} // namespace batbox::tools
