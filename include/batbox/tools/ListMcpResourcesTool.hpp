// include/batbox/tools/ListMcpResourcesTool.hpp
// =============================================================================
// batbox::tools::ListMcpResourcesTool — ITool that lists resources from one or
// all connected MCP servers via the JSON-RPC 2.0 "resources/list" method.
//
// Contract (blueprints table, task CPP 5.26):
//
//   Tool name       : "ListMcpResources"
//   is_read_only()  : true  (listing resources has no side effects)
//   requires_confirmation() : false  (no side-effects; never prompts)
//
//   JSON args:
//     server  (string, optional) — if provided, only query that named server.
//                                  If absent, query ALL connected servers.
//
//   Behaviour:
//     For each server queried, sends resources/list to the transport obtained
//     from McpServerRegistry::get(server_name).  Aggregates results and
//     prefixes every resource name with "<server_name>:" so callers can
//     identify which server owns each resource.
//
//     Unknown server name (when args.server is provided):
//       → ToolResult::error("ListMcpResources: unknown server: <name>")
//
//     No servers registered:
//       → ToolResult::ok("No MCP servers are connected.")
//
//     Transport error for one server (in all-servers mode):
//       → that server's entry shows "error: <message>" in the listing;
//         other servers are still included.
//
//   Returns (body):
//     One resource per line: "<server>:<name>  <uri>  [<description>]"
//     OR "No resources found." when all servers returned empty lists.
//
//   Structured payload:
//     {
//       "servers": [
//         {
//           "server": "<server_name>",
//           "resources": [
//             {"name":"…","uri":"…","description":"…","mimeType":"…"},
//             …
//           ],
//           "error": "<message>"   // present only on transport failure
//         },
//         …
//       ]
//     }
//
// Thread safety:
//   ListMcpResourcesTool holds a non-owning reference to McpServerRegistry.
//   The registry must outlive the tool.  Concurrent calls to run() are safe
//   because McpServerRegistry::get() is thread-safe and IMcpTransport::request()
//   is thread-safe per its contract.
//
// Blueprint contract: batbox::tools::ListMcpResourcesTool (task CPP 5.26)
// =============================================================================

#pragma once

#include <batbox/mcp/McpServerRegistry.hpp>
#include <batbox/tools/ITool.hpp>

namespace batbox::tools {

// =============================================================================
// ListMcpResourcesTool
// =============================================================================

class ListMcpResourcesTool final : public ITool {
public:
    // -------------------------------------------------------------------------
    // Construction
    //
    // registry — reference to the live McpServerRegistry.  Must outlive this
    //            tool instance.
    // -------------------------------------------------------------------------
    explicit ListMcpResourcesTool(batbox::mcp::McpServerRegistry& registry) noexcept;

    // -------------------------------------------------------------------------
    // ITool identity
    // -------------------------------------------------------------------------
    [[nodiscard]] std::string_view name()        const override;
    [[nodiscard]] std::string_view description() const override;
    [[nodiscard]] Json             schema_json() const override;

    // -------------------------------------------------------------------------
    // ITool execution
    // -------------------------------------------------------------------------
    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override;

    // -------------------------------------------------------------------------
    // Permission gate hooks
    // -------------------------------------------------------------------------

    /// Listing resources is read-only — runs in Plan mode.
    [[nodiscard]] bool is_read_only()          const override { return true; }

    /// No side-effects; never requires a confirmation prompt.
    [[nodiscard]] bool requires_confirmation() const override { return false; }

private:
    batbox::mcp::McpServerRegistry& registry_; ///< Non-owning reference.
};

} // namespace batbox::tools
