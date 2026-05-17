// src/tools/CtxInspectTool.cpp
//
// Implementation of batbox::tools::CtxInspectTool.
//
// See include/batbox/tools/CtxInspectTool.hpp for the full public API contract.

#include <batbox/tools/CtxInspectTool.hpp>
#include <batbox/permissions/PermissionMode.hpp>

#include <algorithm>
#include <string>

namespace batbox::tools {

// =============================================================================
// name
// =============================================================================

std::string_view CtxInspectTool::name() const {
    return "CtxInspect";
}

// =============================================================================
// description
// =============================================================================

std::string_view CtxInspectTool::description() const {
    return "Returns a JSON snapshot of the current conversation context state "
           "(token counts, model limits, message count, tool call count, "
           "permission mode, and session identity) without any side effects.";
}

// =============================================================================
// schema_json
// =============================================================================

Json CtxInspectTool::schema_json() const {
    return Json{
        {"name",        "CtxInspect"},
        {"description", "Returns a JSON snapshot of the current conversation context state "
                        "(token counts, model limits, message count, tool call count, "
                        "permission mode, and session identity) without any side effects."},
        {"parameters", Json{
            {"type",       "object"},
            {"properties", Json{
                {"message_count", Json{
                    {"type",        "integer"},
                    {"description", "Total number of messages currently in the active context window."},
                    {"default",     0}
                }},
                {"estimated_tokens", Json{
                    {"type",        "integer"},
                    {"description", "Estimated token count for the current context window."},
                    {"default",     0}
                }},
                {"model_context_limit", Json{
                    {"type",        "integer"},
                    {"description", "Hard token limit for the active model's context window."},
                    {"default",     0}
                }},
                {"tool_call_count", Json{
                    {"type",        "integer"},
                    {"description", "Number of assistant messages in the window that invoked tools."},
                    {"default",     0}
                }},
                {"tools_available", Json{
                    {"type",        "array"},
                    {"items",       Json{{"type", "string"}}},
                    {"description", "Names of tool functions currently registered and available."},
                    {"default",     Json::array()}
                }}
            }},
            {"required", Json::array()}
        }}
    };
}

// =============================================================================
// run
// =============================================================================

ToolResult CtxInspectTool::run(const Json& args, ToolContext& ctx) {
    // ------------------------------------------------------------------
    // 1. Read caller-supplied context fields from args (all optional).
    // ------------------------------------------------------------------
    const int message_count =
        args.contains("message_count") && args["message_count"].is_number_integer()
            ? args["message_count"].get<int>()
            : 0;

    const int estimated_tokens =
        args.contains("estimated_tokens") && args["estimated_tokens"].is_number_integer()
            ? args["estimated_tokens"].get<int>()
            : 0;

    const int model_context_limit =
        args.contains("model_context_limit") && args["model_context_limit"].is_number_integer()
            ? args["model_context_limit"].get<int>()
            : 0;

    const int tool_call_count =
        args.contains("tool_call_count") && args["tool_call_count"].is_number_integer()
            ? args["tool_call_count"].get<int>()
            : 0;

    // Collect tools_available as an array of strings.
    Json tools_available = Json::array();
    if (args.contains("tools_available") && args["tools_available"].is_array()) {
        for (const auto& item : args["tools_available"]) {
            if (item.is_string()) {
                tools_available.push_back(item);
            }
        }
    }

    // ------------------------------------------------------------------
    // 2. Compute derived fields.
    // ------------------------------------------------------------------

    // pct_used: percentage of context window consumed; clamped to [0.0, 100.0].
    double pct_used = 0.0;
    if (model_context_limit > 0) {
        pct_used = (static_cast<double>(estimated_tokens)
                    / static_cast<double>(model_context_limit)) * 100.0;
        pct_used = std::max(0.0, std::min(100.0, pct_used));
    }

    // ------------------------------------------------------------------
    // 3. Read identity fields from ctx.
    // ------------------------------------------------------------------
    const std::string cwd_str      = ctx.cwd.string();
    const std::string mode_str{permissions::to_string(ctx.mode)};
    const std::string session_id   = ctx.session_id;
    const std::string agent_id     = ctx.agent_id;

    // ------------------------------------------------------------------
    // 4. Build the result payload.
    // ------------------------------------------------------------------
    Json payload{
        {"message_count",       message_count},
        {"estimated_tokens",    estimated_tokens},
        {"model_context_limit", model_context_limit},
        {"pct_used",            pct_used},
        {"tool_call_count",     tool_call_count},
        {"tools_available",     tools_available},
        {"cwd",                 cwd_str},
        {"permission_mode",     mode_str},
        {"session_id",          session_id},
        {"agent_id",            agent_id}
    };

    // The body is compact JSON so the model can parse or display it.
    return ToolResult::ok(payload.dump(), payload);
}

} // namespace batbox::tools
