// src/tools/McpTool.cpp
//
// batbox::tools::McpTool — implementation.
//
// Routes model-supplied {server, method, params} directly through
// McpServerRegistry → IMcpTransport::request(), returning the raw JSON-RPC
// result as a text body or surfacing errors as ToolResult::error.
//
// Blueprint contract: batbox::tools::McpTool (blueprints table rows 16734–16736)

#include <batbox/tools/McpTool.hpp>

#include <batbox/core/Json.hpp>
#include <batbox/core/Logging.hpp>
#include <batbox/mcp/IMcpTransport.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>

#include <string>
#include <string_view>

namespace batbox::tools {

// =============================================================================
// Construction
// =============================================================================

McpTool::McpTool(batbox::mcp::McpServerRegistry& registry)
    : registry_(registry)
{}

// =============================================================================
// ITool identity
// =============================================================================

std::string_view McpTool::name() const {
    return "MCP";
}

std::string_view McpTool::description() const {
    return "Call any JSON-RPC method on a configured MCP server; "
           "supply {server, method, params} and receive the raw result.";
}

Json McpTool::schema_json() const {
    return Json{
        {"name",        "MCP"},
        {"description",
         "Call any JSON-RPC method on a configured MCP server; "
         "supply {server, method, params} and receive the raw result."},
        {"parameters", Json{
            {"type",       "object"},
            {"properties", Json{
                {"server", Json{
                    {"type",        "string"},
                    {"description",
                     "Name of the MCP server as configured in mcp.json "
                     "(e.g. \"filesystem\", \"github\")."}
                }},
                {"method", Json{
                    {"type",        "string"},
                    {"description",
                     "JSON-RPC method to invoke on the server "
                     "(e.g. \"tools/call\", \"tools/list\", \"resources/read\")."}
                }},
                {"params", Json{
                    {"type",        "object"},
                    {"description",
                     "Optional JSON-RPC params object forwarded verbatim to the server. "
                     "Omit or pass null when the method takes no parameters."}
                }}
            }},
            {"required", Json::array({"server", "method"})}
        }}
    };
}

// =============================================================================
// run()
// =============================================================================

ToolResult McpTool::run(const Json& args, ToolContext& ctx) {
    // ------------------------------------------------------------------
    // 0. Cancellation check before any work.
    // ------------------------------------------------------------------
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    // ------------------------------------------------------------------
    // 1. Extract and validate required argument: server.
    // ------------------------------------------------------------------
    if (!args.contains("server") || !args.at("server").is_string()) {
        return ToolResult::error("MCP: missing or non-string 'server' argument");
    }
    const std::string server = args.at("server").get<std::string>();
    if (server.empty()) {
        return ToolResult::error("MCP: 'server' must not be empty");
    }

    // ------------------------------------------------------------------
    // 2. Extract and validate required argument: method.
    // ------------------------------------------------------------------
    if (!args.contains("method") || !args.at("method").is_string()) {
        return ToolResult::error("MCP: missing or non-string 'method' argument");
    }
    const std::string method = args.at("method").get<std::string>();
    if (method.empty()) {
        return ToolResult::error("MCP: 'method' must not be empty");
    }

    // ------------------------------------------------------------------
    // 3. Extract optional params argument (defaults to null Json).
    // ------------------------------------------------------------------
    Json params = Json(nullptr);
    if (args.contains("params") && !args.at("params").is_null()) {
        params = args.at("params");
    }

    // ------------------------------------------------------------------
    // 4. Resolve the named transport from the registry.
    // ------------------------------------------------------------------
    batbox::mcp::IMcpTransport* transport = registry_.get(server);
    if (!transport) {
        return ToolResult::error(
            "MCP: unknown server '" + server +
            "'; check that it is configured in mcp.json");
    }

    // ------------------------------------------------------------------
    // 5. Verify the transport is healthy before dispatching.
    // ------------------------------------------------------------------
    if (!transport->healthy()) {
        return ToolResult::error(
            "MCP: server '" + server + "' is not healthy; "
            "check connection or restart with /mcp restart " + server);
    }

    // ------------------------------------------------------------------
    // 6. Cancellation check before network I/O.
    // ------------------------------------------------------------------
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    // ------------------------------------------------------------------
    // 7. Dispatch: IMcpTransport::request(method, params, cancel_token).
    //
    // CancelToken child is derived from the ToolContext cancel token so
    // that either the tool-level cancel or the request-level cancel
    // terminates the call.
    // ------------------------------------------------------------------
    auto [child_src, child_tok] = ctx.cancel_token.child();
    (void)child_src; // lifecycle managed by parent

    BATBOX_LOG_DEBUG("McpTool: dispatching {}/{} to transport", server, method);

    auto result = transport->request(method, std::move(params), std::move(child_tok));

    // ------------------------------------------------------------------
    // 8. Cancellation check after network I/O.
    // ------------------------------------------------------------------
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    // ------------------------------------------------------------------
    // 9. Surface transport / JSON-RPC errors as ToolResult::error.
    // ------------------------------------------------------------------
    if (!result.has_value()) {
        BATBOX_LOG_WARN("McpTool: {}/{} failed: {}", server, method, result.error());
        return ToolResult::error("MCP error: " + result.error());
    }

    // ------------------------------------------------------------------
    // 10. Success — return the raw JSON-RPC result as a compact string.
    // ------------------------------------------------------------------
    const Json& res_json = result.value();

    // Build a human-readable body: compact JSON dump of the result.
    std::string body;
    try {
        body = res_json.dump();
    } catch (const std::exception& ex) {
        return ToolResult::error(
            std::string("MCP: failed to serialise result: ") + ex.what());
    }

    // Provide a structured payload so callers can inspect the raw result
    // without re-parsing the body string.
    Json payload = Json{
        {"server", server},
        {"method", method},
        {"result", res_json}
    };

    BATBOX_LOG_DEBUG("McpTool: {}/{} succeeded ({} bytes)", server, method, body.size());

    return ToolResult::ok(std::move(body), std::move(payload));
}

} // namespace batbox::tools
