// include/batbox/mcp/WsTransport.hpp
// ---------------------------------------------------------------------------
// WsTransport — IXWebSocket-based MCP transport.
//
// Design (per ned-cpp.md §2.C8 — CPP 8.7):
//   Owns a single ix::WebSocket connection (ws:// or wss://).
//   setOnMessageCallback dispatches each received text frame as a JSON-RPC
//   envelope:
//     • Response  → resolved into the pending request's promise.
//     • Notification → forwarded to the registered on_notification() handler.
//     • Request   → logged as unexpected (MCP servers never send requests).
//   sendText() serialises outgoing JSON-RPC messages.
//   Auto-reconnect is DISABLED — callers (McpClient) handle reconnect policy.
//   TLS via OpenSSL: no Boost; setCaFile() to a bundled cacert.pem when the
//   system bundle is absent.
//
// Lifetime:
//   1. WsTransport(config)     — configure URL, headers, optional CA path.
//   2. on_notification(fn)     — register before start().
//   3. start(ct)               — connect (synchronous handshake, 10 s timeout).
//   4. request() / notify()    — exchange messages.
//   5. stop()                  — close + join internal IXWebSocket thread.
//
// Thread safety:
//   request() and notify() may be called from any thread concurrently.
//   The IXWebSocket callback fires on IXWebSocket's internal background thread;
//   all shared state is protected by mutex or atomic.
//
// Build standalone test (from repo root):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/integration/test_mcp_ws.cpp \
//       src/mcp/JsonRpc.cpp \
//       src/mcp/WsTransport.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libixwebsocket.a \
//       -framework Security -framework CoreFoundation \
//       -o /tmp/test_mcp_ws && /tmp/test_mcp_ws
// ---------------------------------------------------------------------------

#pragma once

#include <batbox/mcp/IMcpTransport.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/core/Result.hpp>

#include <ixwebsocket/IXWebSocket.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace batbox::mcp {

// ============================================================================
// WsTransport
// ============================================================================

/// IXWebSocket-based MCP transport.
///
/// Implements IMcpTransport over a persistent WebSocket connection.
/// Supports both ws:// and wss:// URLs; TLS negotiated via OpenSSL (vcpkg
/// ixwebsocket[ssl] feature — no Boost required).
class WsTransport final : public IMcpTransport {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    /// Construct a WsTransport for the given URL.
    ///
    /// @param url          ws:// or wss:// endpoint URL.
    /// @param headers      Extra HTTP headers sent in the upgrade request
    ///                     (e.g. "Authorization: Bearer <token>").
    /// @param ca_file_path Optional path to a PEM CA bundle. When provided,
    ///                     overrides the system bundle. Ignored for ws:// URLs.
    explicit WsTransport(
        std::string url,
        std::unordered_map<std::string, std::string> headers = {},
        std::string ca_file_path = {});

    ~WsTransport() override;

    // Non-copyable, non-movable (owns a mutex + condition_variable).
    WsTransport(const WsTransport&)            = delete;
    WsTransport& operator=(const WsTransport&) = delete;
    WsTransport(WsTransport&&)                 = delete;
    WsTransport& operator=(WsTransport&&)      = delete;

    // -------------------------------------------------------------------------
    // IMcpTransport — lifecycle
    // -------------------------------------------------------------------------

    /// Connect to the WebSocket server.
    ///
    /// Performs the HTTP upgrade handshake (10-second timeout).  Returns Ok
    /// when the Open message is received.  Returns Err("cancelled") if ct fires
    /// before the handshake completes.  Idempotent on an already-healthy transport.
    [[nodiscard]] Result<void> start(CancelToken ct) override;

    /// Gracefully close the WebSocket and cancel all pending requests.
    ///
    /// After stop() returns, healthy() is false.  Calling stop() on an already-
    /// stopped transport is a no-op.
    void stop() override;

    /// Returns true when the WebSocket is in the Open state.
    [[nodiscard]] bool healthy() const override;

    // -------------------------------------------------------------------------
    // IMcpTransport — message exchange
    // -------------------------------------------------------------------------

    /// Send a JSON-RPC 2.0 request and block until the matching response arrives.
    ///
    /// Assigns the next id from next_id(), serialises the request via
    /// make_request(), sends it with sendText(), then waits for the response
    /// to be delivered by the on_message callback.
    ///
    /// Returns:
    ///   Ok(result)                    — on JSON-RPC success response.
    ///   Err("<code>: <message>")      — on JSON-RPC error response.
    ///   Err("cancelled")              — if ct fires before response arrives.
    ///   Err("transport disconnected") — if the connection closes while waiting.
    ///   Err("transport stopped")      — if stop() is called while waiting.
    [[nodiscard]] Result<Json> request(std::string method,
                                       Json        params,
                                       CancelToken ct) override;

    /// Send a JSON-RPC 2.0 notification (fire-and-forget).
    ///
    /// Returns Ok when bytes are queued, Err if the transport is stopped.
    [[nodiscard]] Result<void> notify(std::string method,
                                      Json        params) override;

    // -------------------------------------------------------------------------
    // IMcpTransport — inbound notification dispatch
    // -------------------------------------------------------------------------

    /// Register the callback invoked for every server-initiated notification.
    ///
    /// Must be called before start(). Replaces any previously registered handler.
    void on_notification(
        std::function<void(std::string method, Json params)> handler) override;

private:
    // -------------------------------------------------------------------------
    // Internal helpers
    // -------------------------------------------------------------------------

    /// Dispatch one received text frame.  Called on the IXWebSocket thread.
    void handle_message(const std::string& text);

    /// Notify all pending requests that the transport disconnected.
    void cancel_pending(const std::string& reason);

    // -------------------------------------------------------------------------
    // Per-request state stored in the pending map
    // -------------------------------------------------------------------------
    struct PendingEntry {
        std::optional<Result<Json>> result;  ///< filled by on_message callback
        std::condition_variable     cv;
        std::mutex                  mu;
    };

    // -------------------------------------------------------------------------
    // Configuration (immutable after construction)
    // -------------------------------------------------------------------------
    std::string url_;
    std::unordered_map<std::string, std::string> headers_;
    std::string ca_file_path_;

    // -------------------------------------------------------------------------
    // IXWebSocket instance
    // -------------------------------------------------------------------------
    ix::WebSocket ws_;

    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------
    std::atomic<bool> healthy_{false};
    std::atomic<bool> stopped_{false};

    // Guards start() handshake wait
    std::mutex              connect_mu_;
    std::condition_variable connect_cv_;
    std::optional<bool>     connect_result_;   ///< true=open, false=error
    std::string             connect_error_;

    // Guards pending_ map
    mutable std::mutex pending_mu_;
    std::unordered_map<int64_t, std::shared_ptr<PendingEntry>> pending_;

    // Inbound notification handler (registered before start())
    std::function<void(std::string, Json)> notif_handler_;
    mutable std::mutex                     notif_mu_;
};

} // namespace batbox::mcp
