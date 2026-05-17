// src/config/Config.cpp
// ---------------------------------------------------------------------------
// Implementation of batbox::config::Config.
//
// Covers:
//   - load_default()       — pure built-in defaults (no I/O)
//   - load_from_env()      — parse every BATBOX_* key from an EnvMap
//   - load()               — full merge: env > settings_json > defaults
//   - validate()           — field-level validation (URL shape, ranges, etc.)
//   - to_json()            — full serialisation for /config show
//   - redacted_for_display() — redacted serialisation (api_key → "****")
//   - Enum parse/to_string helpers
//
// Precedence implemented in load():
//   1. Start with load_default() (lowest priority)
//   2. Apply settings_json values (JSON "config" sub-object if present)
//   3. Apply env overrides (highest priority)
//   4. Call validate()
// ---------------------------------------------------------------------------

#include <batbox/config/Config.hpp>
#include <batbox/core/Paths.hpp>
#include <batbox/core/Logging.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace batbox::config {

// ============================================================================
// Internal helpers
// ============================================================================
namespace {

/// Case-insensitive ASCII lower.
static std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

/// Parse a bool from the set {true,false,1,0,yes,no} case-insensitively.
/// Returns Err("<key>: invalid boolean '<value>'") on unrecognised input.
static Result<bool, std::string> parse_bool(std::string_view key, std::string_view raw) {
    const auto lc = to_lower(raw);
    if (lc == "true"  || lc == "1" || lc == "yes") return true;
    if (lc == "false" || lc == "0" || lc == "no")  return false;
    return Err(std::string(key) + ": invalid boolean '" + std::string(raw) + "' (use true/false/1/0/yes/no)");
}

/// Parse a non-negative int. Returns Err("<key>: invalid integer '<value>'") on failure.
static Result<int, std::string> parse_int(std::string_view key, std::string_view raw) {
    const auto trimmed = [&]{
        std::string_view s = raw;
        while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
        while (!s.empty() && s.back()  == ' ') s.remove_suffix(1);
        return s;
    }();

    if (trimmed.empty()) {
        return Err(std::string(key) + ": invalid integer '" + std::string(raw) + "'");
    }

    int val = 0;
    const auto [ptr, ec] = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), val);
    if (ec != std::errc{} || ptr != trimmed.data() + trimmed.size()) {
        return Err(std::string(key) + ": invalid integer '" + std::string(raw) + "'");
    }
    return val;
}

/// Parse a double. Returns Err("<key>: invalid number '<value>'") on failure.
static Result<double, std::string> parse_double(std::string_view key, std::string_view raw) {
    const std::string s(raw);
    try {
        std::size_t pos = 0;
        const double val = std::stod(s, &pos);
        // Ensure entire string was consumed (ignoring trailing whitespace).
        auto rest = std::string_view(s).substr(pos);
        while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
        if (!rest.empty()) {
            return Err(std::string(key) + ": invalid number '" + std::string(raw) + "'");
        }
        return val;
    } catch (const std::exception&) {
        return Err(std::string(key) + ": invalid number '" + std::string(raw) + "'");
    }
}

/// Split a comma-separated string into a vector of trimmed, non-empty tokens.
static std::vector<std::string> split_csv(std::string_view s) {
    std::vector<std::string> result;
    while (!s.empty()) {
        const auto comma = s.find(',');
        auto token = (comma == std::string_view::npos) ? s : s.substr(0, comma);
        // Trim whitespace.
        while (!token.empty() && token.front() == ' ') token.remove_prefix(1);
        while (!token.empty() && token.back()  == ' ') token.remove_suffix(1);
        if (!token.empty()) result.emplace_back(token);
        if (comma == std::string_view::npos) break;
        s = s.substr(comma + 1);
    }
    return result;
}

/// Minimal URL shape check: must start with "http://" or "https://".
static bool is_url_shaped(std::string_view s) {
    return s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0;
}

/// Resolve a path field: expand tilde if present.
static std::filesystem::path resolve_path(std::string_view s) {
    return batbox::paths::expand_tilde(s);
}

// ---------------------------------------------------------------------------
// Macro-free per-key extraction helpers that return Err on bad values.
// Each follows the pattern: if key present → parse → assign → propagate error.
// ---------------------------------------------------------------------------

/// Look up a value from env by trying each name in order.
/// Returns the first non-empty value found, or default_val if none present.
/// When a fallback name (not the first name) is used, sets *which_out to that
/// name so the caller can emit a one-time INFO log.
static std::string getenv_with_fallback(
        const EnvMap& env,
        std::initializer_list<const char*> names,
        std::string default_val,
        const char** which_out = nullptr) {
    const char* canonical = (names.size() > 0) ? *names.begin() : nullptr;
    bool first = true;
    for (const char* name : names) {
        const auto it = env.find(std::string(name));
        if (it != env.end() && !it->second.empty()) {
            if (!first && which_out != nullptr) {
                *which_out = name;
            }
            return it->second;
        }
        first = false;
    }
    (void)canonical;
    return default_val;
}

/// Try to get a string key and apply a transform function.
template<typename Fn>
static Result<void,std::string> apply_if_present(
        const EnvMap& env,
        std::string_view key,
        Fn fn) {
    const auto it = env.find(std::string(key));
    if (it == env.end()) return {};   // absent → keep default
    return fn(it->second);
}

} // anonymous namespace

// ============================================================================
// Enum helpers
// ============================================================================

Result<LogLevel, std::string> Config::parse_log_level(std::string_view s) {
    const auto lc = to_lower(s);
    if (lc == "trace") return LogLevel::Trace;
    if (lc == "debug") return LogLevel::Debug;
    if (lc == "info")  return LogLevel::Info;
    if (lc == "warn")  return LogLevel::Warn;
    if (lc == "error") return LogLevel::Error;
    return Err("BATBOX_LOG_LEVEL: invalid value '" + std::string(s) +
               "' (allowed: trace, debug, info, warn, error)");
}

Result<Theme, std::string> Config::parse_theme(std::string_view s) {
    const auto lc = to_lower(s);
    if (lc == "miss-kittin")    return Theme::MissKittin;
    if (lc == "stock-exchange") return Theme::StockExchange;
    if (lc == "frank-sinatra")  return Theme::FrankSinatra;
    if (lc == "monochrome")     return Theme::Monochrome;
    if (lc == "classic")        return Theme::Classic;
    return Err("BATBOX_THEME: invalid value '" + std::string(s) +
               "' (allowed: miss-kittin, stock-exchange, frank-sinatra, monochrome, classic)");
}

Result<StatusLine, std::string> Config::parse_statusline(std::string_view s) {
    const auto lc = to_lower(s);
    if (lc == "default") return StatusLine::Default;
    if (lc == "minimal") return StatusLine::Minimal;
    if (lc == "verbose") return StatusLine::Verbose;
    return Err("BATBOX_STATUSLINE: invalid value '" + std::string(s) +
               "' (allowed: default, minimal, verbose)");
}

Result<SearchEngine, std::string> Config::parse_search_engine(std::string_view s) {
    const auto lc = to_lower(s);
    if (lc == "ddg")     return SearchEngine::Ddg;
    if (lc == "searxng") return SearchEngine::Searxng;
    return Err("BATBOX_SEARCH_ENGINE: invalid value '" + std::string(s) +
               "' (allowed: ddg, searxng)");
}

Result<PermissionMode, std::string> Config::parse_permission_mode(std::string_view s) {
    const auto lc = to_lower(s);
    if (lc == "default")      return PermissionMode::Default;
    if (lc == "plan")         return PermissionMode::Plan;
    if (lc == "acceptedits")  return PermissionMode::AcceptEdits;
    if (lc == "nuclear")      return PermissionMode::Nuclear;
    return Err("BATBOX_PERMISSION_MODE: invalid value '" + std::string(s) +
               "' (allowed: default, plan, acceptEdits, nuclear)");
}

std::string Config::to_string(LogLevel v) {
    switch (v) {
        case LogLevel::Trace: return "trace";
        case LogLevel::Debug: return "debug";
        case LogLevel::Info:  return "info";
        case LogLevel::Warn:  return "warn";
        case LogLevel::Error: return "error";
    }
    return "info";
}

std::string Config::to_string(Theme v) {
    switch (v) {
        case Theme::MissKittin:    return "miss-kittin";
        case Theme::StockExchange: return "stock-exchange";
        case Theme::FrankSinatra:  return "frank-sinatra";
        case Theme::Monochrome:    return "monochrome";
        case Theme::Classic:       return "classic";
    }
    return "miss-kittin";
}

std::string Config::to_string(StatusLine v) {
    switch (v) {
        case StatusLine::Default: return "default";
        case StatusLine::Minimal: return "minimal";
        case StatusLine::Verbose: return "verbose";
    }
    return "default";
}

std::string Config::to_string(SearchEngine v) {
    switch (v) {
        case SearchEngine::Ddg:     return "ddg";
        case SearchEngine::Searxng: return "searxng";
    }
    return "ddg";
}

std::string Config::to_string(PermissionMode v) {
    switch (v) {
        case PermissionMode::Default:     return "default";
        case PermissionMode::Plan:        return "plan";
        case PermissionMode::AcceptEdits: return "acceptEdits";
        case PermissionMode::Nuclear:     return "nuclear";
    }
    return "default";
}

// ============================================================================
// Copy / move for Config (std::atomic is not copyable)
// ============================================================================

Config::Config(const Config& o)
    : api(o.api), general(o.general), ui(o.ui), search(o.search),
      sidecar(o.sidecar), tools(o.tools), mcp(o.mcp), agents(o.agents),
      security(o.security), compact(o.compact),
      version_seq(o.version_seq.load(std::memory_order_relaxed))
{}

Config::Config(Config&& o) noexcept
    : api(std::move(o.api)), general(std::move(o.general)), ui(std::move(o.ui)),
      search(std::move(o.search)), sidecar(std::move(o.sidecar)),
      tools(std::move(o.tools)), mcp(std::move(o.mcp)), agents(std::move(o.agents)),
      security(std::move(o.security)), compact(std::move(o.compact)),
      version_seq(o.version_seq.load(std::memory_order_relaxed))
{}

Config& Config::operator=(const Config& o) {
    if (this == &o) return *this;
    api      = o.api;
    general  = o.general;
    ui       = o.ui;
    search   = o.search;
    sidecar  = o.sidecar;
    tools    = o.tools;
    mcp      = o.mcp;
    agents   = o.agents;
    security = o.security;
    compact  = o.compact;
    version_seq.store(o.version_seq.load(std::memory_order_relaxed),
                      std::memory_order_relaxed);
    return *this;
}

Config& Config::operator=(Config&& o) noexcept {
    if (this == &o) return *this;
    api      = std::move(o.api);
    general  = std::move(o.general);
    ui       = std::move(o.ui);
    search   = std::move(o.search);
    sidecar  = std::move(o.sidecar);
    tools    = std::move(o.tools);
    mcp      = std::move(o.mcp);
    agents   = std::move(o.agents);
    security = std::move(o.security);
    compact  = std::move(o.compact);
    version_seq.store(o.version_seq.load(std::memory_order_relaxed),
                      std::memory_order_relaxed);
    return *this;
}

// ============================================================================
// load_default()
// ============================================================================

Config Config::load_default() {
    Config cfg;
    // All sub-struct fields are initialised to their documented defaults via
    // in-class initializers in Config.hpp — nothing to do here except ensure
    // path fields containing tildes are left as literals (resolve on first use,
    // or via resolve_path() when actually opening a file).
    cfg.version_seq.store(0, std::memory_order_relaxed);
    return cfg;
}

// ============================================================================
// apply_env() — overlay EnvMap onto an existing Config
// ============================================================================
// Returns the first error encountered; continues overlay for independent keys.
// (Strict: first bad value stops the whole parse.)

static Result<void, std::string> apply_env(Config& cfg, const EnvMap& env) {

    // --- API group -----------------------------------------------------------
    // Resolve BATBOX_MODELS first so the model fallback chain can reference it.
    if (auto it = env.find("BATBOX_MODELS"); it != env.end()) {
        auto tokens = split_csv(it->second);
        if (!tokens.empty()) cfg.api.models = std::move(tokens);
    }

    {
        const char* which = nullptr;
        std::string val = getenv_with_fallback(env,
            {"BATBOX_API_BASE_URL", "OPENAI_BASE_URL"},
            cfg.api.base_url, &which);
        cfg.api.base_url = val;
        if (which != nullptr) {
            BATBOX_LOG_INFO("Config: using {} (BATBOX_API_BASE_URL unset)", which);
        }
    }
    {
        const char* which = nullptr;
        std::string val = getenv_with_fallback(env,
            {"BATBOX_API_KEY", "OPENAI_API_KEY"},
            cfg.api.api_key, &which);
        cfg.api.api_key = val;
        if (which != nullptr) {
            BATBOX_LOG_INFO("Config: using {} (BATBOX_API_KEY unset)", which);
        }
    }
    {
        // Model precedence: BATBOX_DEFAULT_MODEL > BATBOX_MODEL > first of BATBOX_MODELS > built-in default
        const char* which = nullptr;
        // Build fallback default: use first entry of already-resolved cfg.api.models if available,
        // otherwise the original built-in default string (will be in cfg.api.default_model from load_default).
        std::string model_fallback = cfg.api.default_model;
        if (!cfg.api.models.empty()) {
            model_fallback = cfg.api.models.front();
        }
        std::string val = getenv_with_fallback(env,
            {"BATBOX_DEFAULT_MODEL", "BATBOX_MODEL"},
            model_fallback, &which);
        cfg.api.default_model = val;
        if (which != nullptr) {
            BATBOX_LOG_INFO("Config: using {} (BATBOX_DEFAULT_MODEL unset)", which);
        }
    }

    // --- Model alias fields --------------------------------------------------
    // These MUST be populated after cfg.api.default_model is resolved so the
    // fallback (empty env var → use default_model) works correctly.
    cfg.api.opus_model   = getenv_with_fallback(env,
        {"BATBOX_OPUS_MODEL"},   cfg.api.default_model);
    cfg.api.sonnet_model = getenv_with_fallback(env,
        {"BATBOX_SONNET_MODEL"}, cfg.api.default_model);
    cfg.api.haiku_model  = getenv_with_fallback(env,
        {"BATBOX_HAIKU_MODEL"},  cfg.api.default_model);

    if (auto it = env.find("BATBOX_MAX_TOKENS"); it != env.end()) {
        auto r = parse_int("BATBOX_MAX_TOKENS", it->second);
        if (!r) return Err(r.error());
        cfg.api.max_tokens = *r;
    }
    if (auto it = env.find("BATBOX_TEMPERATURE"); it != env.end()) {
        auto r = parse_double("BATBOX_TEMPERATURE", it->second);
        if (!r) return Err(r.error());
        cfg.api.temperature = *r;
    }
    if (auto it = env.find("BATBOX_TOP_P"); it != env.end()) {
        auto r = parse_double("BATBOX_TOP_P", it->second);
        if (!r) return Err(r.error());
        cfg.api.top_p = *r;
    }
    if (auto it = env.find("BATBOX_REQUEST_TIMEOUT_SEC"); it != env.end()) {
        auto r = parse_int("BATBOX_REQUEST_TIMEOUT_SEC", it->second);
        if (!r) return Err(r.error());
        cfg.api.request_timeout_sec = *r;
    }
    if (auto it = env.find("BATBOX_STREAM"); it != env.end()) {
        auto r = parse_bool("BATBOX_STREAM", it->second);
        if (!r) return Err(r.error());
        cfg.api.stream = *r;
    }

    // --- General group -------------------------------------------------------
    if (auto it = env.find("BATBOX_CONFIG_DIR"); it != env.end()) {
        cfg.general.config_dir = resolve_path(it->second);
    }
    if (auto it = env.find("BATBOX_PROJECT_MEMORY_FILE"); it != env.end()) {
        cfg.general.project_memory_file = it->second;
    }
    if (auto it = env.find("BATBOX_LOG_LEVEL"); it != env.end()) {
        auto r = Config::parse_log_level(it->second);
        if (!r) return Err(r.error());
        cfg.general.log_level = *r;
    }
    if (auto it = env.find("BATBOX_LOG_FILE"); it != env.end()) {
        cfg.general.log_file = resolve_path(it->second);
    }

    // --- UI group ------------------------------------------------------------
    if (auto it = env.find("BATBOX_THEME"); it != env.end()) {
        auto r = Config::parse_theme(it->second);
        if (!r) return Err(r.error());
        cfg.ui.theme = *r;
    }
    if (auto it = env.find("BATBOX_NO_SPLASH"); it != env.end()) {
        auto r = parse_bool("BATBOX_NO_SPLASH", it->second);
        if (!r) return Err(r.error());
        cfg.ui.no_splash = *r;
    }
    if (auto it = env.find("BATBOX_VIM_MODE"); it != env.end()) {
        auto r = parse_bool("BATBOX_VIM_MODE", it->second);
        if (!r) return Err(r.error());
        cfg.ui.vim_mode = *r;
    }
    if (auto it = env.find("BATBOX_STATUSLINE"); it != env.end()) {
        auto r = Config::parse_statusline(it->second);
        if (!r) return Err(r.error());
        cfg.ui.statusline = *r;
    }

    // --- Search / WebFetch group ---------------------------------------------
    if (auto it = env.find("BATBOX_SEARCH_ENGINE"); it != env.end()) {
        auto r = Config::parse_search_engine(it->second);
        if (!r) return Err(r.error());
        cfg.search.engine = *r;
    }
    if (auto it = env.find("BATBOX_SEARXNG_URL"); it != env.end()) {
        cfg.search.searxng_url = it->second;
    }
    if (auto it = env.find("BATBOX_RESPECT_ROBOTS"); it != env.end()) {
        auto r = parse_bool("BATBOX_RESPECT_ROBOTS", it->second);
        if (!r) return Err(r.error());
        cfg.search.respect_robots = *r;
    }
    if (auto it = env.find("BATBOX_WEBFETCH_MAX_BYTES"); it != env.end()) {
        auto r = parse_int("BATBOX_WEBFETCH_MAX_BYTES", it->second);
        if (!r) return Err(r.error());
        cfg.search.webfetch_max_bytes = *r;
    }
    if (auto it = env.find("BATBOX_WEBFETCH_TIMEOUT_SEC"); it != env.end()) {
        auto r = parse_int("BATBOX_WEBFETCH_TIMEOUT_SEC", it->second);
        if (!r) return Err(r.error());
        cfg.search.webfetch_timeout_sec = *r;
    }

    // --- Sidecar group -------------------------------------------------------
    if (auto it = env.find("BATBOX_SIDECAR_PYTHON"); it != env.end()) {
        cfg.sidecar.python = resolve_path(it->second);
    }
    if (auto it = env.find("BATBOX_SIDECAR_VENV"); it != env.end()) {
        cfg.sidecar.venv = resolve_path(it->second);
    }
    if (auto it = env.find("BATBOX_SIDECAR_STARTUP_TIMEOUT_SEC"); it != env.end()) {
        auto r = parse_int("BATBOX_SIDECAR_STARTUP_TIMEOUT_SEC", it->second);
        if (!r) return Err(r.error());
        cfg.sidecar.startup_timeout_sec = *r;
    }
    if (auto it = env.find("BATBOX_SIDECAR_AUTOSTART"); it != env.end()) {
        auto r = parse_bool("BATBOX_SIDECAR_AUTOSTART", it->second);
        if (!r) return Err(r.error());
        cfg.sidecar.autostart = *r;
    }
    if (auto it = env.find("BATBOX_SIDECAR_PREWARM"); it != env.end()) {
        auto r = parse_bool("BATBOX_SIDECAR_PREWARM", it->second);
        if (!r) return Err(r.error());
        cfg.sidecar.prewarm = *r;
    }

    // --- Tools group ---------------------------------------------------------
    if (auto it = env.find("BATBOX_BASH_TIMEOUT_SEC"); it != env.end()) {
        auto r = parse_int("BATBOX_BASH_TIMEOUT_SEC", it->second);
        if (!r) return Err(r.error());
        cfg.tools.bash_timeout_sec = *r;
    }
    if (auto it = env.find("BATBOX_BASH_MAX_OUTPUT_BYTES"); it != env.end()) {
        auto r = parse_int("BATBOX_BASH_MAX_OUTPUT_BYTES", it->second);
        if (!r) return Err(r.error());
        cfg.tools.bash_max_output_bytes = *r;
    }
    if (auto it = env.find("BATBOX_TASK_PARALLEL_LIMIT"); it != env.end()) {
        auto r = parse_int("BATBOX_TASK_PARALLEL_LIMIT", it->second);
        if (!r) return Err(r.error());
        cfg.tools.task_parallel_limit = *r;
    }

    // --- MCP group -----------------------------------------------------------
    if (auto it = env.find("BATBOX_MCP_CONFIG"); it != env.end()) {
        cfg.mcp.config_path = resolve_path(it->second);
    }
    if (auto it = env.find("BATBOX_MCP_STARTUP_TIMEOUT_SEC"); it != env.end()) {
        auto r = parse_int("BATBOX_MCP_STARTUP_TIMEOUT_SEC", it->second);
        if (!r) return Err(r.error());
        cfg.mcp.startup_timeout_sec = *r;
    }

    // --- Agents group --------------------------------------------------------
    if (auto it = env.find("BATBOX_AGENTS_CONFIG"); it != env.end()) {
        cfg.agents.agents_config = resolve_path(it->second);
    }
    if (auto it = env.find("BATBOX_AGENTS_DIR"); it != env.end()) {
        cfg.agents.agents_dir = resolve_path(it->second);
    }
    if (auto it = env.find("BATBOX_DEMON_ENABLED"); it != env.end()) {
        auto r = parse_bool("BATBOX_DEMON_ENABLED", it->second);
        if (!r) return Err(r.error());
        cfg.agents.demon_enabled = *r;
    }

    // --- Security group ------------------------------------------------------
    if (auto it = env.find("BATBOX_PERMISSION_MODE"); it != env.end()) {
        auto r = Config::parse_permission_mode(it->second);
        if (!r) return Err(r.error());
        cfg.security.permission_mode = *r;
    }
    if (auto it = env.find("BATBOX_AUTO_APPROVE_READS"); it != env.end()) {
        auto r = parse_bool("BATBOX_AUTO_APPROVE_READS", it->second);
        if (!r) return Err(r.error());
        cfg.security.auto_approve_reads = *r;
    }

    // --- Compact group -------------------------------------------------------
    if (auto it = env.find("BATBOX_AUTO_COMPACT_AT_PCT"); it != env.end()) {
        auto r = parse_int("BATBOX_AUTO_COMPACT_AT_PCT", it->second);
        if (!r) return Err(r.error());
        cfg.compact.auto_compact_at_pct = *r;
    }
    if (auto it = env.find("BATBOX_KEEP_LAST_N_TURNS_VERBATIM"); it != env.end()) {
        auto r = parse_int("BATBOX_KEEP_LAST_N_TURNS_VERBATIM", it->second);
        if (!r) return Err(r.error());
        cfg.compact.keep_last_n_turns_verbatim = *r;
    }

    // --- Sidecar python auto-resolution --------------------------------------
    // If python_bin is still the built-in default "python3" (i.e. not
    // overridden by BATBOX_SIDECAR_PYTHON or settings.json) AND the standard
    // venv python exists on disk, resolve it to the absolute venv path.
    // This prevents posix_spawnp from picking up the Xcode python3 on macOS
    // via PATH resolution instead of the venv interpreter.
    //
    // BATBOX_SIDECAR_PYTHON, when explicitly set above, is run through
    // resolve_path() which expands tildes — the result is never the bare
    // literal "python3", so an explicit user choice is always respected.
    //
    // We derive the config directory from the already-resolved cfg.general.config_dir
    // (which was populated from EnvMap above) rather than calling
    // batbox::paths::config_dir() (which reads the real process environment).
    // This ensures BATBOX_CONFIG_DIR overrides in the EnvMap are respected.
    {
        if (cfg.sidecar.python == std::filesystem::path("python3") ||
            cfg.sidecar.python.empty()) {
            // Expand tilde in config_dir in case it still holds the default "~/.batbox".
            const std::filesystem::path cfg_dir =
                resolve_path(cfg.general.config_dir.string());
            const std::filesystem::path venv_python =
                cfg_dir / "sidecar" / ".venv" / "bin" / "python3";
            if (std::filesystem::exists(venv_python)) {
                cfg.sidecar.python = venv_python;
                BATBOX_LOG_INFO("Config: sidecar python resolved to {}", venv_python.string());
            }
        }
    }

    return {};

}

// ============================================================================
// apply_settings_json() — overlay settings.json "config" sub-object
// ============================================================================
// Uses the same field names as to_json() for round-trip compatibility.
// Invalid or absent JSON keys are silently skipped (permissive — env and
// validate() handle correctness).

static void apply_settings_json(Config& cfg, const Json& j) {
    if (!j.is_object()) return;

    // Helper: return string from JSON, or empty if absent/not-string.
    auto js = [&](const char* section, const char* field) -> std::string {
        try {
            if (j.contains(section) && j[section].is_object() &&
                j[section].contains(field) && j[section][field].is_string()) {
                return j[section][field].get<std::string>();
            }
        } catch (...) {}
        return {};
    };
    auto jb = [&](const char* section, const char* field, bool fallback) -> bool {
        try {
            if (j.contains(section) && j[section].is_object() &&
                j[section].contains(field) && j[section][field].is_boolean()) {
                return j[section][field].get<bool>();
            }
        } catch (...) {}
        return fallback;
    };
    auto ji = [&](const char* section, const char* field, int fallback) -> int {
        try {
            if (j.contains(section) && j[section].is_object() &&
                j[section].contains(field) && j[section][field].is_number_integer()) {
                return j[section][field].get<int>();
            }
        } catch (...) {}
        return fallback;
    };
    auto jd = [&](const char* section, const char* field, double fallback) -> double {
        try {
            if (j.contains(section) && j[section].is_object() &&
                j[section].contains(field) && j[section][field].is_number()) {
                return j[section][field].get<double>();
            }
        } catch (...) {}
        return fallback;
    };

    // API
    if (const auto v = js("api", "base_url"); !v.empty()) cfg.api.base_url = v;
    if (const auto v = js("api", "api_key");  !v.empty()) cfg.api.api_key  = v;
    if (const auto v = js("api", "default_model"); !v.empty()) cfg.api.default_model = v;
    if (j.contains("api") && j["api"].is_object() &&
        j["api"].contains("models") && j["api"]["models"].is_array()) {
        std::vector<std::string> ms;
        for (const auto& m : j["api"]["models"]) {
            if (m.is_string()) ms.push_back(m.get<std::string>());
        }
        if (!ms.empty()) cfg.api.models = std::move(ms);
    }
    cfg.api.max_tokens          = ji("api", "max_tokens",         cfg.api.max_tokens);
    cfg.api.temperature         = jd("api", "temperature",        cfg.api.temperature);
    cfg.api.top_p               = jd("api", "top_p",              cfg.api.top_p);
    cfg.api.request_timeout_sec = ji("api", "request_timeout_sec",cfg.api.request_timeout_sec);
    cfg.api.stream              = jb("api", "stream",             cfg.api.stream);

    // General
    if (const auto v = js("general", "config_dir"); !v.empty())
        cfg.general.config_dir = resolve_path(v);
    if (const auto v = js("general", "project_memory_file"); !v.empty())
        cfg.general.project_memory_file = v;
    if (const auto v = js("general", "log_level"); !v.empty()) {
        auto r = Config::parse_log_level(v);
        if (r) cfg.general.log_level = *r;
    }
    if (const auto v = js("general", "log_file"); !v.empty())
        cfg.general.log_file = resolve_path(v);

    // UI
    if (const auto v = js("ui", "theme"); !v.empty()) {
        auto r = Config::parse_theme(v);
        if (r) cfg.ui.theme = *r;
    }
    cfg.ui.no_splash = jb("ui", "no_splash", cfg.ui.no_splash);
    cfg.ui.vim_mode  = jb("ui", "vim_mode",  cfg.ui.vim_mode);
    if (const auto v = js("ui", "statusline"); !v.empty()) {
        auto r = Config::parse_statusline(v);
        if (r) cfg.ui.statusline = *r;
    }

    // Search
    if (const auto v = js("search", "engine"); !v.empty()) {
        auto r = Config::parse_search_engine(v);
        if (r) cfg.search.engine = *r;
    }
    if (const auto v = js("search", "searxng_url"); !v.empty()) cfg.search.searxng_url = v;
    cfg.search.respect_robots      = jb("search", "respect_robots",      cfg.search.respect_robots);
    cfg.search.webfetch_max_bytes  = ji("search", "webfetch_max_bytes",   cfg.search.webfetch_max_bytes);
    cfg.search.webfetch_timeout_sec= ji("search", "webfetch_timeout_sec", cfg.search.webfetch_timeout_sec);

    // Sidecar
    if (const auto v = js("sidecar", "python"); !v.empty()) cfg.sidecar.python = resolve_path(v);
    if (const auto v = js("sidecar", "venv");   !v.empty()) cfg.sidecar.venv   = resolve_path(v);
    cfg.sidecar.startup_timeout_sec = ji("sidecar", "startup_timeout_sec", cfg.sidecar.startup_timeout_sec);
    cfg.sidecar.autostart  = jb("sidecar", "autostart",  cfg.sidecar.autostart);
    cfg.sidecar.prewarm    = jb("sidecar", "prewarm",    cfg.sidecar.prewarm);

    // Tools
    cfg.tools.bash_timeout_sec     = ji("tools", "bash_timeout_sec",     cfg.tools.bash_timeout_sec);
    cfg.tools.bash_max_output_bytes= ji("tools", "bash_max_output_bytes", cfg.tools.bash_max_output_bytes);
    cfg.tools.task_parallel_limit  = ji("tools", "task_parallel_limit",  cfg.tools.task_parallel_limit);

    // MCP
    if (const auto v = js("mcp", "config_path"); !v.empty()) cfg.mcp.config_path = resolve_path(v);
    cfg.mcp.startup_timeout_sec = ji("mcp", "startup_timeout_sec", cfg.mcp.startup_timeout_sec);

    // Agents
    if (const auto v = js("agents", "agents_config"); !v.empty()) cfg.agents.agents_config = resolve_path(v);
    if (const auto v = js("agents", "agents_dir");    !v.empty()) cfg.agents.agents_dir    = resolve_path(v);
    cfg.agents.demon_enabled = jb("agents", "demon_enabled", cfg.agents.demon_enabled);

    // Security
    if (const auto v = js("security", "permission_mode"); !v.empty()) {
        auto r = Config::parse_permission_mode(v);
        if (r) cfg.security.permission_mode = *r;
    }
    cfg.security.auto_approve_reads = jb("security", "auto_approve_reads", cfg.security.auto_approve_reads);

    // Compact
    cfg.compact.auto_compact_at_pct       = ji("compact", "auto_compact_at_pct",       cfg.compact.auto_compact_at_pct);
    cfg.compact.keep_last_n_turns_verbatim= ji("compact", "keep_last_n_turns_verbatim", cfg.compact.keep_last_n_turns_verbatim);
}

// ============================================================================
// load_from_env()
// ============================================================================

Result<Config, std::string> Config::load_from_env(const EnvMap& env) {
    Config cfg = load_default();
    if (auto r = apply_env(cfg, env); !r) {
        return Err(r.error());
    }
    if (auto r = cfg.validate(); !r) {
        return Err(r.error());
    }
    return cfg;
}

// ============================================================================
// load()  —  env > settings_json > defaults
// ============================================================================

Result<Config, std::string> Config::load(const EnvMap& env, const Json& settings_json) {
    Config cfg = load_default();

    // Layer 2: settings.json (lower priority than env).
    apply_settings_json(cfg, settings_json);

    // Layer 1: env overrides (highest priority).
    if (auto r = apply_env(cfg, env); !r) {
        return Err(r.error());
    }

    if (auto r = cfg.validate(); !r) {
        return Err(r.error());
    }
    return cfg;
}

// ============================================================================
// validate()
// ============================================================================

Result<void, std::string> Config::validate() const {
    // api.base_url must be URL-shaped if non-empty.
    if (!api.base_url.empty() && !is_url_shaped(api.base_url)) {
        return Err("BATBOX_API_BASE_URL: must start with http:// or https:// (got '" +
                   api.base_url + "')");
    }
    // models must be non-empty.
    if (api.models.empty()) {
        return Err("BATBOX_MODELS: must contain at least one model name");
    }
    // max_tokens must be positive.
    if (api.max_tokens <= 0) {
        return Err("BATBOX_MAX_TOKENS: must be > 0 (got " + std::to_string(api.max_tokens) + ")");
    }
    // temperature must be in [0, 2].
    if (api.temperature < 0.0 || api.temperature > 2.0) {
        return Err("BATBOX_TEMPERATURE: must be in [0, 2] (got " +
                   std::to_string(api.temperature) + ")");
    }
    // top_p must be in (0, 1].
    if (api.top_p <= 0.0 || api.top_p > 1.0) {
        return Err("BATBOX_TOP_P: must be in (0, 1] (got " + std::to_string(api.top_p) + ")");
    }
    // request_timeout_sec must be positive.
    if (api.request_timeout_sec <= 0) {
        return Err("BATBOX_REQUEST_TIMEOUT_SEC: must be > 0 (got " +
                   std::to_string(api.request_timeout_sec) + ")");
    }
    // bash_timeout_sec must be positive.
    if (tools.bash_timeout_sec <= 0) {
        return Err("BATBOX_BASH_TIMEOUT_SEC: must be > 0 (got " +
                   std::to_string(tools.bash_timeout_sec) + ")");
    }
    // bash_max_output_bytes must be positive.
    if (tools.bash_max_output_bytes <= 0) {
        return Err("BATBOX_BASH_MAX_OUTPUT_BYTES: must be > 0 (got " +
                   std::to_string(tools.bash_max_output_bytes) + ")");
    }
    // task_parallel_limit must be in [1, 64].
    if (tools.task_parallel_limit < 1 || tools.task_parallel_limit > 64) {
        return Err("BATBOX_TASK_PARALLEL_LIMIT: must be in [1, 64] (got " +
                   std::to_string(tools.task_parallel_limit) + ")");
    }
    // sidecar startup timeout positive.
    if (sidecar.startup_timeout_sec <= 0) {
        return Err("BATBOX_SIDECAR_STARTUP_TIMEOUT_SEC: must be > 0 (got " +
                   std::to_string(sidecar.startup_timeout_sec) + ")");
    }
    // mcp startup timeout positive.
    if (mcp.startup_timeout_sec <= 0) {
        return Err("BATBOX_MCP_STARTUP_TIMEOUT_SEC: must be > 0 (got " +
                   std::to_string(mcp.startup_timeout_sec) + ")");
    }
    // webfetch_max_bytes positive.
    if (search.webfetch_max_bytes <= 0) {
        return Err("BATBOX_WEBFETCH_MAX_BYTES: must be > 0 (got " +
                   std::to_string(search.webfetch_max_bytes) + ")");
    }
    // webfetch_timeout_sec positive.
    if (search.webfetch_timeout_sec <= 0) {
        return Err("BATBOX_WEBFETCH_TIMEOUT_SEC: must be > 0 (got " +
                   std::to_string(search.webfetch_timeout_sec) + ")");
    }
    // auto_compact_at_pct in [1, 100].
    if (compact.auto_compact_at_pct < 1 || compact.auto_compact_at_pct > 100) {
        return Err("BATBOX_AUTO_COMPACT_AT_PCT: must be in [1, 100] (got " +
                   std::to_string(compact.auto_compact_at_pct) + ")");
    }
    // keep_last_n_turns_verbatim non-negative.
    if (compact.keep_last_n_turns_verbatim < 0) {
        return Err("BATBOX_KEEP_LAST_N_TURNS_VERBATIM: must be >= 0 (got " +
                   std::to_string(compact.keep_last_n_turns_verbatim) + ")");
    }
    // If search engine is searxng, searxng_url must be URL-shaped if set.
    if (search.engine == SearchEngine::Searxng &&
        !search.searxng_url.empty() &&
        !is_url_shaped(search.searxng_url)) {
        return Err("BATBOX_SEARXNG_URL: must start with http:// or https:// (got '" +
                   search.searxng_url + "')");
    }
    return {};
}

// ============================================================================
// to_json()
// ============================================================================

Json Config::to_json() const {
    Json j;

    j["api"]["base_url"]           = api.base_url;
    j["api"]["api_key"]            = api.api_key;
    j["api"]["default_model"]      = api.default_model;
    j["api"]["models"]             = api.models;
    j["api"]["max_tokens"]         = api.max_tokens;
    j["api"]["temperature"]        = api.temperature;
    j["api"]["top_p"]              = api.top_p;
    j["api"]["request_timeout_sec"]= api.request_timeout_sec;
    j["api"]["stream"]             = api.stream;

    j["general"]["config_dir"]           = general.config_dir.string();
    j["general"]["project_memory_file"]  = general.project_memory_file;
    j["general"]["log_level"]            = to_string(general.log_level);
    j["general"]["log_file"]             = general.log_file.string();

    j["ui"]["theme"]     = to_string(ui.theme);
    j["ui"]["no_splash"] = ui.no_splash;
    j["ui"]["vim_mode"]  = ui.vim_mode;
    j["ui"]["statusline"]= to_string(ui.statusline);

    j["search"]["engine"]               = to_string(search.engine);
    j["search"]["searxng_url"]          = search.searxng_url;
    j["search"]["respect_robots"]       = search.respect_robots;
    j["search"]["webfetch_max_bytes"]   = search.webfetch_max_bytes;
    j["search"]["webfetch_timeout_sec"] = search.webfetch_timeout_sec;

    j["sidecar"]["python"]              = sidecar.python.string();
    j["sidecar"]["venv"]                = sidecar.venv.string();
    j["sidecar"]["startup_timeout_sec"] = sidecar.startup_timeout_sec;
    j["sidecar"]["autostart"]           = sidecar.autostart;
    j["sidecar"]["prewarm"]             = sidecar.prewarm;

    j["tools"]["bash_timeout_sec"]      = tools.bash_timeout_sec;
    j["tools"]["bash_max_output_bytes"] = tools.bash_max_output_bytes;
    j["tools"]["task_parallel_limit"]   = tools.task_parallel_limit;

    j["mcp"]["config_path"]         = mcp.config_path.string();
    j["mcp"]["startup_timeout_sec"] = mcp.startup_timeout_sec;

    j["agents"]["agents_config"]  = agents.agents_config.string();
    j["agents"]["agents_dir"]     = agents.agents_dir.string();
    j["agents"]["demon_enabled"]  = agents.demon_enabled;

    j["security"]["permission_mode"]   = to_string(security.permission_mode);
    j["security"]["auto_approve_reads"]= security.auto_approve_reads;

    j["compact"]["auto_compact_at_pct"]        = compact.auto_compact_at_pct;
    j["compact"]["keep_last_n_turns_verbatim"] = compact.keep_last_n_turns_verbatim;

    return j;
}

// ============================================================================
// redacted_for_display()
// ============================================================================

Json Config::redacted_for_display() const {
    Json j = to_json();
    j["api"]["api_key"] = "****";
    return j;
}

// ============================================================================
// resolve_model_alias
// ============================================================================

std::string resolve_model_alias(std::string_view name, const Config& cfg) {
    if (name == "opus")   return cfg.api.opus_model.empty()   ? cfg.api.default_model : cfg.api.opus_model;
    if (name == "sonnet") return cfg.api.sonnet_model.empty() ? cfg.api.default_model : cfg.api.sonnet_model;
    if (name == "haiku")  return cfg.api.haiku_model.empty()  ? cfg.api.default_model : cfg.api.haiku_model;
    return std::string(name);
}

} // namespace batbox::config
