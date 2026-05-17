// src/mcp/McpServerRegistry.cpp
// ---------------------------------------------------------------------------
// McpServerRegistry implementation.
// See include/batbox/mcp/McpServerRegistry.hpp for the full design notes.
// ---------------------------------------------------------------------------

#include <batbox/mcp/McpServerRegistry.hpp>

#include <batbox/config/McpConfig.hpp>
#include <batbox/core/Logging.hpp>
#include <batbox/mcp/HttpTransport.hpp>
#include <batbox/mcp/IMcpTransport.hpp>
#include <batbox/mcp/SseTransport.hpp>
#include <batbox/mcp/StdioTransport.hpp>
#include <batbox/mcp/WsTransport.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

namespace batbox::mcp {

// ============================================================================
// Destructor
// ============================================================================

McpServerRegistry::~McpServerRegistry() {
    stop_all();
}

// ============================================================================
// Configuration injection
// ============================================================================

void McpServerRegistry::load_from_config(
    std::vector<batbox::config::McpServerConfig> servers)
{
    std::lock_guard<std::mutex> lk(transports_mu_);
    transports_.clear();
    for (auto& cfg : servers) {
        auto transport = make_transport(cfg);
        if (transport) {
            transports_.emplace(cfg.name, std::move(transport));
        } else {
            BATBOX_LOG_WARN("McpServerRegistry: could not build transport for '{}'",
                            cfg.name);
        }
    }
}

void McpServerRegistry::add_transport(std::string name,
                                       std::unique_ptr<IMcpTransport> transport)
{
    std::lock_guard<std::mutex> lk(transports_mu_);
    transports_[std::move(name)] = std::move(transport);
}

// ============================================================================
// Lifecycle
// ============================================================================

std::vector<std::pair<std::string, std::string>>
McpServerRegistry::start_all(CancelToken ct)
{
    // Load from filesystem if no transports have been injected yet.
    {
        std::lock_guard<std::mutex> lk(transports_mu_);
        if (transports_.empty()) {
            auto configs = batbox::config::load_mcp_configs();
            for (auto& cfg : configs) {
                auto transport = make_transport(cfg);
                if (transport) {
                    transports_.emplace(cfg.name, std::move(transport));
                } else {
                    BATBOX_LOG_WARN(
                        "McpServerRegistry: could not build transport for '{}'",
                        cfg.name);
                }
            }
        }
    }

    // Snapshot names under the lock so we can iterate outside it.
    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> lk(transports_mu_);
        names.reserve(transports_.size());
        for (auto& [name, _] : transports_) {
            names.push_back(name);
        }
    }

    if (names.empty()) {
        BATBOX_LOG_INFO("McpServerRegistry: no MCP servers configured");
        return {};
    }

    // Launch one thread per server to call start() in parallel.
    struct StartResult {
        std::string name;
        std::string error; // empty = success
    };

    std::vector<StartResult>  results(names.size());
    std::vector<std::thread>  threads;
    threads.reserve(names.size());

    for (std::size_t i = 0; i < names.size(); ++i) {
        const std::string& name = names[i];
        threads.emplace_back([this, &name, &results, i, &ct]() {
            // Each thread gets its own child cancel token derived from the
            // caller's ct so individual server cancellation is independent.
            auto [child_src, child_ct] = ct.child();
            (void)child_src; // not used — we cancel via the parent

            IMcpTransport* transport = nullptr;
            {
                std::lock_guard<std::mutex> lk(transports_mu_);
                auto it = transports_.find(name);
                if (it != transports_.end()) {
                    transport = it->second.get();
                }
            }

            if (!transport) {
                results[i] = {name, "transport not found"};
                return;
            }

            BATBOX_LOG_INFO("McpServerRegistry: starting '{}'", name);
            auto res = transport->start(std::move(child_ct));
            if (!res.has_value()) {
                BATBOX_LOG_WARN("McpServerRegistry: '{}' failed to start: {}",
                                name, res.error());
                results[i] = {name, res.error()};
            } else {
                BATBOX_LOG_INFO("McpServerRegistry: '{}' started successfully",
                                name);
                results[i] = {name, ""};
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // Collect errors.
    std::vector<std::pair<std::string, std::string>> errors;
    for (auto& r : results) {
        if (!r.error.empty()) {
            errors.emplace_back(r.name, r.error);
        }
    }
    return errors;
}

Result<void> McpServerRegistry::restart(std::string_view name, CancelToken ct)
{
    IMcpTransport* transport = nullptr;
    {
        std::lock_guard<std::mutex> lk(transports_mu_);
        auto it = transports_.find(std::string(name));
        if (it == transports_.end()) {
            return Err(std::string("unknown server: ") + std::string(name));
        }
        transport = it->second.get();
    }

    BATBOX_LOG_INFO("McpServerRegistry: restarting '{}'", name);
    transport->stop();
    auto res = transport->start(std::move(ct));
    if (!res.has_value()) {
        BATBOX_LOG_WARN("McpServerRegistry: '{}' failed to restart: {}",
                        name, res.error());
        return Err(res.error());
    }
    BATBOX_LOG_INFO("McpServerRegistry: '{}' restarted successfully", name);
    return {};
}

void McpServerRegistry::stop_all()
{
    // Stop health monitor first so it doesn't race with transport teardown.
    stop_health_monitor();

    // Stop all transports.
    std::lock_guard<std::mutex> lk(transports_mu_);
    for (auto& [name, transport] : transports_) {
        BATBOX_LOG_INFO("McpServerRegistry: stopping '{}'", name);
        transport->stop();
    }
}

// ============================================================================
// Health monitoring
// ============================================================================

void McpServerRegistry::on_status_change(std::function<void(HealthEvent)> callback)
{
    std::lock_guard<std::mutex> lk(status_cb_mu_);
    status_cb_ = std::move(callback);
}

void McpServerRegistry::start_health_monitor()
{
    // Idempotent: don't start a second thread.
    bool expected = false;
    if (!health_running_.compare_exchange_strong(expected, true)) {
        return;
    }

    health_thread_ = std::thread([this]() { health_monitor_loop(); });
}

void McpServerRegistry::stop_health_monitor()
{
    health_running_.store(false);
    if (health_thread_.joinable()) {
        health_thread_.join();
    }
}

void McpServerRegistry::health_monitor_loop()
{
    using namespace std::chrono_literals;
    constexpr auto poll_interval = 30s;

    while (health_running_.load()) {
        // Sleep in short increments so we can exit quickly when health_running_
        // is cleared.
        auto deadline = std::chrono::steady_clock::now() + poll_interval;
        while (health_running_.load() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(100ms);
        }

        if (!health_running_.load()) {
            break;
        }

        // Snapshot current health state under the transport lock.
        std::vector<std::pair<std::string, bool>> current;
        {
            std::lock_guard<std::mutex> lk(transports_mu_);
            current.reserve(transports_.size());
            for (auto& [name, transport] : transports_) {
                current.emplace_back(name, transport->healthy());
            }
        }

        // Compare against previous state and fire callback on transitions.
        std::function<void(HealthEvent)> cb;
        {
            std::lock_guard<std::mutex> lk(status_cb_mu_);
            cb = status_cb_;
        }

        for (auto& [name, is_healthy] : current) {
            std::lock_guard<std::mutex> lk(prev_health_mu_);
            auto it = prev_health_.find(name);
            bool prev = (it != prev_health_.end()) ? it->second : true;

            if (is_healthy != prev) {
                prev_health_[name] = is_healthy;

                HealthEvent ev;
                ev.name  = name;
                ev.state = is_healthy ? HealthState::Healthy : HealthState::Unhealthy;

                BATBOX_LOG_WARN("McpServerRegistry: '{}' is now {}",
                                name,
                                is_healthy ? "healthy" : "unhealthy");

                if (cb) {
                    cb(ev);
                }
            } else if (it == prev_health_.end()) {
                // First observation — record without firing callback.
                prev_health_[name] = is_healthy;
            }
        }
    }
}

// ============================================================================
// Accessors
// ============================================================================

IMcpTransport* McpServerRegistry::get(std::string_view name) const
{
    std::lock_guard<std::mutex> lk(transports_mu_);
    auto it = transports_.find(std::string(name));
    if (it == transports_.end()) {
        return nullptr;
    }
    return it->second.get();
}

std::vector<std::string> McpServerRegistry::server_names() const
{
    std::lock_guard<std::mutex> lk(transports_mu_);
    std::vector<std::string> names;
    names.reserve(transports_.size());
    for (auto& [name, _] : transports_) {
        names.push_back(name);
    }
    return names;
}

std::size_t McpServerRegistry::size() const
{
    std::lock_guard<std::mutex> lk(transports_mu_);
    return transports_.size();
}

int McpServerRegistry::count_failed_servers() const
{
    // Snapshot the transport pointers under the lock, then release before
    // calling healthy() so we never hold transports_mu_ during a virtual call.
    std::vector<IMcpTransport*> ptrs;
    {
        std::lock_guard<std::mutex> lk(transports_mu_);
        ptrs.reserve(transports_.size());
        for (auto& [name, t] : transports_) {
            ptrs.push_back(t.get());
        }
    }

    int failed = 0;
    for (auto* t : ptrs) {
        if (t && !t->healthy()) {
            ++failed;
        }
    }
    return failed;
}

// ============================================================================
// Transport factory
// ============================================================================

std::unique_ptr<IMcpTransport>
McpServerRegistry::make_transport(const batbox::config::McpServerConfig& cfg)
{
    return std::visit(
        [&](auto&& impl) -> std::unique_ptr<IMcpTransport> {
            using T = std::decay_t<decltype(impl)>;

            if constexpr (std::is_same_v<T, batbox::config::StdioConfig>) {
                // Convert env map to vector<string> "KEY=VALUE" pairs.
                std::vector<std::string> env_vec;
                env_vec.reserve(impl.env.size());
                for (auto& [k, v] : impl.env) {
                    env_vec.push_back(k + "=" + v);
                }
                return std::make_unique<StdioTransport>(
                    impl.command, impl.args, std::move(env_vec));
            }
            else if constexpr (std::is_same_v<T, batbox::config::HttpConfig>) {
                return std::make_unique<HttpTransport>(impl.url, impl.headers);
            }
            else if constexpr (std::is_same_v<T, batbox::config::SseConfig>) {
                return std::make_unique<SseTransport>(impl.url, impl.headers);
            }
            else if constexpr (std::is_same_v<T, batbox::config::WsConfig>) {
                return std::make_unique<WsTransport>(impl.url, impl.headers);
            }
            else {
                static_assert(sizeof(T) == 0, "Unhandled McpServerConfig variant");
                return nullptr;
            }
        },
        cfg.impl);
}

} // namespace batbox::mcp
