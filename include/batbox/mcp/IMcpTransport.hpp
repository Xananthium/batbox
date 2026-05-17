// include/batbox/mcp/IMcpTransport.hpp
// ---------------------------------------------------------------------------
// IMcpTransport — pure virtual interface for all MCP transport implementations.
//
// Design (per ned-cpp.md §2.C8 — Decision of Record #4):
//   All four concrete transports (stdio, sse, http, ws) implement this
//   interface. McpClient holds a map<string, unique_ptr<IMcpTransport>> and
//   operates exclusively through this interface, enabling transparent
//   transport substitution and easy mock-based unit testing.
//
// Lifetime model:
//   1. Construct the concrete transport (inject config / fd / URL).
//   2. Call start(ct) — establishes connection / spawns reader thread.
//      start() is idempotent: a second call on a healthy transport is a no-op.
//   3. Call request() / notify() to exchange messages.
//   4. Call stop() when done — graceful shutdown. Cancels pending requests.
//   5. Destroy the object (virtual destructor handles cleanup).
//
// Cancellation:
//   Every blocking call accepts a CancelToken. When the token fires the call
//   returns Err("cancelled") as soon as the underlying I/O allows. Callers
//   can pass a root token (for app shutdown) or a per-request child token.
//
// Thread safety:
//   Concrete transports must be safe to call request()/notify() from multiple
//   threads concurrently. on_notification() must be registered before start().
//
// Build standalone test (from repo root):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_mcp_mock_transport.cpp \
//       src/mcp/JsonRpc.cpp \
//       -o /tmp/test_mcp_mock_transport && /tmp/test_mcp_mock_transport
// ---------------------------------------------------------------------------

#pragma once

#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/core/Result.hpp>

#include <functional>
#include <string>

namespace batbox::mcp {

// ============================================================================
// IMcpTransport — abstract transport interface
// ============================================================================

/// Pure virtual base class for all MCP transports (stdio, SSE, HTTP, WebSocket).
///
/// Implementors provide the four concrete classes defined in C8. McpClient
/// owns instances as `std::unique_ptr<IMcpTransport>` and calls through this
/// interface exclusively, enabling mock-based unit testing without network I/O.
class IMcpTransport {
public:
    virtual ~IMcpTransport() = default;

    // -------------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------------

    /// Establish the transport connection and prepare it for message exchange.
    ///
    /// For stdio transports: spawns the child process and starts the reader
    /// thread. For network transports: performs the TCP/TLS handshake and
    /// starts the receive pump.
    ///
    /// Idempotent: a second call on a transport that is already healthy returns
    /// ok without re-connecting.
    ///
    /// @param ct   Cancellation token. If fired before the connection is fully
    ///             established, start() returns Err("cancelled").
    /// @return     Ok on success, Err with a description on failure.
    [[nodiscard]] virtual Result<void> start(CancelToken ct) = 0;

    /// Gracefully shut down the transport.
    ///
    /// Cancels all pending request() calls (they return Err("transport stopped"))
    /// and waits for the reader thread / receive pump to exit. After stop()
    /// returns, healthy() is false. Calling stop() on an already-stopped
    /// transport is a no-op.
    virtual void stop() = 0;

    /// Returns true when the transport is connected and able to exchange
    /// messages.  Thread-safe: may be polled by McpClient's health-check timer.
    [[nodiscard]] virtual bool healthy() const = 0;

    // -------------------------------------------------------------------------
    // Message exchange
    // -------------------------------------------------------------------------

    /// Send a JSON-RPC 2.0 request and wait for the matching response.
    ///
    /// Assigns the next monotonically-increasing id from next_id(), serialises
    /// the request, sends it over the wire, and blocks until:
    ///   (a) the matching response arrives  → returns Ok(result)
    ///   (b) the response contains an error → returns Err("<code>: <message>")
    ///   (c) ct fires                       → returns Err("cancelled")
    ///   (d) the transport disconnects      → returns Err("transport disconnected")
    ///
    /// @param method  JSON-RPC method name (e.g. "tools/call").
    /// @param params  Request parameters; may be null.
    /// @param ct      Per-request cancellation token.
    /// @return        The "result" field of the JSON-RPC response on success.
    [[nodiscard]] virtual Result<Json> request(std::string method,
                                               Json        params,
                                               CancelToken ct) = 0;

    /// Send a JSON-RPC 2.0 notification (fire-and-forget — no response).
    ///
    /// @param method  JSON-RPC method name (e.g. "notifications/progress").
    /// @param params  Notification parameters; may be null.
    /// @return        Ok when the bytes are queued for writing, Err on
    ///                serialisation failure or if the transport is stopped.
    [[nodiscard]] virtual Result<void> notify(std::string method,
                                              Json        params) = 0;

    // -------------------------------------------------------------------------
    // Inbound notification dispatch
    // -------------------------------------------------------------------------

    /// Register a callback invoked for every inbound JSON-RPC notification
    /// received from the server.
    ///
    /// Must be called before start(). Replaces any previously registered
    /// handler; only one handler is supported per transport instance.
    ///
    /// The callback is invoked on the transport's internal reader thread with:
    ///   @param method  The JSON-RPC method string of the notification.
    ///   @param params  The params payload (may be null Json).
    ///
    /// Implementations must ensure the callback is never called after stop()
    /// returns. Callers must not call stop() from inside the callback (deadlock).
    virtual void on_notification(
        std::function<void(std::string method, Json params)> handler) = 0;
};

} // namespace batbox::mcp
