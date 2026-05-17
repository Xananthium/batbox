// include/batbox/mcp/SseTransport.hpp
// ---------------------------------------------------------------------------
// SseTransport — MCP transport over Server-Sent Events.
//
// MCP SSE protocol:
//   1. Client opens a persistent GET /sse (text/event-stream).
//   2. Server emits the first event:
//        event: endpoint
//        data: /messages?session=<id>
//      The data field is the POST URL (may be relative or absolute).
//   3. Subsequent events carry server→client JSON-RPC messages:
//        event: message
//        data: {"jsonrpc":"2.0","id":1,"result":{...}}
//   4. Client sends JSON-RPC requests/notifications by POSTing to the
//      endpoint URL. The server responds 202 Accepted; the actual JSON-RPC
//      response arrives later over the SSE stream and is correlated by id.
//
// Lifetime:
//   1. SseTransport(url, headers)  — configure SSE URL and optional headers.
//   2. on_notification(fn)         — register before start().
//   3. start(ct)                   — opens SSE stream, blocks until the
//                                    endpoint URL is received or failure.
//   4. request() / notify()        — exchange messages.
//   5. stop()                      — aborts SSE stream + joins reader thread.
//
// Thread safety:
//   request() and notify() may be called concurrently from any thread.
//   The SSE reader thread feeds the SseParser and dispatches messages under
//   mutex protection. All shared state guarded by mutex or atomic.
//
// Build standalone test (from repo root):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/integration/test_mcp_sse.cpp \
//       src/mcp/SseTransport.cpp \
//       src/mcp/JsonRpc.cpp \
//       src/inference/SseParser.cpp \
//       src/core/CancelToken.cpp \
//       -Lbuild/vcpkg_installed/arm64-osx/lib -lcpr -lssl -lcrypto -lcurl \
//       -o /tmp/test_mcp_sse && /tmp/test_mcp_sse
// ---------------------------------------------------------------------------

#pragma once

#include <batbox/mcp/IMcpTransport.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/core/Result.hpp>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

namespace batbox::mcp {

// ============================================================================
// SseTransport
// ============================================================================

/// MCP transport over Server-Sent Events (SSE).
///
/// Implements IMcpTransport using a persistent GET /sse connection for
/// inbound messages and cpr::Post to the server-provided endpoint URL for
/// outbound requests and notifications.
class SseTransport final : public IMcpTransport {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    /// Construct an SseTransport for the given SSE endpoint URL.
    ///
    /// @param url      Full URL of the SSE endpoint (e.g. "http://localhost:8080/sse").
    /// @param headers  Extra HTTP headers included in both the GET /sse request
    ///                 and all POST requests (e.g. "Authorization: Bearer <token>").
    explicit SseTransport(
        std::string url,
        std::unordered_map<std::string, std::string> headers = {});

    ~SseTransport() override;

    // Non-copyable, non-movable (owns mutex + condition_variable + thread).
    SseTransport(const SseTransport&)            = delete;
    SseTransport& operator=(const SseTransport&) = delete;
    SseTransport(SseTransport&&)                 = delete;
    SseTransport& operator=(SseTransport&&)      = delete;

    // -------------------------------------------------------------------------
    // IMcpTransport — lifecycle
    // -------------------------------------------------------------------------

    /// Open the SSE stream and wait for the endpoint URL.
    ///
    /// Spawns a background reader thread that issues GET /sse and feeds the
    /// response stream through SseParser. Blocks until the first "endpoint"
    /// event is received (endpoint URL captured) or until failure/cancellation.
    ///
    /// Idempotent on an already-healthy transport.
    ///
    /// @param ct  Cancellation token. Returns Err("cancelled") if fired before
    ///            the endpoint event arrives.
    /// @return    Ok on success, Err with description on failure.
    [[nodiscard]] Result<void> start(CancelToken ct) override;

    /// Stop the SSE stream and cancel all pending requests.
    ///
    /// Sets the stopped flag (causing the WriteCallback to return false, which
    /// aborts the cpr transfer), then joins the reader thread. Cancels all
    /// pending request() calls with Err("transport stopped"). Idempotent.
    void stop() override;

    /// Returns true when the SSE stream is open and the endpoint URL is known.
    [[nodiscard]] bool healthy() const override;

    // -------------------------------------------------------------------------
    // IMcpTransport — message exchange
    // -------------------------------------------------------------------------

    /// Send a JSON-RPC 2.0 request and block until the matching response arrives.
    ///
    /// Assigns the next id from next_id(), serialises the request, POSTs it to
    /// the server endpoint URL, then waits for the matching response on the SSE
    /// stream.
    ///
    /// Returns:
    ///   Ok(result)                    — on JSON-RPC success response.
    ///   Err("<code>: <message>")      — on JSON-RPC error response.
    ///   Err("cancelled")              — if ct fires before response arrives.
    ///   Err("transport disconnected") — if the SSE stream closes while waiting.
    ///   Err("transport stopped")      — if stop() is called while waiting.
    ///   Err("post failed: ...")       — if the POST itself fails at transport level.
    [[nodiscard]] Result<Json> request(std::string method,
                                       Json        params,
                                       CancelToken ct) override;

    /// Send a JSON-RPC 2.0 notification (fire-and-forget).
    ///
    /// POSTs the notification to the server endpoint URL. Returns Ok when the
    /// server acknowledges with 202, Err if the POST fails or transport is stopped.
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

    /// Background thread entry point: runs cpr::Download on the SSE URL.
    /// Feeds received bytes into SseParser and dispatches complete events.
    void reader_loop();

    /// Dispatch one complete SSE event from the reader loop.
    /// Handles "endpoint" events (captures post URL) and "message" events
    /// (routes JSON-RPC payloads to pending requests or notification handler).
    void handle_event(const std::string& event_type, const std::string& data);

/// Notify all pending requests that the transport has disconnected.
    void cancel_pending(const std::string& reason);

    // -------------------------------------------------------------------------
    // Per-request pending state
    // -------------------------------------------------------------------------
    struct PendingEntry {
        std::optional<Result<Json>> result;  ///< filled by SSE reader thread
        std::condition_variable     cv;
        std::mutex                  mu;
    };

    // -------------------------------------------------------------------------
    // Configuration (immutable after construction)
    // -------------------------------------------------------------------------
    std::string sse_url_;
    std::unordered_map<std::string, std::string> headers_;

    // -------------------------------------------------------------------------
    // Runtime state
    // -------------------------------------------------------------------------
    std::atomic<bool> healthy_{false};
    std::atomic<bool> stopped_{false};

    /// The POST endpoint URL received from the server's "endpoint" SSE event.
    std::string post_url_;
    std::mutex  post_url_mu_;

    // Guards start() handshake wait: blocks until endpoint URL received or error.
    std::mutex              connect_mu_;
    std::condition_variable connect_cv_;
    std::optional<bool>     connect_result_;  ///< true = endpoint received, false = error
    std::string             connect_error_;

    // Background SSE reader thread.
    std::thread reader_thread_;

    // Guards pending_ map (id → PendingEntry).
    mutable std::mutex pending_mu_;
    std::unordered_map<int64_t, std::shared_ptr<PendingEntry>> pending_;

    // Inbound notification handler (registered before start()).
    std::function<void(std::string, Json)> notif_handler_;
    mutable std::mutex                     notif_mu_;
};

} // namespace batbox::mcp
