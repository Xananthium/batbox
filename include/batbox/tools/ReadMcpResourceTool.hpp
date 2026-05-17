// include/batbox/tools/ReadMcpResourceTool.hpp
// =============================================================================
// batbox::tools::ReadMcpResourceTool — ITool that reads the content of a
// specific MCP resource via the JSON-RPC 2.0 "resources/read" method.
//
// Contract (blueprints table, task CPP 5.26):
//
//   Tool name       : "ReadMcpResource"
//   is_read_only()  : true  (reading a resource has no side effects)
//   requires_confirmation() : false  (no mutation; never prompts)
//
//   JSON args:
//     server  (string, required) — the MCP server name to read from.
//     uri     (string, required) — the resource URI to read
//                                  (e.g. "file:///tmp/foo.txt").
//
//   Behaviour:
//     Looks up the named server in McpServerRegistry.  If not found, returns
//     an error.  Sends resources/read with {"uri": <uri>} to the transport.
//     On success, concatenates all content items (text fields preferred;
//     blob fields shown as "<binary blob>" placeholder) and returns them as
//     the body.
//
//     Unknown server:
//       → ToolResult::error("ReadMcpResource: unknown server: <name>")
//
//     Transport error (JSON-RPC error from server):
//       → ToolResult::error("ReadMcpResource: <error message>")
//
//   Returns (body):
//     Combined text content of all returned content items, separated by
//     newlines.  If the server returns only binary blobs, the body will
//     contain "<binary blob>" entries.
//
//   Structured payload:
//     {
//       "server":   "<server_name>",
//       "uri":      "<uri>",
//       "contents": [
//         {"uri":"…", "mimeType":"…", "text":"…"},   // text content
//         {"uri":"…", "mimeType":"…", "blob":true},  // binary content
//         …
//       ]
//     }
//
// Thread safety:
//   ReadMcpResourceTool holds a non-owning reference to McpServerRegistry.
//   The registry must outlive the tool.  Concurrent calls to run() are safe
//   because McpServerRegistry::get() is thread-safe and IMcpTransport::request()
//   is thread-safe per its contract.
//
// Blueprint contract: batbox::tools::ReadMcpResourceTool (task CPP 5.26)
// =============================================================================

#pragma once

#include <batbox/mcp/McpServerRegistry.hpp>
#include <batbox/tools/ITool.hpp>

namespace batbox::tools {

// =============================================================================
// ReadMcpResourceTool
// =============================================================================

class ReadMcpResourceTool final : public ITool {
public:
    // -------------------------------------------------------------------------
    // Construction
    //
    // registry — reference to the live McpServerRegistry.  Must outlive this
    //            tool instance.
    // -------------------------------------------------------------------------
    explicit ReadMcpResourceTool(batbox::mcp::McpServerRegistry& registry) noexcept;

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

    /// Reading a resource is read-only — runs in Plan mode.
    [[nodiscard]] bool is_read_only()          const override { return true; }

    /// No side-effects; never requires a confirmation prompt.
    [[nodiscard]] bool requires_confirmation() const override { return false; }

private:
    batbox::mcp::McpServerRegistry& registry_; ///< Non-owning reference.
};

} // namespace batbox::tools
