// include/batbox/config/ConfigReload.hpp
// ---------------------------------------------------------------------------
// batbox::config::reload_config — /config reload semantics.
//
// reload_config(cfg) re-runs the full load chain:
//   .env → settings.json → agents.json → mcp.json → keybindings.json
//
// On success:
//   1. A new Config is loaded via Config::load() (transactional — the current
//      cfg is not modified until all loading succeeds).
//   2. cfg is replaced with the new value in one assignment.
//   3. cfg.version_seq is incremented atomically.
//   4. A ReloadReport describing which fields changed and which require a
//      restart is returned.
//
// On failure:
//   The original cfg is preserved untouched (reload is transactional).
//   Err(message) is returned describing the parse/validation failure.
//
// Restart-required fields (sidecar.python, general.log_file):
//   A change to these values takes effect only after a full process restart.
//   reload_config() lists them in ReloadReport::restart_required_fields but
//   does not prevent the reload; the caller (/config reload command) is
//   responsible for noticing and showing the user the restart notice.
//
// Observer / subscriber API:
//   Components that need to react to a reload without polling version_seq can
//   register a callback:
//
//     auto handle = ConfigReloadBus::instance().subscribe(
//         [](const Config& new_cfg, const ReloadReport& report) {
//             // react to the reload
//         });
//     // handle is a RAII guard — dropping it unregisters the callback.
//
//   Registered callbacks are fired synchronously inside reload_config(),
//   after the config has been swapped and version_seq bumped.  Callbacks must
//   not call reload_config() themselves.
//
// Thread safety:
//   ConfigReloadBus is protected by a shared mutex: subscribe() takes an
//   exclusive lock; fire() (called from reload_config()) takes a shared lock
//   so multiple callbacks can run concurrently in read-mode.
//   reload_config() itself is not thread-safe with respect to the Config
//   object it modifies — the caller is responsible for synchronising access
//   to cfg (e.g. via a std::mutex guarding the shared Config instance).
// ---------------------------------------------------------------------------
#pragma once

#include <batbox/config/Config.hpp>
#include <batbox/core/Result.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

namespace batbox::config {

// ============================================================================
// ReloadReport — result of a successful reload
// ============================================================================

/// Report returned by reload_config() on success.
///
/// changed_fields lists every top-level field path (e.g., "api.default_model",
/// "ui.theme") whose value differs from the previous config.
///
/// restart_required_fields lists the subset of changed_fields that only take
/// effect after a full process restart (currently: "sidecar.python",
/// "general.log_file").
///
/// errors is always empty for a successful reload; it exists so callers can
/// pass a single ReloadReport through a reporting pipeline without extra
/// wrapping.
struct ReloadReport {
    std::vector<std::string> changed_fields;
    std::vector<std::string> restart_required_fields;
    std::vector<std::string> errors;

    /// True when no configuration values changed.
    [[nodiscard]] bool is_unchanged() const noexcept {
        return changed_fields.empty();
    }

    /// True when the report contains no content at all.
    [[nodiscard]] bool empty() const noexcept {
        return changed_fields.empty() &&
               restart_required_fields.empty() &&
               errors.empty();
    }
};

// ============================================================================
// ConfigReloadBus — singleton subscriber/observer registry
// ============================================================================

/// Opaque handle returned by ConfigReloadBus::subscribe().
/// Destroying the handle unregisters the associated callback.
class ReloadSubscription;

/// Singleton event bus for config reload notifications.
///
/// Usage:
///   auto handle = ConfigReloadBus::instance().subscribe(
///       [](const Config& cfg, const ReloadReport& report) { ... });
///   // unregister by letting handle go out of scope
class ConfigReloadBus {
public:
    /// Callback type: receives the new Config and the diff report.
    using Callback = std::function<void(const Config&, const ReloadReport&)>;

    /// Access the process-wide singleton instance.
    [[nodiscard]] static ConfigReloadBus& instance();

    /// Register a callback to be called after every successful reload.
    /// Returns a subscription handle; dropping the handle unregisters the
    /// callback.
    [[nodiscard]] std::shared_ptr<ReloadSubscription>
    subscribe(Callback cb);

    /// Fire all registered callbacks with the new config and report.
    /// Called internally by reload_config(). Holds a shared lock so multiple
    /// callbacks can be observed concurrently.
    void fire(const Config& cfg, const ReloadReport& report);

private:
    ConfigReloadBus() = default;
    ~ConfigReloadBus() = default;

    ConfigReloadBus(const ConfigReloadBus&)            = delete;
    ConfigReloadBus& operator=(const ConfigReloadBus&) = delete;

    struct Entry {
        uint64_t id;
        Callback cb;
        std::weak_ptr<ReloadSubscription> handle;
    };

    mutable std::shared_mutex mutex_;
    std::vector<Entry>        entries_;
    uint64_t                  next_id_{0};

    friend class ReloadSubscription;
    void unsubscribe(uint64_t id);
};

/// RAII guard returned by subscribe(); destructor calls unsubscribe().
class ReloadSubscription {
public:
    ~ReloadSubscription();

    ReloadSubscription(const ReloadSubscription&)            = delete;
    ReloadSubscription& operator=(const ReloadSubscription&) = delete;

private:
    explicit ReloadSubscription(uint64_t id, ConfigReloadBus* bus)
        : id_(id), bus_(bus) {}

    uint64_t          id_;
    ConfigReloadBus*  bus_;

    friend class ConfigReloadBus;
};

// ============================================================================
// reload_config() — the main entry point
// ============================================================================

/// Reload the full configuration stack and update cfg in place.
///
/// Parameters:
///   cfg          — the Config to update on success (not modified on failure).
///   config_dir   — directory containing .batbox config files; defaults to
///                  cfg.general.config_dir.  May be overridden for testing.
///                  The following files are loaded from this directory:
///                    - .env
///                    - settings.json
///                    - agents.json    (informational only — not merged into Config)
///                    - mcp.json       (informational only — not merged into Config)
///                    - keybindings.json (informational only — not merged into Config)
///                  Only .env and settings.json affect the Config struct.
///                  The others are loaded to validate them and surface errors.
///
/// Returns:
///   Ok(ReloadReport) — reload succeeded; cfg has been updated.
///   Err(std::string) — parse or validation error; cfg is UNCHANGED.
///
/// On success, fires all ConfigReloadBus subscribers synchronously.
[[nodiscard]]
Result<ReloadReport, std::string>
reload_config(Config& cfg,
              std::filesystem::path config_dir = {});

} // namespace batbox::config
