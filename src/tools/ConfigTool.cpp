// src/tools/ConfigTool.cpp
// ---------------------------------------------------------------------------
// Implementation of batbox::tools::ConfigTool.
//
// Dispatches on args["action"]:
//   get    — return redacted_for_display() value for a single key
//   set    — validate key + value, write via write_settings(), fire reload
//   list   — return redacted_for_display() for all keys
//   reload — call reload_config() and report changed fields
//
// Blueprint contract: batbox::tools::ConfigTool (task CPP 5.23)
// ---------------------------------------------------------------------------

#include <batbox/tools/ConfigTool.hpp>

#include <batbox/config/ConfigReload.hpp>
#include <batbox/config/SettingsLoader.hpp>
#include <batbox/core/Json.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace batbox::tools {

// ============================================================================
// Schema table — maps canonical BATBOX_* key → dot-path short form → writable?
// ============================================================================
namespace {

struct KeyEntry {
    std::string_view env_key;       // canonical BATBOX_* name
    std::string_view dot_path;      // short form (e.g. "api.default_model")
    bool             read_only;     // true = cannot be set via this tool
};

// All ~40 BATBOX_* keys derived from Config.hpp.
// Ordered: api, general, ui, search, sidecar, tools, mcp, agents, security, compact.
static constexpr std::array<KeyEntry, 40> kKeys = {{
    // ApiConfig
    { "BATBOX_API_BASE_URL",              "api.base_url",                    false },
    { "BATBOX_API_KEY",                   "api.api_key",                     true  },  // READ-ONLY
    { "BATBOX_DEFAULT_MODEL",             "api.default_model",               false },
    { "BATBOX_MODELS",                    "api.models",                      false },
    { "BATBOX_MAX_TOKENS",                "api.max_tokens",                  false },
    { "BATBOX_TEMPERATURE",               "api.temperature",                 false },
    { "BATBOX_TOP_P",                     "api.top_p",                       false },
    { "BATBOX_REQUEST_TIMEOUT_SEC",       "api.request_timeout_sec",         false },
    { "BATBOX_STREAM",                    "api.stream",                      false },
    // GeneralConfig
    { "BATBOX_CONFIG_DIR",                "general.config_dir",              false },
    { "BATBOX_PROJECT_MEMORY_FILE",       "general.project_memory_file",     false },
    { "BATBOX_LOG_LEVEL",                 "general.log_level",               false },
    { "BATBOX_LOG_FILE",                  "general.log_file",                false },
    // UiConfig
    { "BATBOX_THEME",                     "ui.theme",                        false },
    { "BATBOX_NO_SPLASH",                 "ui.no_splash",                    false },
    { "BATBOX_VIM_MODE",                  "ui.vim_mode",                     false },
    { "BATBOX_STATUSLINE",                "ui.statusline",                   false },
    // SearchConfig
    { "BATBOX_SEARCH_ENGINE",             "search.engine",                   false },
    { "BATBOX_SEARXNG_URL",               "search.searxng_url",              false },
    { "BATBOX_RESPECT_ROBOTS",            "search.respect_robots",           false },
    { "BATBOX_WEBFETCH_MAX_BYTES",        "search.webfetch_max_bytes",       false },
    { "BATBOX_WEBFETCH_TIMEOUT_SEC",      "search.webfetch_timeout_sec",     false },
    // SidecarConfig
    { "BATBOX_SIDECAR_PYTHON",            "sidecar.python",                  false },
    { "BATBOX_SIDECAR_VENV",              "sidecar.venv",                    false },
    { "BATBOX_SIDECAR_STARTUP_TIMEOUT_SEC","sidecar.startup_timeout_sec",   false },
    { "BATBOX_SIDECAR_AUTOSTART",         "sidecar.autostart",               false },
    { "BATBOX_SIDECAR_PREWARM",           "sidecar.prewarm",                 false },
    // ToolsConfig
    { "BATBOX_BASH_TIMEOUT_SEC",          "tools.bash_timeout_sec",          false },
    { "BATBOX_BASH_MAX_OUTPUT_BYTES",     "tools.bash_max_output_bytes",     false },
    { "BATBOX_TASK_PARALLEL_LIMIT",       "tools.task_parallel_limit",       false },
    // McpConfig
    { "BATBOX_MCP_CONFIG",                "mcp.config_path",                 false },
    { "BATBOX_MCP_STARTUP_TIMEOUT_SEC",   "mcp.startup_timeout_sec",         false },
    // AgentsConfig
    { "BATBOX_AGENTS_CONFIG",             "agents.agents_config",            false },
    { "BATBOX_AGENTS_DIR",                "agents.agents_dir",               false },
    { "BATBOX_DEMON_ENABLED",             "agents.demon_enabled",            false },
    // SecurityConfig
    { "BATBOX_PERMISSION_MODE",           "security.permission_mode",        false },
    { "BATBOX_AUTO_APPROVE_READS",        "security.auto_approve_reads",     false },
    // CompactConfig
    { "BATBOX_AUTO_COMPACT_AT_PCT",       "compact.auto_compact_at_pct",     false },
    { "BATBOX_KEEP_LAST_N_TURNS_VERBATIM","compact.keep_last_n_turns_verbatim",false },
}};

/// Case-insensitive string comparison helper.
static bool iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

/// Find the KeyEntry for a given canonical BATBOX_* key or dot-path short form.
/// Returns nullptr when not found.
static const KeyEntry* find_key_entry(std::string_view input) {
    for (const auto& entry : kKeys) {
        if (iequal(input, entry.env_key) || iequal(input, entry.dot_path)) {
            return &entry;
        }
    }
    return nullptr;
}

/// Extract the current value for a given canonical key from a redacted Json.
/// The redacted Json returned by Config::redacted_for_display() has structure:
///   { "api": { "base_url": ... }, "general": { ... }, ... }
/// The dot_path short form maps directly to this structure (e.g. "api.base_url").
static Json extract_value_from_json(const Json& cfg_json, std::string_view dot_path) {
    // Split dot_path at the first '.'
    const auto dot = dot_path.find('.');
    if (dot == std::string_view::npos) {
        const std::string key(dot_path);
        if (cfg_json.contains(key)) return cfg_json[key];
        return Json{};
    }

    const std::string section(dot_path.substr(0, dot));
    const std::string field(dot_path.substr(dot + 1));

    if (!cfg_json.contains(section)) return Json{};
    const auto& sec = cfg_json[section];
    if (!sec.is_object() || !sec.contains(field)) return Json{};
    return sec[field];
}

/// Build a flat JSON object of all config keys → redacted values.
static Json build_flat_config(const Json& cfg_json) {
    Json result = Json::object();
    for (const auto& entry : kKeys) {
        const std::string k(entry.env_key);
        const Json val = extract_value_from_json(cfg_json, entry.dot_path);
        result[k] = val;
    }
    return result;
}

} // anonymous namespace

// ============================================================================
// ConfigTool — constructor
// ============================================================================

ConfigTool::ConfigTool(config::Config&        cfg,
                       std::mutex&            cfg_mutex,
                       std::filesystem::path  config_dir)
    : cfg_(cfg)
    , cfg_mutex_(cfg_mutex)
    , config_dir_(std::move(config_dir))
{}

// ============================================================================
// ITool identity
// ============================================================================

std::string_view ConfigTool::name() const {
    return "Config";
}

std::string_view ConfigTool::description() const {
    return "Read or write BATBOX_* runtime configuration. "
           "action=get returns the current value of one key; "
           "action=set writes a new value and triggers a config reload; "
           "action=list returns all keys with current values (secrets redacted); "
           "action=reload re-runs the full config load chain.";
}

Json ConfigTool::schema_json() const {
    return Json{
        {"name",        "Config"},
        {"description", description()},
        {"parameters", Json{
            {"type",       "object"},
            {"properties", Json{
                {"action", Json{
                    {"type",        "string"},
                    {"enum",        Json::array({"get", "set", "list", "reload"})},
                    {"description", "Operation: get=read one key, set=write one key, "
                                    "list=all keys, reload=re-read config from disk."}
                }},
                {"key", Json{
                    {"type",        "string"},
                    {"description", "Config key — use BATBOX_* env-var name "
                                    "(e.g. BATBOX_DEFAULT_MODEL) or dot-path short form "
                                    "(e.g. api.default_model). Required for get/set."}
                }},
                {"value", Json{
                    {"type",        "string"},
                    {"description", "New value as a string. Required for set."}
                }}
            }},
            {"required", Json::array({"action"})}
        }}
    };
}

// ============================================================================
// run() — dispatch
// ============================================================================

ToolResult ConfigTool::run(const Json& args, ToolContext& ctx) {
    // Cancellation check.
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    // Validate "action" argument.
    if (!args.contains("action") || !args["action"].is_string()) {
        return ToolResult::error(
            "Config: required argument 'action' is missing or not a string. "
            "Valid values: get, set, list, reload.");
    }

    const std::string action = args["action"].get<std::string>();

    if (action == "get") {
        return handle_get(args);
    } else if (action == "set") {
        // set is a mutating operation — refuse in Plan mode.
        if (ctx.is_plan_mode()) {
            return ToolResult::error(
                "Config: 'set' is not allowed in Plan mode (read-only).");
        }
        return handle_set(args);
    } else if (action == "list") {
        return handle_list();
    } else if (action == "reload") {
        // reload is mutating — refuse in Plan mode.
        if (ctx.is_plan_mode()) {
            return ToolResult::error(
                "Config: 'reload' is not allowed in Plan mode (read-only).");
        }
        return handle_reload();
    } else {
        return ToolResult::error(
            "Config: unknown action '" + action + "'. "
            "Valid values: get, set, list, reload.");
    }
}

// ============================================================================
// resolve_key / is_read_only_key
// ============================================================================

std::string ConfigTool::resolve_key(std::string_view input) {
    const KeyEntry* entry = find_key_entry(input);
    if (!entry) return {};
    return std::string(entry->env_key);
}

bool ConfigTool::is_read_only_key(std::string_view canonical_key) {
    const KeyEntry* entry = find_key_entry(canonical_key);
    return entry && entry->read_only;
}

// ============================================================================
// handle_get
// ============================================================================

ToolResult ConfigTool::handle_get(const Json& args) const {
    // Require "key" argument.
    if (!args.contains("key") || !args["key"].is_string()) {
        return ToolResult::error(
            "Config: get requires a 'key' argument (string).");
    }
    const std::string raw_key = args["key"].get<std::string>();

    // Resolve to canonical BATBOX_* key.
    const std::string canonical = resolve_key(raw_key);
    if (canonical.empty()) {
        return ToolResult::error(
            "Config: unknown key '" + raw_key + "'. "
            "Use action=list to see all available keys.");
    }

    // Find entry so we know the dot_path.
    const KeyEntry* entry = find_key_entry(canonical);

    // Build redacted config JSON and extract the value.
    Json cfg_json;
    {
        std::lock_guard<std::mutex> lock(cfg_mutex_);
        cfg_json = cfg_.redacted_for_display();
    }

    const Json value = extract_value_from_json(cfg_json, entry->dot_path);

    const Json payload = Json{
        {"action",    "get"},
        {"key",       canonical},
        {"dot_path",  std::string(entry->dot_path)},
        {"value",     value},
        {"read_only", entry->read_only}
    };

    const std::string body = canonical + " = " + value.dump();
    return ToolResult::ok(body, payload);
}

// ============================================================================
// handle_set
// ============================================================================

ToolResult ConfigTool::handle_set(const Json& args) {
    // Require "key" argument.
    if (!args.contains("key") || !args["key"].is_string()) {
        return ToolResult::error(
            "Config: set requires a 'key' argument (string).");
    }
    const std::string raw_key = args["key"].get<std::string>();

    // Require "value" argument.
    if (!args.contains("value") || !args["value"].is_string()) {
        return ToolResult::error(
            "Config: set requires a 'value' argument (string).");
    }
    const std::string raw_value = args["value"].get<std::string>();

    // Resolve canonical key.
    const std::string canonical = resolve_key(raw_key);
    if (canonical.empty()) {
        return ToolResult::error(
            "Config: unknown key '" + raw_key + "'. "
            "Use action=list to see all available keys.");
    }

    // Reject read-only keys.
    if (is_read_only_key(canonical)) {
        return ToolResult::error(
            "Config: '" + canonical + "' is read-only and cannot be set via this tool. "
            "Set it in your environment or .env file instead.");
    }

    // Determine the effective config_dir.
    std::filesystem::path cfg_dir;
    {
        std::lock_guard<std::mutex> lock(cfg_mutex_);
        cfg_dir = config_dir_.empty() ? cfg_.general.config_dir : config_dir_;
    }

    // Load current settings.json so we can patch only the changed field.
    const std::filesystem::path settings_path = cfg_dir / "settings.json";
    auto load_result = config::load_settings(settings_path);
    if (!load_result) {
        return ToolResult::error(
            "Config: failed to read settings.json: " + load_result.error());
    }
    config::Settings settings = std::move(load_result.value());

    // Apply the requested change to the settings struct.
    // We write only the fields that are stored in settings.json.
    // Fields that come purely from env vars can only be changed by editing
    // the .env file — for those we still update settings.json with an
    // "override" so the value takes effect on the next full load.
    // In practice, we map the canonical key to the matching settings field
    // where possible, then call reload_config() to pick it up.

    // For theme: store in settings.theme
    if (canonical == "BATBOX_THEME") {
        settings.theme = raw_value;
    } else if (canonical == "BATBOX_PERMISSION_MODE") {
        // Validate before writing.
        auto r = config::Config::parse_permission_mode(raw_value);
        if (!r) {
            return ToolResult::error(
                "Config: invalid value for " + canonical + ": " + r.error());
        }
        // Permission mode is stored in settings.json as output_style is a custom
        // field; we persist it via the env-layer .env update pattern below.
        // Fall through to the generic .env write path.
    }
    // All other fields are persisted by writing an env override line to .env
    // and then reloading.

    // Write the updated settings.json atomically.
    auto write_result = config::write_settings(settings_path, settings);
    if (!write_result) {
        return ToolResult::error(
            "Config: failed to write settings.json: " + write_result.error());
    }

    // Also patch the .env file with the new BATBOX_* key=value line so that
    // the reload picks it up for fields that are env-driven.
    const std::filesystem::path env_path = cfg_dir / ".env";
    {
        // Read existing .env content (best-effort; create if absent).
        std::string env_content;
        {
            std::ifstream f(env_path);
            if (f.is_open()) {
                std::ostringstream ss;
                ss << f.rdbuf();
                env_content = ss.str();
            }
        }

        // Find and replace the existing key line, or append it.
        const std::string key_eq = canonical + "=";
        std::string new_content;
        bool found = false;

        std::istringstream iss(env_content);
        std::string line;
        while (std::getline(iss, line)) {
            // Trim leading whitespace.
            std::string_view lv = line;
            while (!lv.empty() && (lv.front() == ' ' || lv.front() == '\t')) {
                lv.remove_prefix(1);
            }
            if (lv.rfind(key_eq, 0) == 0) {
                // Replace this line.
                new_content += canonical + "=" + raw_value + "\n";
                found = true;
            } else {
                new_content += line + "\n";
            }
        }

        if (!found) {
            new_content += canonical + "=" + raw_value + "\n";
        }

        // Write atomically via tmp+rename.
        const std::filesystem::path tmp_path = env_path.string() + ".tmp";
        {
            std::ofstream tf(tmp_path, std::ios::trunc);
            if (!tf.is_open()) {
                return ToolResult::error(
                    "Config: failed to open " + tmp_path.string() + " for writing.");
            }
            tf << new_content;
            if (!tf) {
                return ToolResult::error(
                    "Config: write error on " + tmp_path.string());
            }
        }
        std::error_code ec;
        std::filesystem::rename(tmp_path, env_path, ec);
        if (ec) {
            return ToolResult::error(
                "Config: failed to rename " + tmp_path.string() +
                " to " + env_path.string() + ": " + ec.message());
        }
    }

    // Trigger reload so the in-memory Config reflects the change.
    config::ReloadReport report;
    {
        std::lock_guard<std::mutex> lock(cfg_mutex_);
        const std::filesystem::path reload_dir =
            config_dir_.empty() ? cfg_.general.config_dir : config_dir_;
        auto reload_result = config::reload_config(cfg_, reload_dir);
        if (!reload_result) {
            return ToolResult::error(
                "Config: value written to .env but reload failed: " +
                reload_result.error());
        }
        report = std::move(reload_result.value());
    }

    // Build response.
    Json changed_arr = Json::array();
    for (const auto& f : report.changed_fields) changed_arr.push_back(f);

    Json restart_arr = Json::array();
    for (const auto& f : report.restart_required_fields) restart_arr.push_back(f);

    const Json payload = Json{
        {"action",                  "set"},
        {"key",                     canonical},
        {"value",                   raw_value},
        {"changed_fields",          changed_arr},
        {"restart_required_fields", restart_arr}
    };

    std::string body = "Set " + canonical + " = " + raw_value;
    if (!report.restart_required_fields.empty()) {
        body += " (restart required for: ";
        for (std::size_t i = 0; i < report.restart_required_fields.size(); ++i) {
            if (i > 0) body += ", ";
            body += report.restart_required_fields[i];
        }
        body += ")";
    }

    return ToolResult::ok(body, payload);
}

// ============================================================================
// handle_list
// ============================================================================

ToolResult ConfigTool::handle_list() const {
    Json cfg_json;
    {
        std::lock_guard<std::mutex> lock(cfg_mutex_);
        cfg_json = cfg_.redacted_for_display();
    }

    const Json flat = build_flat_config(cfg_json);

    // Build a human-readable body: "KEY = value" lines.
    std::string body;
    body.reserve(2048);
    for (const auto& entry : kKeys) {
        const std::string k(entry.env_key);
        const Json val = extract_value_from_json(cfg_json, entry.dot_path);
        body += k;
        body += " = ";
        body += val.dump();
        if (entry.read_only) body += "  [read-only]";
        body += "\n";
    }

    return ToolResult::ok(body, flat);
}

// ============================================================================
// handle_reload
// ============================================================================

ToolResult ConfigTool::handle_reload() {
    config::ReloadReport report;
    {
        std::lock_guard<std::mutex> lock(cfg_mutex_);
        const std::filesystem::path reload_dir =
            config_dir_.empty() ? cfg_.general.config_dir : config_dir_;
        auto result = config::reload_config(cfg_, reload_dir);
        if (!result) {
            return ToolResult::error(
                "Config reload failed: " + result.error());
        }
        report = std::move(result.value());
    }

    // Build structured payload.
    Json changed_arr = Json::array();
    for (const auto& f : report.changed_fields) changed_arr.push_back(f);

    Json restart_arr = Json::array();
    for (const auto& f : report.restart_required_fields) restart_arr.push_back(f);

    const Json payload = Json{
        {"action",                  "reload"},
        {"changed_fields",          changed_arr},
        {"restart_required_fields", restart_arr},
        {"is_unchanged",            report.is_unchanged()}
    };

    std::string body;
    if (report.is_unchanged()) {
        body = "Config reloaded. No fields changed.";
    } else {
        body = "Config reloaded. Changed fields: ";
        for (std::size_t i = 0; i < report.changed_fields.size(); ++i) {
            if (i > 0) body += ", ";
            body += report.changed_fields[i];
        }
        body += ".";
    }

    if (!report.restart_required_fields.empty()) {
        body += " Restart required for: ";
        for (std::size_t i = 0; i < report.restart_required_fields.size(); ++i) {
            if (i > 0) body += ", ";
            body += report.restart_required_fields[i];
        }
        body += ".";
    }

    return ToolResult::ok(body, payload);
}

} // namespace batbox::tools
