// include/batbox/mcp/McpClient.hpp
// ---------------------------------------------------------------------------
// McpClient — orchestrator over all configured MCP servers.
//
// Design (per ned-cpp.md §2.C8 — Decision of Record #4):
//   McpClient holds a reference to a McpServerRegistry (which owns the
//   transport map and lifecycle).  It drives the MCP initialize handshake on
//   every transport and exposes per-method helpers that fan out to the right
//   transport by server name.
//
// Lifecycle:
//   1. Construct McpClient with the registry that already has transports loaded
//      (McpServerRegistry::load_from_config / add_transport).
//   2. Call initialize_all(ct) — runs the MCP "initialize" + "initialized"
//      handshake on every transport in parallel.  Returns Ok when all succeed.
//      Partial failures are reported in the Err string (comma-separated list).
//   3. Call tools_list / tools_call / resources_list / resources_read /
//      prompts_list / prompts_get as needed.  Each call looks up the named
//      transport and issues the JSON-RPC method.
//
// Notification handling:
//   Each transport's on_notification callback is registered during
//   initialize_all().  The client handles "notifications/tools/list_changed"
//   by invalidating the cached tools list for that server.
//
// Tool-list caching:
//   tools_list(server, ct) caches the result per server.  The cache is
//   invalidated when:
//     (a) the server pushes a "notifications/tools/list_changed" notification.
//     (b) the transport transitions to unhealthy and then reconnects (caller
//         should call initialize_all() or initialize_one() again).
//   Force-refresh by passing force_refresh=true.
//
// Reconnect on transport failure:
//   Any request() call that returns Err on a transport that was previously
//   healthy tries one reconnect via McpServerRegistry::restart() and retries
//   the request once.  If the retry also fails the error is returned as-is.
//
// Thread safety:
//   All public methods are safe to call from multiple threads concurrently.
//
// Build standalone test (from repo root):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_mcp_client.cpp \
//       src/mcp/McpClient.cpp \
//       src/mcp/McpServerRegistry.cpp \
//       src/mcp/JsonRpc.cpp \
//       src/config/McpConfig.cpp \
//       src/core/CancelToken.cpp src/core/Json.cpp \
//       src/core/Logging.cpp src/core/Paths.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       build/vcpkg_installed/arm64-osx/lib/libspdlog.a \
//       build/vcpkg_installed/arm64-osx/lib/libfmt.a \
//       -o /tmp/test_mcp_client && /tmp/test_mcp_client
// ---------------------------------------------------------------------------

#pragma once

#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/mcp/McpServerRegistry.hpp>

#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace batbox::mcp {

// ============================================================================
// ServerCapabilities — result of the MCP initialize handshake
// ============================================================================

/// Capabilities advertised by an MCP server during the initialize handshake.
struct ServerCapabilities {
    bool tools     = false;  ///< Server supports tools/list + tools/call.
    bool resources = false;  ///< Server supports resources/list + resources/read.
    bool prompts   = false;  ///< Server supports prompts/list + prompts/get.
    Json raw;                ///< Full capabilities object from the initialize response.
};

// ============================================================================
// McpClient
// ============================================================================

/// Orchestrates all configured MCP servers via their registered IMcpTransports.
///
/// McpClient does NOT own the registry — it holds a non-owning reference.
/// The registry (and thus all transports) must outlive the McpClient.
class McpClient {
public:
    /// Construct a McpClient bound to the given registry.
    /// The registry must outlive this client.
    explicit McpClient(McpServerRegistry& registry) noexcept;

    // Non-copyable, non-movable (holds references and owns a mutex).
    McpClient(const McpClient&)            = delete;
    McpClient& operator=(const McpClient&) = delete;
    McpClient(McpClient&&)                 = delete;
    McpClient& operator=(McpClient&&)      = delete;

    ~McpClient() = default;

    // -------------------------------------------------------------------------
    // Initialization
    // -------------------------------------------------------------------------

    /// Run the MCP initialize handshake on ALL transports in parallel.
    ///
    /// For each transport:
    ///   1. Send "initialize" request with our client capabilities.
    ///   2. Parse the server's capabilities from the response.
    ///   3. Send "notifications/initialized" notification.
    ///   4. Register the on_notification handler for list_changed events.
    ///
    /// @param ct  Cancellation token.  Each per-transport handshake receives
    ///            a child token derived from ct.
    /// @return    Ok when ALL transports succeed.
    ///            Err("<name>: <reason>, ...") listing all failures.
    [[nodiscard]] Result<void> initialize_all(CancelToken ct);

    /// Run the MCP initialize handshake on a single named transport.
    ///
    /// Useful after a reconnect via McpServerRegistry::restart().
    ///
    /// @param server  Server name as registered in the registry.
    /// @param ct      Cancellation token.
    /// @return        Ok on success, Err with a human-readable description.
    [[nodiscard]] Result<void> initialize_one(std::string_view server, CancelToken ct);

    // -------------------------------------------------------------------------
    // Tools
    // -------------------------------------------------------------------------

    /// Fetch the tools list from the named server.
    ///
    /// Results are cached per server; the cache is invalidated when the server
    /// pushes a "notifications/tools/list_changed" notification or when
    /// force_refresh is true.
    ///
    /// @param server        Server name.
    /// @param ct            Cancellation token.
    /// @param force_refresh When true, bypasses the cache and re-fetches.
    /// @return              The "result" JSON from the server's tools/list reply.
    [[nodiscard]] Result<Json> tools_list(std::string_view server,
                                          CancelToken       ct,
                                          bool              force_refresh = false);

    /// Invoke a tool on the named server.
    ///
    /// @param server     Server name.
    /// @param tool_name  Name of the tool to call.
    /// @param args       Tool arguments object (JSON).
    /// @param ct         Cancellation token.
    /// @return           The "result" JSON from the server's tools/call reply.
    [[nodiscard]] Result<Json> tools_call(std::string_view server,
                                          std::string_view tool_name,
                                          const Json&      args,
                                          CancelToken      ct);

    // -------------------------------------------------------------------------
    // Resources
    // -------------------------------------------------------------------------

    /// Fetch the resource list from the named server.
    ///
    /// @param server  Server name.
    /// @param ct      Cancellation token.
    /// @return        The "result" JSON from the server's resources/list reply.
    [[nodiscard]] Result<Json> resources_list(std::string_view server,
                                              CancelToken       ct);

    /// Read a resource from the named server.
    ///
    /// @param server  Server name.
    /// @param uri     Resource URI to read.
    /// @param ct      Cancellation token.
    /// @return        The "result" JSON from the server's resources/read reply.
    [[nodiscard]] Result<Json> resources_read(std::string_view server,
                                              std::string_view uri,
                                              CancelToken      ct);

    // -------------------------------------------------------------------------
    // Prompts
    // -------------------------------------------------------------------------

    /// Fetch the prompts list from the named server.
    ///
    /// @param server  Server name.
    /// @param ct      Cancellation token.
    /// @return        The "result" JSON from the server's prompts/list reply.
    [[nodiscard]] Result<Json> prompts_list(std::string_view server,
                                            CancelToken       ct);

    /// Get a specific prompt from the named server.
    ///
    /// @param server       Server name.
    /// @param prompt_name  Name of the prompt to get.
    /// @param prompt_args  Optional argument map for the prompt (JSON object or null).
    /// @param ct           Cancellation token.
    /// @return             The "result" JSON from the server's prompts/get reply.
    [[nodiscard]] Result<Json> prompts_get(std::string_view server,
                                           std::string_view prompt_name,
                                           const Json&      prompt_args,
                                           CancelToken      ct);

    // -------------------------------------------------------------------------
    // Capability queries
    // -------------------------------------------------------------------------

    /// Return the cached capabilities for the named server, or nullopt if the
    /// server has not yet been initialized.
    [[nodiscard]] std::optional<ServerCapabilities>
    capabilities(std::string_view server) const;

    /// Return the names of all servers that have been successfully initialized.
    [[nodiscard]] std::vector<std::string> initialized_servers() const;

private:
    // -------------------------------------------------------------------------
    // Internal helpers
    // -------------------------------------------------------------------------

    /// Look up the named transport; return nullptr with an Err if not found.
    /// Also checks healthy() and attempts one reconnect if unhealthy.
    [[nodiscard]] IMcpTransport* resolve_transport(std::string_view   server,
                                                    std::string&       out_err);

    /// Perform the initialize handshake on the given transport.
    /// Called from initialize_all() and initialize_one().
    [[nodiscard]] Result<void> do_initialize(std::string_view server,
                                              IMcpTransport&   transport,
                                              CancelToken      ct);

    /// Dispatch a JSON-RPC request to the named server; handles one reconnect
    /// on transport failure.
    [[nodiscard]] Result<Json> dispatch(std::string_view server,
                                        std::string      method,
                                        Json             params,
                                        CancelToken      ct);

    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------

    McpServerRegistry& registry_;  ///< Non-owning reference to the transport map.

    mutable std::mutex            caps_mu_;
    std::unordered_map<std::string, ServerCapabilities> caps_;  ///< per-server capabilities

    mutable std::mutex            cache_mu_;
    std::unordered_map<std::string, Json> tools_cache_;  ///< cached tools/list results
    std::unordered_map<std::string, bool> cache_valid_;  ///< per-server validity flag
};

} // namespace batbox::mcp
