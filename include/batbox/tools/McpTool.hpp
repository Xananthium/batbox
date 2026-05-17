// include/batbox/tools/McpTool.hpp
//
// batbox::tools::McpTool — generic JSON-RPC proxy tool for MCP servers.
//
// Purpose:
//   Allows the model to call any arbitrary JSON-RPC method on any configured
//   MCP server.  The model supplies {server, method, params} and McpTool
//   routes the call through the corresponding IMcpTransport, returning the
//   raw JSON-RPC result as the tool body.
//
// Tool name   : "MCP"
// is_read_only()          : false (arbitrary method may have side effects)
// requires_confirmation() : true  (calling arbitrary server methods requires
//                                  user confirmation in non-nuclear modes)
//
// Arguments (JSON args):
//   server  (string, required) — name of the MCP server as configured in
//                                mcp.json (e.g. "filesystem", "github").
//   method  (string, required) — JSON-RPC method to invoke on that server
//                                (e.g. "tools/call", "tools/list",
//                                "resources/read", or any custom method).
//   params  (object, optional) — JSON-RPC params object forwarded verbatim
//                                to IMcpTransport::request().  Defaults to
//                                null when absent.
//
// Routing:
//   McpServerRegistry::get(server) → IMcpTransport*
//   IMcpTransport::request(method, params, cancel_token)
//
//   If the named server is not found → ToolResult::error("MCP: unknown server …")
//   If the transport is unhealthy     → ToolResult::error("MCP: server … is not healthy")
//   If request() returns Err          → ToolResult::error("MCP error: …")
//   If request() returns Ok           → ToolResult::ok(result.dump())
//
// Blueprint contract: batbox::tools::McpTool (blueprints table rows 16734–16736)

#pragma once

#include <batbox/mcp/McpServerRegistry.hpp>
#include <batbox/tools/ITool.hpp>

#include <string_view>

namespace batbox::tools {

// =============================================================================
// McpTool
// =============================================================================

class McpTool final : public ITool {
public:
    // -------------------------------------------------------------------------
    // Construction
    //
    // registry — reference to the shared McpServerRegistry that owns all
    //            MCP transport instances; must outlive this tool.
    // -------------------------------------------------------------------------
    explicit McpTool(batbox::mcp::McpServerRegistry& registry);

    // -------------------------------------------------------------------------
    // ITool interface
    // -------------------------------------------------------------------------

    /// Tool name: "MCP".
    [[nodiscard]] std::string_view name() const override;

    /// One-sentence description for the model's tool schema.
    [[nodiscard]] std::string_view description() const override;

    /// OpenAI tools[*].function JSON object for this tool.
    [[nodiscard]] Json schema_json() const override;

    /// Execute the MCP proxy call.
    ///
    /// args must contain:
    ///   "server"  — string (required); name of configured MCP server
    ///   "method"  — string (required); JSON-RPC method to invoke
    ///   "params"  — object (optional); forwarded verbatim as JSON-RPC params
    ///
    /// Returns:
    ///   ToolResult::ok(result_json_string)   on success
    ///   ToolResult::error(description)       on any failure path
    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override;

    /// Returns false — MCP method calls may mutate remote state.
    [[nodiscard]] bool is_read_only() const override { return false; }

    /// Returns true — arbitrary server method calls always require confirmation.
    [[nodiscard]] bool requires_confirmation() const override { return true; }

private:
    batbox::mcp::McpServerRegistry& registry_;
};

} // namespace batbox::tools
