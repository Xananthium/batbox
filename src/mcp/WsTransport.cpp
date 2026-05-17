// src/mcp/WsTransport.cpp
// ---------------------------------------------------------------------------
// WsTransport — IXWebSocket-based MCP transport implementation.
//
// Key design decisions:
//   • Auto-reconnect disabled (ix::WebSocket::disableAutomaticReconnection()).
//     McpClient owns the reconnect policy.
//   • Handshake uses ix::WebSocket::connect() (synchronous, 10-second timeout)
//     followed by ix::WebSocket::start() (launches the background I/O thread).
//   • Pending requests are tracked in a map<id, PendingEntry>.  Each entry
//     holds an optional<Result<Json>> filled by the on_message callback and a
//     condition_variable that the waiting request() call sleeps on.
//   • On Close or Error events, cancel_pending() is called to unblock all
//     waiters with Err("transport disconnected").
//   • stop() calls ws_.stop() (graceful close, joins the background thread)
//     then calls cancel_pending("transport stopped") for any remaining waiters.
// ---------------------------------------------------------------------------

#include <batbox/mcp/WsTransport.hpp>
#include <batbox/mcp/JsonRpc.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/core/Logging.hpp>

#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketMessage.h>
#include <ixwebsocket/IXWebSocketMessageType.h>
#include <ixwebsocket/IXSocketTLSOptions.h>

#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

namespace batbox::mcp {

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

WsTransport::WsTransport(
    std::string url,
    std::unordered_map<std::string, std::string> headers,
    std::string ca_file_path)
    : url_(std::move(url))
    , headers_(std::move(headers))
    , ca_file_path_(std::move(ca_file_path))
{
    // Disable automatic reconnect — McpClient owns reconnect policy.
    ws_.disableAutomaticReconnection();
    ws_.setUrl(url_);

    // Apply extra handshake headers.
    if (!headers_.empty()) {
        ix::WebSocketHttpHeaders ix_headers;
        for (auto& [k, v] : headers_) {
            ix_headers[k] = v;
        }
        ws_.setExtraHeaders(ix_headers);
    }

    // TLS options — only meaningful for wss:// URLs.
    if (!ca_file_path_.empty()) {
        ix::SocketTLSOptions tls;
        tls.caFile = ca_file_path_;
        ws_.setTLSOptions(tls);
    }

    // Register the message callback.  Captures `this` — safe because the
    // ix::WebSocket instance is owned by *this and the callback is cleared
    // (via ws_.stop()) before the destructor completes.
    ws_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        if (!msg) return;

        switch (msg->type) {
        case ix::WebSocketMessageType::Open: {
            std::lock_guard<std::mutex> lk(connect_mu_);
            healthy_.store(true, std::memory_order_release);
            connect_result_ = true;
            connect_cv_.notify_all();
            BATBOX_LOG_INFO("WsTransport: connected to {}", url_);
            break;
        }

        case ix::WebSocketMessageType::Close: {
            BATBOX_LOG_WARN("WsTransport: connection closed (code={} reason={})",
                            msg->closeInfo.code, msg->closeInfo.reason);
            healthy_.store(false, std::memory_order_release);
            // Unblock a pending start() if still waiting.
            {
                std::lock_guard<std::mutex> lk(connect_mu_);
                if (!connect_result_.has_value()) {
                    connect_result_ = false;
                    connect_error_  = "connection closed during handshake";
                    connect_cv_.notify_all();
                }
            }
            cancel_pending("transport disconnected");
            break;
        }

        case ix::WebSocketMessageType::Error: {
            BATBOX_LOG_ERROR("WsTransport: error — {}", msg->errorInfo.reason);
            healthy_.store(false, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lk(connect_mu_);
                if (!connect_result_.has_value()) {
                    connect_result_ = false;
                    connect_error_  = msg->errorInfo.reason;
                    connect_cv_.notify_all();
                }
            }
            cancel_pending("transport disconnected");
            break;
        }

        case ix::WebSocketMessageType::Message: {
            if (!msg->binary) {
                handle_message(msg->str);
            }
            break;
        }

        default:
            // Ping / Pong / Fragment — handled by IXWebSocket internally.
            break;
        }
    });
}

WsTransport::~WsTransport() {
    // Ensure the background thread is stopped before we are destroyed.
    if (!stopped_.load(std::memory_order_acquire)) {
        ws_.stop();
        cancel_pending("transport stopped");
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

Result<void> WsTransport::start(CancelToken ct) {
    // Idempotent on an already-healthy transport.
    if (healthy_.load(std::memory_order_acquire)) {
        return {};
    }

    if (stopped_.load(std::memory_order_acquire)) {
        return Err(std::string("transport stopped"));
    }

    // Reset handshake state.
    {
        std::lock_guard<std::mutex> lk(connect_mu_);
        connect_result_.reset();
        connect_error_.clear();
    }

    // Start the IXWebSocket background I/O thread (initiates the connection).
    ws_.start();

    // Register a cancellation callback that wakes the cv.
    auto cancel_handle = ct.on_cancel([this] {
        std::lock_guard<std::mutex> lk(connect_mu_);
        if (!connect_result_.has_value()) {
            connect_result_ = false;
            connect_error_  = "cancelled";
            connect_cv_.notify_all();
        }
    });

    // Wait for Open, Close/Error, or cancellation.
    std::unique_lock<std::mutex> lk(connect_mu_);
    connect_cv_.wait(lk, [this] { return connect_result_.has_value(); });

    bool ok = connect_result_.value_or(false);
    if (!ok) {
        std::string err = connect_error_.empty() ? "connection failed" : connect_error_;
        // Stop the background thread if the handshake failed.
        if (!healthy_.load(std::memory_order_acquire)) {
            ws_.stop();
        }
        if (err == "cancelled") {
            return Err(std::string("cancelled"));
        }
        return Err(err);
    }

    return {};
}

void WsTransport::stop() {
    if (stopped_.exchange(true, std::memory_order_acq_rel)) {
        return; // Already stopped.
    }

    healthy_.store(false, std::memory_order_release);
    ws_.stop();
    cancel_pending("transport stopped");
}

bool WsTransport::healthy() const {
    return healthy_.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// Message exchange
// ---------------------------------------------------------------------------

Result<Json> WsTransport::request(std::string method,
                                   Json        params,
                                   CancelToken ct) {
    if (stopped_.load(std::memory_order_acquire)) {
        return Err(std::string("transport stopped"));
    }
    if (!healthy_.load(std::memory_order_acquire)) {
        return Err(std::string("transport disconnected"));
    }

    // Assign a monotonic id and build the JSON-RPC request.
    int64_t id  = next_id();
    Json    req = make_request(id, method, std::move(params));

    // Register the pending entry before sending.
    auto entry = std::make_shared<PendingEntry>();
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        pending_[id] = entry;
    }

    // Serialise and send.
    std::string payload = req.dump();
    auto send_info = ws_.sendText(payload);
    if (!send_info.success) {
        std::lock_guard<std::mutex> lk(pending_mu_);
        pending_.erase(id);
        return Err(std::string("send failed"));
    }

    // Register cancellation callback.
    auto cancel_handle = ct.on_cancel([entry] {
        std::lock_guard<std::mutex> lk(entry->mu);
        if (!entry->result.has_value()) {
            entry->result = Err(std::string("cancelled"));
            entry->cv.notify_all();
        }
    });

    // Wait for the response.
    {
        std::unique_lock<std::mutex> lk(entry->mu);
        entry->cv.wait(lk, [&entry] { return entry->result.has_value(); });
    }

    // Remove from pending map.
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        pending_.erase(id);
    }

    return std::move(*entry->result);
}

Result<void> WsTransport::notify(std::string method, Json params) {
    if (stopped_.load(std::memory_order_acquire)) {
        return Err(std::string("transport stopped"));
    }
    if (!healthy_.load(std::memory_order_acquire)) {
        return Err(std::string("transport disconnected"));
    }

    Json notif = make_notification(method, std::move(params));
    std::string payload = notif.dump();

    auto info = ws_.sendText(payload);
    if (!info.success) {
        return Err(std::string("send failed"));
    }
    return {};
}

// ---------------------------------------------------------------------------
// Inbound notification dispatch
// ---------------------------------------------------------------------------

void WsTransport::on_notification(
    std::function<void(std::string method, Json params)> handler)
{
    std::lock_guard<std::mutex> lk(notif_mu_);
    notif_handler_ = std::move(handler);
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

void WsTransport::handle_message(const std::string& text) {
    // Parse the raw JSON text.
    auto parse_res = batbox::parse(text);
    if (!parse_res) {
        BATBOX_LOG_WARN("WsTransport: failed to parse frame: {}", parse_res.error());
        return;
    }
    const Json& j = *parse_res;

    // Dispatch via parse_message.
    auto msg_res = parse_message(j);
    if (!msg_res) {
        BATBOX_LOG_WARN("WsTransport: unrecognised JSON-RPC frame: {}", msg_res.error());
        return;
    }

    std::visit([this](auto&& m) {
        using T = std::decay_t<decltype(m)>;

        if constexpr (std::is_same_v<T, JsonRpcResponse>) {
            // Route to the pending request by id.
            int64_t id = 0;
            if (std::holds_alternative<int64_t>(m.id)) {
                id = std::get<int64_t>(m.id);
            } else {
                // String ids: not used by this transport (we always send int ids).
                BATBOX_LOG_WARN("WsTransport: received response with string id — ignored");
                return;
            }

            std::shared_ptr<PendingEntry> entry;
            {
                std::lock_guard<std::mutex> lk(pending_mu_);
                auto it = pending_.find(id);
                if (it == pending_.end()) {
                    BATBOX_LOG_WARN("WsTransport: response for unknown id={}", id);
                    return;
                }
                entry = it->second;
            }

            Result<Json> result = [&]() -> Result<Json> {
                if (m.error.has_value()) {
                    std::ostringstream oss;
                    oss << m.error->code << ": " << m.error->message;
                    return Err(oss.str());
                }
                if (m.result.has_value()) {
                    return *m.result;
                }
                return Err(std::string("response has neither result nor error"));
            }();

            std::lock_guard<std::mutex> lk(entry->mu);
            if (!entry->result.has_value()) {
                entry->result = std::move(result);
                entry->cv.notify_all();
            }

        } else if constexpr (std::is_same_v<T, JsonRpcNotification>) {
            // Forward to registered handler.
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
            BATBOX_LOG_WARN("WsTransport: received unexpected server-side request "
                            "method={}", m.method);
        }
    }, *msg_res);
}

void WsTransport::cancel_pending(const std::string& reason) {
    std::unordered_map<int64_t, std::shared_ptr<PendingEntry>> snapshot;
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        snapshot = pending_;
        pending_.clear();
    }

    for (auto& [id, entry] : snapshot) {
        std::lock_guard<std::mutex> lk(entry->mu);
        if (!entry->result.has_value()) {
            entry->result = Err(std::string(reason));
            entry->cv.notify_all();
        }
    }
}

} // namespace batbox::mcp
