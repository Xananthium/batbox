// src/mcp/McpClient.cpp
// ---------------------------------------------------------------------------
// McpClient — orchestrator implementation.
//
// See include/batbox/mcp/McpClient.hpp for the full design contract.
// ---------------------------------------------------------------------------

#include <batbox/mcp/McpClient.hpp>
#include <batbox/mcp/JsonRpc.hpp>
#include <batbox/core/Logging.hpp>

#include <future>
#include <sstream>
#include <thread>
#include <vector>

namespace batbox::mcp {

// ============================================================================
// Client info constants sent in the initialize request
// ============================================================================

static constexpr std::string_view kClientName    = "batbox";
static constexpr std::string_view kClientVersion = "1.0.0";
static constexpr std::string_view kMcpVersion    = "2024-11-05";

// ============================================================================
// Construction
// ============================================================================

McpClient::McpClient(McpServerRegistry& registry) noexcept
    : registry_(registry)
{}

// ============================================================================
// Initialization
// ============================================================================

Result<void> McpClient::initialize_all(CancelToken ct) {
    auto names = registry_.server_names();
    if (names.empty()) return {};

    // Launch one thread per server, each getting a child token so we can cancel
    // the whole batch if the parent ct fires.
    struct Outcome {
        std::string server;
        std::string error;
    };

    std::vector<std::future<Outcome>> futures;
    futures.reserve(names.size());

    for (const auto& name : names) {
        auto [child_src, child_tok] = ct.child();
        futures.push_back(std::async(std::launch::async,
            [this, name, child_src = std::move(child_src),
             child_tok = std::move(child_tok)]() mutable -> Outcome {
                auto* transport = registry_.get(name);
                if (!transport) {
                    return {name, "transport not found"};
                }
                auto r = do_initialize(name, *transport, std::move(child_tok));
                if (!r) return {name, r.error()};
                return {name, {}};
            }
        ));
    }

    // Collect results.
    std::string errors;
    for (auto& fut : futures) {
        auto outcome = fut.get();
        if (!outcome.error.empty()) {
            if (!errors.empty()) errors += ", ";
            errors += outcome.server + ": " + outcome.error;
            SPDLOG_WARN("McpClient: initialize failed for '{}': {}",
                        outcome.server, outcome.error);
        }
    }

    if (!errors.empty()) return Err(errors);
    return {};
}

Result<void> McpClient::initialize_one(std::string_view server, CancelToken ct) {
    auto* transport = registry_.get(server);
    if (!transport) {
        return Err(std::string("unknown server: ") + std::string(server));
    }
    return do_initialize(server, *transport, std::move(ct));
}

// ============================================================================
// do_initialize — per-transport MCP handshake
// ============================================================================

Result<void> McpClient::do_initialize(std::string_view server,
                                       IMcpTransport&   transport,
                                       CancelToken      ct) {
    // Build the initialize request params per MCP spec 2024-11-05.
    Json params = {
        {"protocolVersion", kMcpVersion},
        {"clientInfo", {
            {"name",    kClientName},
            {"version", kClientVersion}
        }},
        {"capabilities", {
            {"roots",    {{"listChanged", true}}},
            {"sampling", Json::object()}
        }}
    };

    // Register the inbound notification handler BEFORE sending initialize.
    // This must be done before start() per IMcpTransport contract — but since
    // we get called after start_all(), the transport is already running.
    // on_notification() replaces any prior handler so calling it now is safe.
    std::string server_name{server};
    transport.on_notification([this, server_name](std::string method, Json /*params*/) {
        if (method == "notifications/tools/list_changed") {
            SPDLOG_DEBUG("McpClient: tools/list_changed from '{}'", server_name);
            std::lock_guard<std::mutex> lk(cache_mu_);
            cache_valid_[server_name] = false;
        }
    });

    // Send the initialize request.
    auto resp = transport.request("initialize", std::move(params), std::move(ct));
    if (!resp) {
        return Err(resp.error());
    }

    // Parse capabilities from the response.
    ServerCapabilities caps;
    caps.raw = *resp;

    if (resp->contains("capabilities")) {
        const auto& c = (*resp)["capabilities"];
        caps.tools     = c.contains("tools");
        caps.resources = c.contains("resources");
        caps.prompts   = c.contains("prompts");
    }

    {
        std::lock_guard<std::mutex> lk(caps_mu_);
        caps_[server_name] = std::move(caps);
    }

    // Send the initialized notification (fire-and-forget; spec requires it).
    auto nr = transport.notify("notifications/initialized", Json::object());
    if (!nr) {
        // Non-fatal: log but do not fail initialization.
        SPDLOG_WARN("McpClient: failed to send initialized notification to '{}': {}",
                    server_name, nr.error());
    }

    SPDLOG_DEBUG("McpClient: initialized '{}' — tools={} resources={} prompts={}",
                 server_name, caps.tools, caps.resources, caps.prompts);
    return {};
}

// ============================================================================
// Transport resolution with reconnect
// ============================================================================

IMcpTransport* McpClient::resolve_transport(std::string_view server,
                                             std::string&     out_err) {
    auto* transport = registry_.get(server);
    if (!transport) {
        out_err = std::string("unknown server: ") + std::string(server);
        return nullptr;
    }
    if (!transport->healthy()) {
        // Attempt one reconnect via the registry.
        auto [src, tok] = CancelToken::make_root();
        auto r = registry_.restart(server, src.token());
        if (!r) {
            out_err = std::string("transport unhealthy and reconnect failed: ") + r.error();
            return nullptr;
        }
        SPDLOG_INFO("McpClient: reconnected '{}'", server);
    }
    return transport;
}

// ============================================================================
// dispatch — issues one JSON-RPC call with reconnect-on-failure
// ============================================================================

Result<Json> McpClient::dispatch(std::string_view server,
                                  std::string      method,
                                  Json             params,
                                  CancelToken      ct) {
    std::string err;
    auto* transport = resolve_transport(server, err);
    if (!transport) return Err(err);

    auto result = transport->request(method, std::move(params), std::move(ct));
    if (result) return result;

    // One reconnect attempt on transport error (not on cancellation).
    if (result.error() == "cancelled") return result;

    SPDLOG_WARN("McpClient: dispatch '{}' to '{}' failed ({}); reconnecting",
                method, server, result.error());

    auto [src, tok] = CancelToken::make_root();
    auto rr = registry_.restart(server, src.token());
    if (!rr) {
        return Err(std::string("transport error and reconnect failed: ") + rr.error());
    }

    // Re-initialize after reconnect.
    auto* t2 = registry_.get(server);
    if (!t2) return Err(std::string("transport vanished after reconnect"));

    auto [src2, tok2] = CancelToken::make_root();
    auto ir = do_initialize(server, *t2, src2.token());
    if (!ir) {
        SPDLOG_WARN("McpClient: re-initialize after reconnect failed for '{}': {}",
                    server, ir.error());
    }

    // Retry the original request once.
    auto [src3, tok3] = CancelToken::make_root();
    return t2->request(std::move(method), std::move(params), src3.token());
}

// ============================================================================
// Tools
// ============================================================================

Result<Json> McpClient::tools_list(std::string_view server,
                                    CancelToken       ct,
                                    bool              force_refresh) {
    std::string sname{server};

    // Check cache.
    if (!force_refresh) {
        std::lock_guard<std::mutex> lk(cache_mu_);
        auto it = cache_valid_.find(sname);
        if (it != cache_valid_.end() && it->second) {
            return tools_cache_.at(sname);
        }
    }

    auto result = dispatch(server, "tools/list", Json::object(), std::move(ct));
    if (!result) return result;

    // Populate cache.
    {
        std::lock_guard<std::mutex> lk(cache_mu_);
        tools_cache_[sname]  = *result;
        cache_valid_[sname]  = true;
    }

    return result;
}

Result<Json> McpClient::tools_call(std::string_view server,
                                    std::string_view tool_name,
                                    const Json&      args,
                                    CancelToken      ct) {
    Json params = {
        {"name",      tool_name},
        {"arguments", args}
    };
    return dispatch(server, "tools/call", std::move(params), std::move(ct));
}

// ============================================================================
// Resources
// ============================================================================

Result<Json> McpClient::resources_list(std::string_view server, CancelToken ct) {
    return dispatch(server, "resources/list", Json::object(), std::move(ct));
}

Result<Json> McpClient::resources_read(std::string_view server,
                                        std::string_view uri,
                                        CancelToken      ct) {
    Json params = {{"uri", uri}};
    return dispatch(server, "resources/read", std::move(params), std::move(ct));
}

// ============================================================================
// Prompts
// ============================================================================

Result<Json> McpClient::prompts_list(std::string_view server, CancelToken ct) {
    return dispatch(server, "prompts/list", Json::object(), std::move(ct));
}

Result<Json> McpClient::prompts_get(std::string_view server,
                                     std::string_view prompt_name,
                                     const Json&      prompt_args,
                                     CancelToken      ct) {
    Json params = {
        {"name",      prompt_name},
        {"arguments", prompt_args}
    };
    return dispatch(server, "prompts/get", std::move(params), std::move(ct));
}

// ============================================================================
// Capability queries
// ============================================================================

std::optional<ServerCapabilities>
McpClient::capabilities(std::string_view server) const {
    std::lock_guard<std::mutex> lk(caps_mu_);
    auto it = caps_.find(std::string(server));
    if (it == caps_.end()) return std::nullopt;
    return it->second;
}

std::vector<std::string> McpClient::initialized_servers() const {
    std::lock_guard<std::mutex> lk(caps_mu_);
    std::vector<std::string> names;
    names.reserve(caps_.size());
    for (const auto& [name, _] : caps_) {
        names.push_back(name);
    }
    return names;
}

} // namespace batbox::mcp
