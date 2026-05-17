// src/mcp/SseTransport.cpp
// ---------------------------------------------------------------------------
// SseTransport — MCP transport over Server-Sent Events.
// See include/batbox/mcp/SseTransport.hpp for design notes.
//
// MCP SSE protocol flow:
//   1. Background thread issues GET /sse with cpr::Download + WriteCallback.
//   2. WriteCallback feeds bytes into SseParser on each received chunk.
//   3. First SSE event (event: endpoint) provides the POST URL.
//      start() unblocks once the endpoint URL is received.
//   4. Subsequent SSE events (event: message) carry JSON-RPC envelopes.
//      Responses are routed to pending requests by id.
//      Notifications are forwarded to the registered handler.
//   5. POSTing client→server: cpr::Post to post_url_ with JSON-RPC body.
//      Server responds 202 Accepted; actual response arrives over SSE.
//   6. stop(): sets stopped_ atomic → WriteCallback returns false (aborts cpr
//      transfer), reader_thread_ joins, pending requests cancelled.
// ---------------------------------------------------------------------------

#include <batbox/mcp/SseTransport.hpp>
#include <batbox/mcp/JsonRpc.hpp>
#include <batbox/inference/SseParser.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/core/Logging.hpp>

#include <cpr/cpr.h>

#include <chrono>
#include <sstream>
#include <string>
#include <utility>

namespace batbox::mcp {

// ============================================================================
// Construction / Destruction
// ============================================================================

SseTransport::SseTransport(
    std::string url,
    std::unordered_map<std::string, std::string> headers)
    : sse_url_(std::move(url))
    , headers_(std::move(headers))
{}

SseTransport::~SseTransport() {
    if (!stopped_.load(std::memory_order_acquire)) {
        stop();
    }
}

// ============================================================================
// on_notification — must be called before start()
// ============================================================================

void SseTransport::on_notification(
    std::function<void(std::string method, Json params)> handler)
{
    std::lock_guard<std::mutex> lk(notif_mu_);
    notif_handler_ = std::move(handler);
}

// ============================================================================
// healthy
// ============================================================================

bool SseTransport::healthy() const {
    return healthy_.load(std::memory_order_acquire);
}

// ============================================================================
// start
// ============================================================================

Result<void> SseTransport::start(CancelToken ct) {
    // Idempotent: already healthy.
    if (healthy_.load(std::memory_order_acquire)) {
        return {};
    }
    if (stopped_.load(std::memory_order_acquire)) {
        return batbox::Err(std::string("transport stopped"));
    }

    // Reset handshake state.
    {
        std::lock_guard<std::mutex> lk(connect_mu_);
        connect_result_.reset();
        connect_error_.clear();
    }

    // Spawn the background SSE reader thread.
    reader_thread_ = std::thread(&SseTransport::reader_loop, this);

    // Register cancellation: wake the connect_cv_ so start() returns promptly.
    auto cancel_handle = ct.on_cancel([this] {
        std::lock_guard<std::mutex> lk(connect_mu_);
        if (!connect_result_.has_value()) {
            connect_result_ = false;
            connect_error_  = "cancelled";
            connect_cv_.notify_all();
        }
        // Also set stopped_ so the reader loop aborts.
        stopped_.store(true, std::memory_order_release);
    });

    // Block until the endpoint URL is received, an error occurs, or cancellation.
    {
        std::unique_lock<std::mutex> lk(connect_mu_);
        connect_cv_.wait(lk, [this] { return connect_result_.has_value(); });
    }

    bool ok = connect_result_.value_or(false);
    if (!ok) {
        std::string err = connect_error_.empty() ? "SSE connection failed" : connect_error_;
        // Join the reader thread before returning error.
        if (reader_thread_.joinable()) {
            reader_thread_.join();
        }
        if (err == "cancelled") {
            return batbox::Err(std::string("cancelled"));
        }
        return batbox::Err(err);
    }

    return {};
}

// ============================================================================
// stop
// ============================================================================

void SseTransport::stop() {
    // Idempotent.
    if (stopped_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    healthy_.store(false, std::memory_order_release);

    // Cancel all pending requests so their waiting threads unblock.
    cancel_pending("transport stopped");

    // Wake any in-progress start() call.
    {
        std::lock_guard<std::mutex> lk(connect_mu_);
        if (!connect_result_.has_value()) {
            connect_result_ = false;
            connect_error_  = "transport stopped";
            connect_cv_.notify_all();
        }
    }

    // The WriteCallback checks stopped_ and returns false, which aborts the
    // cpr::Download call. Join the reader thread.
    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }
}

// ============================================================================
// request
// ============================================================================

Result<Json> SseTransport::request(std::string method,
                                    Json        params,
                                    CancelToken ct) {
    if (stopped_.load(std::memory_order_acquire)) {
        return batbox::Err(std::string("transport stopped"));
    }
    if (!healthy_.load(std::memory_order_acquire)) {
        return batbox::Err(std::string("transport disconnected"));
    }
    if (ct.is_cancelled()) {
        return batbox::Err(std::string("cancelled"));
    }

    // Assign a monotonic id and build the JSON-RPC request.
    int64_t id  = next_id();
    Json    req = make_request(id, method, std::move(params));

    // Register the pending entry BEFORE sending (avoid race where response
    // arrives before the entry is in the map).
    auto entry = std::make_shared<PendingEntry>();
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        pending_[id] = entry;
    }

    // POST the request to the server endpoint URL.
    std::string payload = req.dump();
    {
        std::string current_post_url;
        {
            std::lock_guard<std::mutex> lk(post_url_mu_);
            current_post_url = post_url_;
        }

        cpr::Header req_headers{{"Content-Type", "application/json"}};
        for (auto& [k, v] : headers_) {
            req_headers.insert({k, v});
        }

        cpr::Response post_resp = cpr::Post(
            cpr::Url{current_post_url},
            std::move(req_headers),
            cpr::Body{payload}
        );

        if (post_resp.error) {
            std::lock_guard<std::mutex> lk(pending_mu_);
            pending_.erase(id);
            return batbox::Err("post failed: " + post_resp.error.message);
        }

        // MCP servers must respond with 202 Accepted.
        if (post_resp.status_code != 202) {
            std::lock_guard<std::mutex> lk(pending_mu_);
            pending_.erase(id);
            const std::string excerpt =
                post_resp.text.size() > 200
                    ? post_resp.text.substr(0, 200)
                    : post_resp.text;
            return batbox::Err(
                "post http " + std::to_string(post_resp.status_code) + ": " + excerpt);
        }
    }

    // Register cancellation callback.
    auto cancel_handle = ct.on_cancel([entry] {
        std::lock_guard<std::mutex> lk(entry->mu);
        if (!entry->result.has_value()) {
            entry->result = batbox::Err(std::string("cancelled"));
            entry->cv.notify_all();
        }
    });

    // Wait for the response to arrive over the SSE stream.
    {
        std::unique_lock<std::mutex> lk(entry->mu);
        entry->cv.wait(lk, [&entry] { return entry->result.has_value(); });
    }

    // Clean up the pending entry.
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        pending_.erase(id);
    }

    return std::move(*entry->result);
}

// ============================================================================
// notify
// ============================================================================

Result<void> SseTransport::notify(std::string method, Json params) {
    if (stopped_.load(std::memory_order_acquire)) {
        return batbox::Err(std::string("transport stopped"));
    }
    if (!healthy_.load(std::memory_order_acquire)) {
        return batbox::Err(std::string("transport disconnected"));
    }

    Json notif  = make_notification(method, std::move(params));
    std::string payload = notif.dump();

    std::string current_post_url;
    {
        std::lock_guard<std::mutex> lk(post_url_mu_);
        current_post_url = post_url_;
    }

    cpr::Header req_headers{{"Content-Type", "application/json"}};
    for (auto& [k, v] : headers_) {
        req_headers.insert({k, v});
    }

    cpr::Response post_resp = cpr::Post(
        cpr::Url{current_post_url},
        std::move(req_headers),
        cpr::Body{payload}
    );

    if (post_resp.error) {
        return batbox::Err("post failed: " + post_resp.error.message);
    }

    if (post_resp.status_code != 202) {
        const std::string excerpt =
            post_resp.text.size() > 200
                ? post_resp.text.substr(0, 200)
                : post_resp.text;
        return batbox::Err(
            "post http " + std::to_string(post_resp.status_code) + ": " + excerpt);
    }

    return {};
}

// ============================================================================
// Private: reader_loop
// ============================================================================

void SseTransport::reader_loop() {
    batbox::inference::SseParser parser;

    // Build cpr headers from our headers_ map.
    cpr::Header cpr_headers{{"Accept", "text/event-stream"}};
    for (auto& [k, v] : headers_) {
        cpr_headers.insert({k, v});
    }

    // Issue the persistent GET /sse. The WriteCallback is invoked for each
    // received chunk and feeds bytes into the SseParser. Returns false to abort.
    cpr::Response sse_resp = cpr::Download(
        cpr::WriteCallback{[this, &parser](std::string_view data, intptr_t) -> bool {
            if (stopped_.load(std::memory_order_acquire)) {
                return false; // Abort the transfer.
            }

            auto feed_result = parser.feed(data);
            if (!feed_result) {
                BATBOX_LOG_ERROR("SseTransport: SSE parser overflow: {}",
                                 feed_result.error());
                // Signal start() failure.
                {
                    std::lock_guard<std::mutex> lk(connect_mu_);
                    if (!connect_result_.has_value()) {
                        connect_result_ = false;
                        connect_error_  = "SSE parser overflow";
                        connect_cv_.notify_all();
                    }
                }
                return false; // Abort.
            }

            for (auto& ev : *feed_result) {
                if (stopped_.load(std::memory_order_acquire)) {
                    return false;
                }
                handle_event(ev.event, ev.data);
            }

            return true; // Continue receiving.
        }},
        cpr::Url{sse_url_},
        std::move(cpr_headers)
    );

    // The Download call returned — either normally (stream ended) or aborted.
    // Mark transport as unhealthy if it wasn't already stopped externally.
    healthy_.store(false, std::memory_order_release);

    // Signal any still-waiting start() call (connection dropped before endpoint).
    {
        std::lock_guard<std::mutex> lk(connect_mu_);
        if (!connect_result_.has_value()) {
            connect_result_ = false;
            if (sse_resp.error) {
                connect_error_ = sse_resp.error.message;
            } else if (sse_resp.status_code != 200) {
                connect_error_ = "http " + std::to_string(sse_resp.status_code);
            } else {
                connect_error_ = "SSE stream ended before endpoint event";
            }
            connect_cv_.notify_all();
        }
    }

    // Cancel any pending request() calls with "transport disconnected".
    if (!stopped_.load(std::memory_order_acquire)) {
        cancel_pending("transport disconnected");
    }

    BATBOX_LOG_INFO("SseTransport: reader loop exited (url={})", sse_url_);
}

// ============================================================================
// Private: handle_event
// ============================================================================

void SseTransport::handle_event(const std::string& event_type,
                                 const std::string& data) {
    if (event_type == "endpoint") {
        // The server has told us where to POST requests.
        // The data may be a relative path or a full URL.
        std::string resolved_post_url;
        if (!data.empty() && data[0] == '/') {
            // Relative path — resolve against the SSE base URL.
            // Extract scheme + host from sse_url_:
            //   "http://host:port/sse" → "http://host:port"
            auto path_start = sse_url_.find("://");
            if (path_start != std::string::npos) {
                auto host_end = sse_url_.find('/', path_start + 3);
                if (host_end != std::string::npos) {
                    resolved_post_url = sse_url_.substr(0, host_end) + data;
                } else {
                    // No path in sse_url_ — just append.
                    resolved_post_url = sse_url_ + data;
                }
            } else {
                resolved_post_url = data;
            }
        } else {
            resolved_post_url = data;
        }

        BATBOX_LOG_INFO("SseTransport: endpoint URL received: {}", resolved_post_url);

        {
            std::lock_guard<std::mutex> lk(post_url_mu_);
            post_url_ = resolved_post_url;
        }

        healthy_.store(true, std::memory_order_release);

        // Unblock start().
        {
            std::lock_guard<std::mutex> lk(connect_mu_);
            if (!connect_result_.has_value()) {
                connect_result_ = true;
                connect_cv_.notify_all();
            }
        }

    } else if (event_type == "message" || event_type.empty()) {
        // JSON-RPC payload.
        if (data.empty()) {
            return;
        }

        auto parse_res = batbox::parse(data);
        if (!parse_res) {
            BATBOX_LOG_WARN("SseTransport: failed to parse SSE data as JSON: {}",
                            parse_res.error());
            return;
        }

        auto msg_res = parse_message(*parse_res);
        if (!msg_res) {
            BATBOX_LOG_WARN("SseTransport: unrecognised JSON-RPC message: {}",
                            msg_res.error());
            return;
        }

        std::visit([this](auto&& m) {
            using T = std::decay_t<decltype(m)>;

            if constexpr (std::is_same_v<T, JsonRpcResponse>) {
                // Route response to pending request by id.
                int64_t resp_id = 0;
                if (std::holds_alternative<int64_t>(m.id)) {
                    resp_id = std::get<int64_t>(m.id);
                } else {
                    BATBOX_LOG_WARN("SseTransport: received response with string id — ignored");
                    return;
                }

                std::shared_ptr<PendingEntry> entry;
                {
                    std::lock_guard<std::mutex> lk(pending_mu_);
                    auto it = pending_.find(resp_id);
                    if (it == pending_.end()) {
                        BATBOX_LOG_WARN("SseTransport: response for unknown id={}", resp_id);
                        return;
                    }
                    entry = it->second;
                }

                Result<Json> result = [&]() -> Result<Json> {
                    if (m.error.has_value()) {
                        std::ostringstream oss;
                        oss << m.error->code << ": " << m.error->message;
                        return batbox::Err(oss.str());
                    }
                    if (m.result.has_value()) {
                        return *m.result;
                    }
                    return batbox::Err(std::string("response has neither result nor error"));
                }();

                std::lock_guard<std::mutex> lk(entry->mu);
                if (!entry->result.has_value()) {
                    entry->result = std::move(result);
                    entry->cv.notify_all();
                }

            } else if constexpr (std::is_same_v<T, JsonRpcNotification>) {
                std::function<void(std::string, Json)> handler;
                {
                    std::lock_guard<std::mutex> lk(notif_mu_);
                    handler = notif_handler_;
                }
                if (handler) {
                    handler(std::move(m.method), std::move(m.params));
                }

            } else if constexpr (std::is_same_v<T, JsonRpcRequest>) {
                // MCP servers do not send requests to the client.
                BATBOX_LOG_WARN("SseTransport: received unexpected server-side "
                                "request method={}", m.method);
            }
        }, *msg_res);
    } else {
        BATBOX_LOG_WARN("SseTransport: unknown SSE event type: {}", event_type);
    }
}
// ============================================================================
// Private: cancel_pending
// ============================================================================

void SseTransport::cancel_pending(const std::string& reason) {
    std::unordered_map<int64_t, std::shared_ptr<PendingEntry>> snapshot;
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        snapshot = pending_;
        pending_.clear();
    }

    for (auto& [id, entry] : snapshot) {
        std::lock_guard<std::mutex> lk(entry->mu);
        if (!entry->result.has_value()) {
            entry->result = batbox::Err(std::string(reason));
            entry->cv.notify_all();
        }
    }
}

} // namespace batbox::mcp
