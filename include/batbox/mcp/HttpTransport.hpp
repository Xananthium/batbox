// include/batbox/mcp/HttpTransport.hpp
// ---------------------------------------------------------------------------
// HttpTransport — JSON-RPC 2.0 over HTTP POST (+ optional streamable-http
// SSE variant).
//
// Design (per ned-cpp.md §2.C8 — CPP 8.5):
//   Plain HTTP POST per request to a configured URL. If the MCP server
//   advertises the "streamable-http" capability in its initialize response,
//   subsequent calls POST and expect a "text/event-stream" response which is
//   reassembled with batbox::inference::SseParser into one JSON-RPC response.
//   Notifications use the same POST path but do not wait for a response body.
//   A shared cpr::Session provides HTTP keep-alive across calls.
//   healthy() does a lightweight HEAD (or GET) probe on the base URL; once
//   stop() is called it returns false permanently.
//
// Thread safety:
//   request() and notify() may be called concurrently from multiple threads;
//   a mutex serialises access to the cpr::Session (which is not thread-safe).
//
// Lifetime:
//   1. HttpTransport(url, headers)  — configure endpoint and static headers.
//   2. on_notification(fn)          — register before start().
//   3. start(ct)                    — probe the base URL; sets healthy_.
//   4. request() / notify()         — exchange messages.
//   5. stop()                       — sets healthy_ = false; cancels in-flight
//                                    request() calls.
//
// Build standalone test (from repo root):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/integration/test_mcp_http.cpp \
//       src/mcp/JsonRpc.cpp \
//       src/mcp/HttpTransport.cpp \
//       src/inference/SseParser.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libcpr.a \
//       build/vcpkg_installed/arm64-osx/lib/libcurl.a \
//       -o /tmp/test_mcp_http && /tmp/test_mcp_http
// ---------------------------------------------------------------------------

#pragma once

#include <batbox/mcp/IMcpTransport.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/core/Result.hpp>

#include <cpr/cpr.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace batbox::mcp {

// ============================================================================
// HttpTransport
// ============================================================================

/// HTTP POST JSON-RPC MCP transport with optional streamable-http SSE variant.
///
/// Implements IMcpTransport by sending every JSON-RPC 2.0 request as an HTTP
/// POST to the configured URL.  When the server signals the "streamable-http"
/// capability the response body is a text/event-stream and is assembled back
/// into a single JSON-RPC response via SseParser.
class HttpTransport final : public IMcpTransport {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    /// Construct an HttpTransport.
    ///
    /// @param url      http:// or https:// endpoint (the MCP POST target).
    /// @param headers  Static HTTP headers sent with every request; supports
    ///                 Authorization: Bearer <token> patterns. Values must
    ///                 already have ${env:NAME} variables expanded (McpConfig
    ///                 does this at parse time).
    explicit HttpTransport(
        std::string url,
        std::unordered_map<std::string, std::string> headers = {});

    ~HttpTransport() override;

    // Non-copyable, non-movable (owns mutex + condition_variable).
    HttpTransport(const HttpTransport&)            = delete;
    HttpTransport& operator=(const HttpTransport&) = delete;
    HttpTransport(HttpTransport&&)                 = delete;
    HttpTransport& operator=(HttpTransport&&)      = delete;

    // -------------------------------------------------------------------------
    // IMcpTransport — lifecycle
    // -------------------------------------------------------------------------

    /// Probe the base URL (HEAD or GET /) and mark the transport healthy.
    ///
    /// Returns Ok when the server responds (any 2xx / 4xx is acceptable —
    /// we only care that the TCP connection succeeds, not that the method
    /// is supported).  Returns Err("cancelled") if ct fires before the probe
    /// completes.  Idempotent on an already-healthy transport.
    [[nodiscard]] Result<void> start(CancelToken ct) override;

    /// Set healthy_ = false and cancel all pending request() calls.
    ///
    /// After stop() returns, every in-progress request() unblocks with
    /// Err("transport stopped").  Calling stop() on an already-stopped
    /// transport is a no-op.
    void stop() override;

    /// Returns true when the transport is connected and ready.
    /// Thread-safe.
    [[nodiscard]] bool healthy() const override;

    // -------------------------------------------------------------------------
    // IMcpTransport — message exchange
    // -------------------------------------------------------------------------

    /// POST a JSON-RPC 2.0 request and block until the response arrives.
    ///
    /// If streamable_http_ is true the POST response is a text/event-stream;
    /// each SSE data: line is a JSON-RPC chunk and the final assembled JSON is
    /// used as the response.  Otherwise the response body is a plain JSON-RPC
    /// response object.
    ///
    /// Returns:
    ///   Ok(result)                    — on JSON-RPC success response.
    ///   Err("<code>: <message>")      — on JSON-RPC error response.
    ///   Err("cancelled")              — if ct fires before the response.
    ///   Err("transport stopped")      — if stop() is called first.
    ///   Err("http <N>: ...")          — on HTTP-level error.
    ///   Err("transport: ...")         — on network/TLS/DNS failure.
    [[nodiscard]] Result<Json> request(std::string method,
                                       Json        params,
                                       CancelToken ct) override;

    /// POST a JSON-RPC 2.0 notification (fire-and-forget — no response).
    ///
    /// The POST is issued synchronously but the response body is discarded.
    /// Returns Ok when bytes have been sent, Err if the transport is stopped
    /// or the POST fails at the network layer.
    [[nodiscard]] Result<void> notify(std::string method,
                                      Json        params) override;

    // -------------------------------------------------------------------------
    // IMcpTransport — inbound notification dispatch
    // -------------------------------------------------------------------------

    /// Register the callback invoked for every server-initiated notification
    /// received inside a streamable-http SSE response stream.
    ///
    /// Must be called before start(). Replaces any previously registered
    /// handler.
    void on_notification(
        std::function<void(std::string method, Json params)> handler) override;

    // -------------------------------------------------------------------------
    // HttpTransport-specific
    // -------------------------------------------------------------------------

    /// Enable streamable-http mode.  Called by the MCP client layer after
    /// the initialize handshake reveals the server supports it.
    void set_streamable_http(bool enabled);

    /// Returns true when streamable-http mode is active.
    [[nodiscard]] bool streamable_http() const noexcept;

private:
    // -------------------------------------------------------------------------
    // Internal helpers
    // -------------------------------------------------------------------------

    /// Build the cpr::Header for a POST request (Content-Type + user headers).
    [[nodiscard]] cpr::Header build_post_headers() const;

    /// Execute one HTTP POST and return the raw cpr::Response.
    /// Caller holds session_mu_.
    [[nodiscard]] cpr::Response do_post(const std::string& body_str);

    /// Parse a plain JSON-RPC response body.
    [[nodiscard]] Result<Json> parse_plain_response(const cpr::Response& http) const;

    /// Parse a text/event-stream response body, assembling chunks into one
    /// JSON-RPC response.  Also dispatches any notifications found in stream.
    [[nodiscard]] Result<Json> parse_sse_response(const cpr::Response& http);

    // -------------------------------------------------------------------------
    // Configuration (immutable after construction)
    // -------------------------------------------------------------------------
    std::string url_;
    std::unordered_map<std::string, std::string> headers_;

    // -------------------------------------------------------------------------
    // cpr::Session for keep-alive (not thread-safe — guarded by session_mu_)
    // -------------------------------------------------------------------------
    cpr::Session            session_;
    mutable std::mutex      session_mu_;

    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------
    std::atomic<bool> healthy_{false};
    std::atomic<bool> stopped_{false};
    std::atomic<bool> streamable_http_{false};

    // Pending request wakeup: stop() broadcasts to cancel all waiters.
    std::mutex              stop_mu_;
    std::condition_variable stop_cv_;

    // Inbound notification handler (registered before start()).
    std::function<void(std::string, Json)> notif_handler_;
    mutable std::mutex                     notif_mu_;
};

} // namespace batbox::mcp
