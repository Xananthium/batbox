// src/config/ConfigReload.cpp
// ---------------------------------------------------------------------------
// Implementation of batbox::config::reload_config and ConfigReloadBus.
//
// Design: transactional swap.
//
//   1. Resolve config_dir (fall back to cfg.general.config_dir).
//   2. Load the .env file from config_dir/.env (missing = empty env is ok).
//   3. Merge with the live process environment (process env wins for shell
//      vars; .env wins for BATBOX_* vars per the established precedence).
//   4. Load settings.json from config_dir/settings.json via load_settings().
//   5. Build a new Config via Config::load(env, settings_json).
//      On error → Err propagated; cfg unchanged.
//   6. Diff old vs new: walk all config fields, record which changed.
//   7. Identify restart-required fields (sidecar.python, general.log_file).
//   8. Preserve the previous version_seq+1 into the new config.
//   9. Assign new config to cfg (atomic version_seq bump via fetch_add).
//  10. Fire ConfigReloadBus subscribers.
// ---------------------------------------------------------------------------

#include <batbox/config/ConfigReload.hpp>
#include <batbox/config/EnvLoader.hpp>
#include <batbox/config/SettingsLoader.hpp>
#include <batbox/core/Json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace batbox::config {

// ============================================================================
// ReloadSubscription
// ============================================================================

ReloadSubscription::~ReloadSubscription() {
    if (bus_) {
        bus_->unsubscribe(id_);
    }
}

// ============================================================================
// ConfigReloadBus
// ============================================================================

ConfigReloadBus& ConfigReloadBus::instance() {
    static ConfigReloadBus s_instance;
    return s_instance;
}

std::shared_ptr<ReloadSubscription>
ConfigReloadBus::subscribe(Callback cb) {
    std::unique_lock lock(mutex_);
    const uint64_t id = next_id_++;
    auto handle = std::shared_ptr<ReloadSubscription>(
        new ReloadSubscription(id, this));
    entries_.push_back(Entry{id, std::move(cb), handle});
    return handle;
}

void ConfigReloadBus::unsubscribe(uint64_t id) {
    std::unique_lock lock(mutex_);
    entries_.erase(
        std::remove_if(entries_.begin(), entries_.end(),
                       [id](const Entry& e) { return e.id == id; }),
        entries_.end());
}

void ConfigReloadBus::fire(const Config& cfg, const ReloadReport& report) {
    // Collect live callbacks under the shared lock, then call them outside
    // the lock to avoid re-entrancy issues.
    std::vector<Callback> live;
    {
        std::shared_lock lock(mutex_);
        live.reserve(entries_.size());
        for (const auto& e : entries_) {
            if (!e.handle.expired()) {
                live.push_back(e.cb);
            }
        }
    }
    for (const auto& cb : live) {
        cb(cfg, report);
    }
}

// ============================================================================
// Internal diff helpers
// ============================================================================
namespace {

/// Append a field name to 'report.changed_fields' and, if it is in the
/// restart-required set, also to 'report.restart_required_fields'.
static void record_change(ReloadReport& report,
                          const std::string& field_name,
                          bool restart_required = false) {
    report.changed_fields.push_back(field_name);
    if (restart_required) {
        report.restart_required_fields.push_back(field_name);
    }
}

/// Compare two Config objects and populate a ReloadReport with every field
/// that differs.  Restart-required fields (sidecar.python, general.log_file)
/// are flagged accordingly.
static ReloadReport diff_configs(const Config& old_cfg, const Config& new_cfg) {
    ReloadReport report;

    // --- API group -----------------------------------------------------------
    if (old_cfg.api.base_url          != new_cfg.api.base_url)
        record_change(report, "api.base_url");
    if (old_cfg.api.api_key           != new_cfg.api.api_key)
        record_change(report, "api.api_key");
    if (old_cfg.api.default_model     != new_cfg.api.default_model)
        record_change(report, "api.default_model");
    if (old_cfg.api.models            != new_cfg.api.models)
        record_change(report, "api.models");
    if (old_cfg.api.max_tokens        != new_cfg.api.max_tokens)
        record_change(report, "api.max_tokens");
    if (old_cfg.api.temperature       != new_cfg.api.temperature)
        record_change(report, "api.temperature");
    if (old_cfg.api.top_p             != new_cfg.api.top_p)
        record_change(report, "api.top_p");
    if (old_cfg.api.request_timeout_sec != new_cfg.api.request_timeout_sec)
        record_change(report, "api.request_timeout_sec");
    if (old_cfg.api.stream            != new_cfg.api.stream)
        record_change(report, "api.stream");

    // --- General group -------------------------------------------------------
    if (old_cfg.general.config_dir    != new_cfg.general.config_dir)
        record_change(report, "general.config_dir");
    if (old_cfg.general.project_memory_file != new_cfg.general.project_memory_file)
        record_change(report, "general.project_memory_file");
    if (old_cfg.general.log_level     != new_cfg.general.log_level)
        record_change(report, "general.log_level");
    // log_file change requires restart (file descriptor already open).
    if (old_cfg.general.log_file      != new_cfg.general.log_file)
        record_change(report, "general.log_file", /*restart_required=*/true);

    // --- UI group ------------------------------------------------------------
    if (old_cfg.ui.theme      != new_cfg.ui.theme)
        record_change(report, "ui.theme");
    if (old_cfg.ui.no_splash  != new_cfg.ui.no_splash)
        record_change(report, "ui.no_splash");
    if (old_cfg.ui.vim_mode   != new_cfg.ui.vim_mode)
        record_change(report, "ui.vim_mode");
    if (old_cfg.ui.statusline != new_cfg.ui.statusline)
        record_change(report, "ui.statusline");

    // --- Search group --------------------------------------------------------
    if (old_cfg.search.engine             != new_cfg.search.engine)
        record_change(report, "search.engine");
    if (old_cfg.search.searxng_url        != new_cfg.search.searxng_url)
        record_change(report, "search.searxng_url");
    if (old_cfg.search.respect_robots     != new_cfg.search.respect_robots)
        record_change(report, "search.respect_robots");
    if (old_cfg.search.webfetch_max_bytes != new_cfg.search.webfetch_max_bytes)
        record_change(report, "search.webfetch_max_bytes");
    if (old_cfg.search.webfetch_timeout_sec != new_cfg.search.webfetch_timeout_sec)
        record_change(report, "search.webfetch_timeout_sec");

    // --- Sidecar group -------------------------------------------------------
    // sidecar.python change requires restart (binary path is resolved at startup).
    if (old_cfg.sidecar.python              != new_cfg.sidecar.python)
        record_change(report, "sidecar.python", /*restart_required=*/true);
    if (old_cfg.sidecar.venv                != new_cfg.sidecar.venv)
        record_change(report, "sidecar.venv");
    if (old_cfg.sidecar.startup_timeout_sec != new_cfg.sidecar.startup_timeout_sec)
        record_change(report, "sidecar.startup_timeout_sec");
    if (old_cfg.sidecar.autostart           != new_cfg.sidecar.autostart)
        record_change(report, "sidecar.autostart");
    if (old_cfg.sidecar.prewarm             != new_cfg.sidecar.prewarm)
        record_change(report, "sidecar.prewarm");

    // --- Tools group ---------------------------------------------------------
    if (old_cfg.tools.bash_timeout_sec     != new_cfg.tools.bash_timeout_sec)
        record_change(report, "tools.bash_timeout_sec");
    if (old_cfg.tools.bash_max_output_bytes != new_cfg.tools.bash_max_output_bytes)
        record_change(report, "tools.bash_max_output_bytes");
    if (old_cfg.tools.task_parallel_limit  != new_cfg.tools.task_parallel_limit)
        record_change(report, "tools.task_parallel_limit");

    // --- MCP group -----------------------------------------------------------
    if (old_cfg.mcp.config_path         != new_cfg.mcp.config_path)
        record_change(report, "mcp.config_path");
    if (old_cfg.mcp.startup_timeout_sec != new_cfg.mcp.startup_timeout_sec)
        record_change(report, "mcp.startup_timeout_sec");

    // --- Agents group --------------------------------------------------------
    if (old_cfg.agents.agents_config != new_cfg.agents.agents_config)
        record_change(report, "agents.agents_config");
    if (old_cfg.agents.agents_dir    != new_cfg.agents.agents_dir)
        record_change(report, "agents.agents_dir");
    if (old_cfg.agents.demon_enabled != new_cfg.agents.demon_enabled)
        record_change(report, "agents.demon_enabled");

    // --- Security group ------------------------------------------------------
    if (old_cfg.security.permission_mode   != new_cfg.security.permission_mode)
        record_change(report, "security.permission_mode");
    if (old_cfg.security.auto_approve_reads != new_cfg.security.auto_approve_reads)
        record_change(report, "security.auto_approve_reads");

    // --- Compact group -------------------------------------------------------
    if (old_cfg.compact.auto_compact_at_pct        != new_cfg.compact.auto_compact_at_pct)
        record_change(report, "compact.auto_compact_at_pct");
    if (old_cfg.compact.keep_last_n_turns_verbatim != new_cfg.compact.keep_last_n_turns_verbatim)
        record_change(report, "compact.keep_last_n_turns_verbatim");

    return report;
}

/// Read and return the contents of 'path' as a string.
/// Returns empty string if the file does not exist.
/// Returns Err if the file exists but cannot be read.
static Result<std::string, std::string>
read_file_optional(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        return std::string{};
    }
    std::ifstream f(path);
    if (!f) {
        return Err("cannot open " + path.string());
    }
    std::ostringstream buf;
    buf << f.rdbuf();
    if (f.fail() && !f.eof()) {
        return Err("read error on " + path.string());
    }
    return buf.str();
}

} // anonymous namespace

// ============================================================================
// reload_config()
// ============================================================================

Result<ReloadReport, std::string>
reload_config(Config& cfg, std::filesystem::path config_dir) {
    namespace fs = std::filesystem;

    // ------------------------------------------------------------------
    // 1. Resolve config_dir
    // ------------------------------------------------------------------
    if (config_dir.empty()) {
        config_dir = cfg.general.config_dir;
    }

    // ------------------------------------------------------------------
    // 2. Load .env from config_dir/.env (missing = empty; hard error only
    //    if the file exists but cannot be opened).
    // ------------------------------------------------------------------
    EnvMap env;
    const fs::path env_path = config_dir / ".env";
    if (fs::exists(env_path)) {
        auto r = load_env_file(env_path);
        if (!r) {
            return Err("reload: .env: " + r.error());
        }
        env = std::move(*r);
    }

    // Merge with the live process environment.
    // .env values win for BATBOX_* vars (process_env_wins = false).
    merge_with_process_env(env, /*process_env_wins=*/false);

    // ------------------------------------------------------------------
    // 3. Load settings.json
    // ------------------------------------------------------------------
    const fs::path settings_path = config_dir / "settings.json";
    auto settings_r = load_settings(settings_path);
    if (!settings_r) {
        return Err("reload: settings.json: " + settings_r.error());
    }
    // Convert Settings to a Json object matching the shape that
    // Config::load() expects (the "config" sub-object from settings.json).
    // We re-serialise only the fields that apply to the Config aggregate.
    Json settings_json = Json::object();
    {
        const Settings& s = *settings_r;
        // Theme — stored under ui.theme in Config terms.
        if (!s.theme.empty()) {
            settings_json["ui"]["theme"] = s.theme;
        }
        // Permissions — not part of Config struct, skip.
        // output_style — not part of Config struct, skip.
        // plugins_disabled — not part of Config struct, skip.
    }

    // ------------------------------------------------------------------
    // 4. Build new Config (transactional — old cfg not touched on error)
    // ------------------------------------------------------------------
    auto new_cfg_r = Config::load(env, settings_json);
    if (!new_cfg_r) {
        return Err("reload: " + new_cfg_r.error());
    }
    Config new_cfg = std::move(*new_cfg_r);

    // ------------------------------------------------------------------
    // 5. Diff old vs new
    // ------------------------------------------------------------------
    ReloadReport report = diff_configs(cfg, new_cfg);

    // ------------------------------------------------------------------
    // 6. Bump version_seq: new_seq = old_seq + 1.
    //    Carry the incremented value into new_cfg before swapping.
    // ------------------------------------------------------------------
    const uint64_t new_seq =
        cfg.version_seq.load(std::memory_order_relaxed) + 1;
    new_cfg.version_seq.store(new_seq, std::memory_order_relaxed);

    // ------------------------------------------------------------------
    // 7. Swap cfg atomically (single assignment)
    // ------------------------------------------------------------------
    cfg = std::move(new_cfg);

    // ------------------------------------------------------------------
    // 8. Notify subscribers
    // ------------------------------------------------------------------
    ConfigReloadBus::instance().fire(cfg, report);

    return report;
}

} // namespace batbox::config
