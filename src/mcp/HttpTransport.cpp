// src/mcp/HttpTransport.cpp
// ---------------------------------------------------------------------------
// HttpTransport — JSON-RPC 2.0 over HTTP POST implementation.
// See include/batbox/mcp/HttpTransport.hpp for the design contract.
// ---------------------------------------------------------------------------

#include <batbox/mcp/HttpTransport.hpp>

#include <batbox/mcp/JsonRpc.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/core/Logging.hpp>

#include <batbox/inference/SseParser.hpp>

#include <cpr/cpr.h>

#include <cstdint>
#include <string>
#include <utility>

namespace batbox::mcp {

// ============================================================================
// Construction / Destruction
// ============================================================================

HttpTransport::HttpTransport(
    std::string url,
    std::unordered_map<std::string, std::string> headers)
    : url_(std::move(url))
    , headers_(std::move(headers))
{
    // Pre-configure the session URL so keep-alive is associated with the right
    // connection from the first request onward.
    session_.SetUrl(cpr::Url{url_});
}

HttpTransport::~HttpTransport() {
    stop();
}

// ============================================================================
// IMcpTransport — lifecycle
// ============================================================================

Result<void> HttpTransport::start(CancelToken ct) {
    // Idempotent: already healthy → nothing to do.
    if (healthy_.load(std::memory_order_acquire)) {
        return {};
    }
    if (stopped_.load(std::memory_order_acquire)) {
        return Err(std::string("transport stopped"));
    }

    // Quick cancellation check before blocking I/O.
    if (ct.is_cancelled()) {
        return Err(std::string("cancelled"));
    }

    BATBOX_LOG_DEBUG("HttpTransport::start probing {}", url_);

    // Probe the base URL with a HEAD request to verify connectivity.
    // We treat any response (including 4xx/5xx) as "server is reachable".
    cpr::Response probe;
    {
        std::unique_lock<std::mutex> lock(session_mu_);
        // Use a separate one-shot request so we don't disturb the session state.
        probe = cpr::Head(
            cpr::Url{url_},
            cpr::Header{build_post_headers()},
            cpr::Timeout{10'000}
        );
    }

    if (ct.is_cancelled()) {
        return Err(std::string("cancelled"));
    }

    if (probe.error && probe.status_code == 0) {
        // Transport-level failure (DNS, TLS, connection refused).
        // Fall back to a plain GET in case HEAD is not supported.
        std::unique_lock<std::mutex> lock(session_mu_);
        probe = cpr::Get(
            cpr::Url{url_},
            cpr::Header{build_post_headers()},
            cpr::Timeout{10'000}
        );
    }

    if (ct.is_cancelled()) {
        return Err(std::string("cancelled"));
    }

    if (probe.error && probe.status_code == 0) {
        const std::string msg = "transport: " + probe.error.message;
        BATBOX_LOG_WARN("HttpTransport::start probe failed: {}", msg);
        return Err(msg);
    }

    healthy_.store(true, std::memory_order_release);
    BATBOX_LOG_INFO("HttpTransport::start connected to {}", url_);
    return {};
}

void HttpTransport::stop() {
    const bool was_stopped = stopped_.exchange(true, std::memory_order_acq_rel);
    if (was_stopped) {
        return; // already stopped — no-op
    }

    healthy_.store(false, std::memory_order_release);

    // Wake any threads blocked in request() waiting for the stop condition.
    {
        std::unique_lock<std::mutex> lock(stop_mu_);
        stop_cv_.notify_all();
    }

    BATBOX_LOG_DEBUG("HttpTransport::stop called");
}

bool HttpTransport::healthy() const {
    return healthy_.load(std::memory_order_acquire);
}

// ============================================================================
// IMcpTransport — message exchange
// ============================================================================

Result<Json> HttpTransport::request(std::string method,
                                    Json        params,
                                    CancelToken ct) {
    if (stopped_.load(std::memory_order_acquire)) {
        return Err(std::string("transport stopped"));
    }
    if (ct.is_cancelled()) {
        return Err(std::string("cancelled"));
    }

    const int64_t id = next_id();
    const std::string body_str = make_request(id, method, std::move(params)).dump();

    BATBOX_LOG_DEBUG("HttpTransport::request id={} method={}", id, method);

    // Atomically check stopped_ and register our wakeup sentinel before posting.
    {
        std::unique_lock<std::mutex> stop_lock(stop_mu_);
        if (stopped_.load(std::memory_order_acquire)) {
            return Err(std::string("transport stopped"));
        }
        // We hold stop_lock while posting so stop() cannot call notify_all()
        // before we start waiting — but the actual HTTP I/O is synchronous
        // in this lock, so we release it immediately and re-acquire for the wait.
    }

    // ------------------------------------------------------------------
    // Execute the POST (synchronous; releases stop_mu_ first to avoid
    // deadlock with stop() which also acquires stop_mu_).
    // ------------------------------------------------------------------
    cpr::Response http;
    {
        // Register a cancel callback: if ct fires we want to abort the request.
        // cpr::Session has SetCancellationParam; use it to wire ct into cpr.
        auto cancel_flag = std::make_shared<std::atomic_bool>(false);
        auto cancel_reg = ct.on_cancel([cancel_flag]() {
            cancel_flag->store(true, std::memory_order_relaxed);
        });

        std::unique_lock<std::mutex> sess_lock(session_mu_);
        if (stopped_.load(std::memory_order_acquire)) {
            return Err(std::string("transport stopped"));
        }
        if (ct.is_cancelled()) {
            return Err(std::string("cancelled"));
        }

        session_.SetCancellationParam(cancel_flag);
        session_.SetUrl(cpr::Url{url_});
        session_.SetHeader(build_post_headers());
        session_.SetBody(cpr::Body{body_str});
        session_.SetTimeout(cpr::Timeout{30'000});

        http = session_.Post();
    }

    // Check cancellation before inspecting the response.
    if (ct.is_cancelled()) {
        return Err(std::string("cancelled"));
    }
    if (stopped_.load(std::memory_order_acquire)) {
        return Err(std::string("transport stopped"));
    }

    // Transport-level error.
    if (http.error && http.status_code == 0) {
        healthy_.store(false, std::memory_order_release);
        BATBOX_LOG_WARN("HttpTransport::request transport error: {}",
                        http.error.message);
        return Err("transport: " + http.error.message);
    }

    // HTTP-level error.
    if (http.status_code < 200 || http.status_code >= 300) {
        // 405 on HEAD during start() probe is acceptable; here a non-2xx
        // during a real POST is a genuine error.
        const std::string excerpt =
            http.text.size() > 200 ? http.text.substr(0, 200) : http.text;
        BATBOX_LOG_WARN("HttpTransport::request http {}: {}", http.status_code, excerpt);
        return Err("http " + std::to_string(http.status_code) + ": " + excerpt);
    }

    // ------------------------------------------------------------------
    // Parse the response body.
    // ------------------------------------------------------------------
    const auto content_type_it = http.header.find("content-type");
    const bool is_sse =
        streamable_http_.load(std::memory_order_acquire) &&
        content_type_it != http.header.end() &&
        content_type_it->second.find("text/event-stream") != std::string::npos;

    if (is_sse) {
        return parse_sse_response(http);
    }
    return parse_plain_response(http);
}

Result<void> HttpTransport::notify(std::string method, Json params) {
    if (stopped_.load(std::memory_order_acquire)) {
        return Err(std::string("transport stopped"));
    }

    const std::string body_str =
        make_notification(method, std::move(params)).dump();

    BATBOX_LOG_DEBUG("HttpTransport::notify method={}", method);

    std::unique_lock<std::mutex> sess_lock(session_mu_);
    if (stopped_.load(std::memory_order_acquire)) {
        return Err(std::string("transport stopped"));
    }

    session_.SetUrl(cpr::Url{url_});
    session_.SetHeader(build_post_headers());
    session_.SetBody(cpr::Body{body_str});
    session_.SetTimeout(cpr::Timeout{10'000});

    cpr::Response http = session_.Post();

    if (http.error && http.status_code == 0) {
        healthy_.store(false, std::memory_order_release);
        return Err("transport: " + http.error.message);
    }
    // For notifications we only care about network success; ignore HTTP status.
    return {};
}

// ============================================================================
// IMcpTransport — inbound notification dispatch
// ============================================================================

void HttpTransport::on_notification(
    std::function<void(std::string method, Json params)> handler)
{
    std::unique_lock<std::mutex> lock(notif_mu_);
    notif_handler_ = std::move(handler);
}

// ============================================================================
// HttpTransport-specific
// ============================================================================

void HttpTransport::set_streamable_http(bool enabled) {
    streamable_http_.store(enabled, std::memory_order_release);
    BATBOX_LOG_DEBUG("HttpTransport: streamable-http mode {}",
                     enabled ? "enabled" : "disabled");
}

bool HttpTransport::streamable_http() const noexcept {
    return streamable_http_.load(std::memory_order_acquire);
}

// ============================================================================
// Private helpers
// ============================================================================

cpr::Header HttpTransport::build_post_headers() const {
    cpr::Header h;
    h["Content-Type"] = "application/json";
    h["Accept"]       = "application/json, text/event-stream";
    for (const auto& [k, v] : headers_) {
        h[k] = v;
    }
    return h;
}

cpr::Response HttpTransport::do_post(const std::string& body_str) {
    // Caller must hold session_mu_.
    session_.SetUrl(cpr::Url{url_});
    session_.SetHeader(build_post_headers());
    session_.SetBody(cpr::Body{body_str});
    session_.SetTimeout(cpr::Timeout{30'000});
    return session_.Post();
}

Result<Json> HttpTransport::parse_plain_response(const cpr::Response& http) const {
    auto parse_res = batbox::parse(http.text);
    if (!parse_res) {
        return Err("parse: " + parse_res.error());
    }
    const Json& root = parse_res.value();

    auto msg_res = parse_message(root);
    if (!msg_res) {
        return Err("jsonrpc: " + msg_res.error());
    }

    return std::visit(
        [](auto&& msg) -> Result<Json> {
            using T = std::decay_t<decltype(msg)>;
            if constexpr (std::is_same_v<T, JsonRpcResponse>) {
                if (msg.error) {
                    return Err(std::to_string(msg.error->code) + ": " +
                               msg.error->message);
                }
                return msg.result.value_or(Json(nullptr));
            } else {
                return Err(std::string("unexpected message type in HTTP response"));
            }
        },
        std::move(msg_res.value()));
}

Result<Json> HttpTransport::parse_sse_response(const cpr::Response& http) {
    // The entire SSE body is buffered in http.text (cpr collected it all).
    // Feed it to SseParser to extract individual events.
    batbox::inference::SseParser parser;
    auto feed_res = parser.feed(http.text);
    if (!feed_res) {
        return Err("sse-parse: " + feed_res.error());
    }

    // Gather the notification handler snapshot under lock.
    std::function<void(std::string, Json)> notif_fn;
    {
        std::unique_lock<std::mutex> lock(notif_mu_);
        notif_fn = notif_handler_;
    }

    // Process events: notifications are dispatched; the first response is
    // returned to the caller.
    std::optional<Result<Json>> response_result;

    for (const auto& ev : feed_res.value()) {
        if (ev.is_done || ev.data.empty()) {
            continue;
        }

        auto jres = batbox::parse(ev.data);
        if (!jres) {
            // Malformed JSON in an SSE chunk — skip gracefully.
            BATBOX_LOG_WARN("HttpTransport: SSE chunk parse error: {}", jres.error());
            continue;
        }

        auto msg_res = parse_message(jres.value());
        if (!msg_res) {
            BATBOX_LOG_WARN("HttpTransport: SSE jsonrpc error: {}", msg_res.error());
            continue;
        }

        std::visit(
            [&](auto&& msg) {
                using T = std::decay_t<decltype(msg)>;
                if constexpr (std::is_same_v<T, JsonRpcResponse>) {
                    if (!response_result.has_value()) {
                        if (msg.error) {
                            response_result = Err(
                                std::to_string(msg.error->code) + ": " +
                                msg.error->message);
                        } else {
                            response_result = msg.result.value_or(Json(nullptr));
                        }
                    }
                } else if constexpr (std::is_same_v<T, JsonRpcNotification>) {
                    if (notif_fn) {
                        notif_fn(msg.method, msg.params);
                    }
                }
                // JsonRpcRequest from server is unexpected in this direction — ignore.
            },
            std::move(msg_res.value()));
    }

    if (!response_result.has_value()) {
        return Err(std::string("no JSON-RPC response found in SSE stream"));
    }
    return std::move(*response_result);
}

} // namespace batbox::mcp
