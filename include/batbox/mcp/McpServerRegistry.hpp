// include/batbox/mcp/McpServerRegistry.hpp
// ---------------------------------------------------------------------------
// McpServerRegistry — owns the full lifecycle of every configured MCP server.
//
// Design (per ned-cpp.md §2.C8):
//   Reads ~/.batbox/mcp.json (and ~/.claude/mcp.json) via load_mcp_configs(),
//   instantiates the correct IMcpTransport concrete type for each entry, and
//   manages start / stop / health monitoring across all servers.
//
//   Transport factory:
//     StdioConfig → StdioTransport(command, args, env)
//     HttpConfig  → HttpTransport(url, headers)
//     SseConfig   → SseTransport(url, headers)
//     WsConfig    → WsTransport(url, headers)
//
//   start_all():
//     Launches start() on each transport in parallel (one std::thread per
//     server).  Collects errors.  Returns a vector of (name, error_message)
//     for any server that failed to start; an empty vector means all succeeded.
//
//   restart(name):
//     Calls stop() on the named transport then start() again.
//     Returns Err if the name is unknown or start() fails.
//
//   health_monitor():
//     Background thread polling healthy() every 30 s. When a transport
//     transitions from healthy to unhealthy, fires the registered
//     on_status_change callback with the server name and new state.
//     Must be started explicitly via start_health_monitor().
//
//   on_status_change(fn):
//     Register a callback invoked whenever a server's health transitions.
//     Called from the health-monitor thread.
//
// Thread safety:
//   start_all(), restart(), and the public accessors are all safe to call
//   concurrently.  The health-monitor thread only reads healthy() (thread-safe
//   per IMcpTransport contract) and writes no shared mutable state beyond
//   firing the callback.
//
// Build standalone integration test (from repo root):
//   c++ -std=c++20 -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/integration/test_mcp_registry.cpp \
//       src/mcp/McpServerRegistry.cpp src/mcp/JsonRpc.cpp \
//       src/config/McpConfig.cpp \
//       src/core/CancelToken.cpp src/core/Json.cpp \
//       src/core/Logging.cpp src/core/Paths.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       build/vcpkg_installed/arm64-osx/lib/libspdlog.a \
//       build/vcpkg_installed/arm64-osx/lib/libfmt.a \
//       -o /tmp/test_mcp_registry && /tmp/test_mcp_registry
// ---------------------------------------------------------------------------

#pragma once

#include <batbox/config/McpConfig.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/mcp/IMcpTransport.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace batbox::mcp {

// ============================================================================
// HealthEvent — passed to the on_status_change callback
// ============================================================================

/// Health state reported by the health monitor.
enum class HealthState {
    Healthy,   ///< Transport became healthy (or was already healthy at startup).
    Unhealthy, ///< Transport transitioned from healthy → not healthy.
};

/// A single health status-change notification.
struct HealthEvent {
    std::string name;   ///< Server name as it appears in mcp.json.
    HealthState state;  ///< New health state.
};

// ============================================================================
// McpServerRegistry
// ============================================================================

/// Owns the lifecycle of all configured MCP servers.
///
/// Usage:
///   McpServerRegistry reg;
///   reg.on_status_change([](HealthEvent e) { /* update status line */ });
///   auto errs = reg.start_all(ct);
///   // ...
///   auto r = reg.restart("filesystem", ct);
///   reg.stop_health_monitor();
class McpServerRegistry {
public:
    McpServerRegistry() = default;
    ~McpServerRegistry();

    // Non-copyable, non-movable (owns threads and mutex).
    McpServerRegistry(const McpServerRegistry&)            = delete;
    McpServerRegistry& operator=(const McpServerRegistry&) = delete;
    McpServerRegistry(McpServerRegistry&&)                 = delete;
    McpServerRegistry& operator=(McpServerRegistry&&)      = delete;

    // -------------------------------------------------------------------------
    // Configuration injection
    // -------------------------------------------------------------------------

    /// Load servers from the provided config vector instead of reading the
    /// filesystem.  Replaces any previously loaded servers.  Must be called
    /// before start_all().
    ///
    /// Primarily used by tests to inject MockTransport instances directly
    /// (use the two-argument overload below for that).
    void load_from_config(std::vector<batbox::config::McpServerConfig> servers);

    /// Register a pre-constructed transport under the given name.
    /// Intended for unit tests that inject MockTransport or other fakes.
    /// Replaces any existing transport with the same name.
    void add_transport(std::string name, std::unique_ptr<IMcpTransport> transport);

    // -------------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------------

    /// Load MCP server configs from ~/.batbox/mcp.json (and ~/.claude/mcp.json),
    /// instantiate transports, start all in parallel, and return any errors.
    ///
    /// @param ct  Cancellation token; passed to each transport's start().
    /// @return    Vector of (server_name, error_message) for failed servers.
    ///            An empty vector means all servers started successfully.
    [[nodiscard]] std::vector<std::pair<std::string, std::string>>
    start_all(CancelToken ct);

    /// Restart a single named server: stop() then start() again.
    ///
    /// @param name  Server name as it appears in mcp.json.
    /// @param ct    Cancellation token passed to the new start() call.
    /// @return      Ok on success, Err with a human-readable description if
    ///              the name is unknown or start() fails.
    [[nodiscard]] Result<void> restart(std::string_view name, CancelToken ct);

    /// Stop all transports and join the health-monitor thread (if running).
    void stop_all();

    // -------------------------------------------------------------------------
    // Health monitoring
    // -------------------------------------------------------------------------

    /// Register the callback invoked when any server's health changes.
    ///
    /// Called from the health-monitor thread with the server name and new state.
    /// Must be registered before start_health_monitor().
    void on_status_change(std::function<void(HealthEvent)> callback);

    /// Start the background health-monitor thread (polls every 30 s).
    /// Idempotent: a second call while the monitor is already running is a no-op.
    void start_health_monitor();

    /// Stop the health-monitor thread and join it.  Idempotent.
    void stop_health_monitor();

    // -------------------------------------------------------------------------
    // Accessors
    // -------------------------------------------------------------------------

    /// Returns a pointer to the named transport, or nullptr if not found.
    /// The returned pointer is valid for the lifetime of this registry.
    [[nodiscard]] IMcpTransport* get(std::string_view name) const;

    /// Returns the names of all registered servers in unspecified order.
    [[nodiscard]] std::vector<std::string> server_names() const;

    /// Returns the number of registered servers.
    [[nodiscard]] std::size_t size() const;

    /// Returns the count of servers whose transport is currently not healthy.
    ///
    /// A server is counted as "failed" when its IMcpTransport::healthy() returns
    /// false (i.e. it failed to start, crashed, or was stopped).
    ///
    /// Thread-safe: acquires transports_mu_ for the snapshot, then releases it
    /// before iterating — healthy() is itself thread-safe per IMcpTransport contract.
    ///
    /// Called by McpStatusPoller (TUI-FLOW-T11) once per second to populate the
    /// InputBar right-side footer chip.
    [[nodiscard]] int count_failed_servers() const;

private:
    // -------------------------------------------------------------------------
    // Internal helpers
    // -------------------------------------------------------------------------

    /// Factory: build the correct concrete transport from a McpServerConfig.
    [[nodiscard]] static std::unique_ptr<IMcpTransport>
    make_transport(const batbox::config::McpServerConfig& cfg);

    /// Body of the health-monitor background thread.
    void health_monitor_loop();

    // -------------------------------------------------------------------------
    // Transport map
    // -------------------------------------------------------------------------

    /// Guards transports_ for all public methods except the health thread
    /// (which reads healthy() only, which is itself thread-safe per contract).
    mutable std::mutex transports_mu_;
    std::unordered_map<std::string, std::unique_ptr<IMcpTransport>> transports_;

    // -------------------------------------------------------------------------
    // Health monitor
    // -------------------------------------------------------------------------

    std::function<void(HealthEvent)>     status_cb_;
    mutable std::mutex                   status_cb_mu_;

    std::thread            health_thread_;
    std::atomic<bool>      health_running_{false};

    /// Previous health snapshot, used to detect transitions.
    /// Keyed by server name; value is the last-known healthy() result.
    mutable std::mutex           prev_health_mu_;
    std::unordered_map<std::string, bool> prev_health_;
};

} // namespace batbox::mcp
