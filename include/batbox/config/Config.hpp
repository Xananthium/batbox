// include/batbox/config/Config.hpp
// ---------------------------------------------------------------------------
// batbox::config::Config — top-level runtime configuration aggregate.
//
// Mirrors every BATBOX_* environment variable from pmdraft.md §"Environment
// Variables (full .env schema)". All ~40 variables are represented as strongly-
// typed fields grouped into nested sub-structs.
//
// Loading pipeline:
//   Config::load_default()                   → pure built-in defaults
//   Config::load_from_env(EnvMap)            → strict env-only parse
//   Config::load(EnvMap, Json settings_json) → full merge: env > settings_json > defaults
//
// Precedence (highest → lowest):
//   1. Environment variables (EnvMap, which already merged shell env + .env file)
//   2. ~/.batbox/settings.json ("config" sub-object)
//   3. Built-in defaults (load_default())
//
// Hot-reload:
//   version_seq is an std::atomic<uint64_t> incremented each time Config is
//   replaced. Components poll this to detect a change without holding a lock.
//
// Display helpers:
//   to_json()              → full config as nlohmann::json (for /config show)
//   redacted_for_display() → same but secret fields replaced with "****"
//
// Validation:
//   validate() → Result<void, std::string>
//   Called automatically by load(); returns the first validation failure found.
//   On error the message identifies the field name (e.g., "BATBOX_MAX_TOKENS must be > 0").
// ---------------------------------------------------------------------------
#pragma once

#include <batbox/core/Json.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/config/EnvLoader.hpp>

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace batbox::config {

// ============================================================================
// Enum types for fields with a fixed vocabulary
// ============================================================================

/// BATBOX_LOG_LEVEL — spdlog severity levels.
enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
};

/// BATBOX_THEME — named colour palettes.
enum class Theme {
    MissKittin,    // miss-kittin (default)
    StockExchange, // stock-exchange
    FrankSinatra,  // frank-sinatra
    Monochrome,    // monochrome
    Classic,       // classic (original claude-code colours)
};

/// BATBOX_STATUSLINE — status-line display variant.
enum class StatusLine {
    Default,
    Minimal,
    Verbose,
};

/// BATBOX_SEARCH_ENGINE — web-search backend routed through Scrapling sidecar.
enum class SearchEngine {
    Ddg,     // ddg  — DuckDuckGo HTML (default, no API key)
    Searxng, // searxng — user-hosted SearXNG instance
};

/// BATBOX_PERMISSION_MODE — tool-confirmation / safety mode.
/// Decision of Record #6: default | plan | acceptEdits | nuclear
enum class PermissionMode {
    Default,      // confirm before destructive actions
    Plan,         // read-only until plan approved
    AcceptEdits,  // auto-accept file edits; still confirm bash
    Nuclear,      // auto-accept everything (☢️)
};

// ============================================================================
// Nested config sub-structs
// ============================================================================

/// API / inference settings (BATBOX_API_* and related).
struct ApiConfig {
    std::string   base_url         = "https://api.openai.com/v1"; ///< BATBOX_API_BASE_URL
    std::string   api_key;                                         ///< BATBOX_API_KEY
    std::string   default_model    = "gpt-4o";                     ///< BATBOX_DEFAULT_MODEL
    std::vector<std::string> models = {"gpt-4o", "gpt-4o-mini"};  ///< BATBOX_MODELS (comma-sep)
    int           max_tokens       = 4096;                         ///< BATBOX_MAX_TOKENS
    double        temperature      = 0.7;                          ///< BATBOX_TEMPERATURE
    double        top_p            = 1.0;                          ///< BATBOX_TOP_P
    int           request_timeout_sec = 120;                       ///< BATBOX_REQUEST_TIMEOUT_SEC
    bool          stream           = true;                         ///< BATBOX_STREAM
    /// Provider hint for pre-request quirk handling.
    /// Valid values: openai | vllm | together | ollama | anthropic | groq | mistral | lm-studio | llama-cpp | auto
    /// When empty or "auto", the provider is detected from base_url.
    /// Unknown non-empty non-auto values fall back to openai semantics with a warning.
    std::string   provider_hint;                                    ///< BATBOX_PROVIDER_HINT

    /// Model alias for "opus" in agent spec frontmatter.
    /// Defaults to default_model when BATBOX_OPUS_MODEL is not set.
    std::string   opus_model;    ///< BATBOX_OPUS_MODEL

    /// Model alias for "sonnet" in agent spec frontmatter.
    /// Defaults to default_model when BATBOX_SONNET_MODEL is not set.
    std::string   sonnet_model;  ///< BATBOX_SONNET_MODEL

    /// Model alias for "haiku" in agent spec frontmatter.
    /// Defaults to default_model when BATBOX_HAIKU_MODEL is not set.
    std::string   haiku_model;   ///< BATBOX_HAIKU_MODEL

    /// Resolved context length for default_model, in tokens.
    ///
    /// Fallback chain (resolved ONCE at Config::load() time):
    ///   1. BATBOX_CTX_LEN_<MODEL_UPPER_UNDERSCORED>  — per-model env var
    ///      The MODEL is the RESOLVED model name (after resolve_model_alias()).
    ///      Non-alphanumeric characters are replaced with '_', then uppercased.
    ///      Example: "mistralai/magistral-small-2509"
    ///               → BATBOX_CTX_LEN_MISTRALAI_MAGISTRAL_SMALL_2509
    ///   2. BATBOX_CTX_LEN_DEFAULT                    — global default env var
    ///   3. Built-in model table (ContextWindow::context_limit_for_model)
    ///   4. 4096 (hard fallback)
    ///
    /// Never read via std::getenv at runtime — resolved once and stored here.
    std::size_t   default_model_ctx_len = 4096; ///< BATBOX_CTX_LEN_<MODEL> / BATBOX_CTX_LEN_DEFAULT

    /// Idle stream timeout in seconds.
    ///
    /// Maps to CURLOPT_LOW_SPEED_LIMIT=1 (bytes/sec) +
    /// CURLOPT_LOW_SPEED_TIME=<this value> so that a stalled upstream
    /// (zero bytes for this many seconds) is terminated and surfaced as an error.
    ///
    /// Default: 60 seconds. Set BATBOX_STREAM_IDLE_TIMEOUT_SEC=0 to disable.
    int           stream_idle_timeout_sec = 60; ///< BATBOX_STREAM_IDLE_TIMEOUT_SEC
};

/// General config paths / directories.
struct GeneralConfig {
    std::filesystem::path config_dir     = "~/.batbox";    ///< BATBOX_CONFIG_DIR
    std::string           project_memory_file = "BATBOX.md"; ///< BATBOX_PROJECT_MEMORY_FILE
    LogLevel              log_level      = LogLevel::Info;   ///< BATBOX_LOG_LEVEL
    std::filesystem::path log_file       = "~/.batbox/batbox.log"; ///< BATBOX_LOG_FILE
};

/// TUI / visual settings.
struct UiConfig {
    Theme      theme      = Theme::MissKittin; ///< BATBOX_THEME
    bool       no_splash  = false;             ///< BATBOX_NO_SPLASH
    bool       vim_mode   = false;             ///< BATBOX_VIM_MODE
    StatusLine statusline = StatusLine::Default; ///< BATBOX_STATUSLINE
};

/// Web-search + WebFetch settings (routed through Scrapling sidecar).
struct SearchConfig {
    SearchEngine engine            = SearchEngine::Ddg; ///< BATBOX_SEARCH_ENGINE
    std::string  searxng_url;                           ///< BATBOX_SEARXNG_URL
    bool         respect_robots    = true;              ///< BATBOX_RESPECT_ROBOTS
    int          webfetch_max_bytes     = 5'242'880;    ///< BATBOX_WEBFETCH_MAX_BYTES (5 MB)
    int          webfetch_timeout_sec   = 30;           ///< BATBOX_WEBFETCH_TIMEOUT_SEC
};

/// Python Scrapling sidecar lifecycle settings.
struct SidecarConfig {
    std::filesystem::path python  = "python3";                   ///< BATBOX_SIDECAR_PYTHON
    std::filesystem::path venv    = "~/.batbox/sidecar/.venv";   ///< BATBOX_SIDECAR_VENV
    int                   startup_timeout_sec = 15;              ///< BATBOX_SIDECAR_STARTUP_TIMEOUT_SEC
    bool                  autostart  = true;                     ///< BATBOX_SIDECAR_AUTOSTART
    bool                  prewarm    = false;                    ///< BATBOX_SIDECAR_PREWARM
};

/// Tool execution settings.
struct ToolsConfig {
    int  bash_timeout_sec    = 120;     ///< BATBOX_BASH_TIMEOUT_SEC
    int  bash_max_output_bytes = 1'048'576; ///< BATBOX_BASH_MAX_OUTPUT_BYTES (1 MB)
    int  task_parallel_limit = 4;       ///< BATBOX_TASK_PARALLEL_LIMIT
};

/// MCP server / transport settings.
struct McpConfig {
    std::filesystem::path config_path         = "~/.batbox/mcp.json"; ///< BATBOX_MCP_CONFIG
    int                   startup_timeout_sec = 10;                   ///< BATBOX_MCP_STARTUP_TIMEOUT_SEC
};

/// Sub-agent / Task-family settings (Decision of Record #8 — centerpiece).
struct AgentsConfig {
    std::filesystem::path agents_config = "~/.batbox/agents.json"; ///< BATBOX_AGENTS_CONFIG
    std::filesystem::path agents_dir    = "~/.batbox/agents";      ///< BATBOX_AGENTS_DIR
    bool                  demon_enabled = false;                   ///< BATBOX_DEMON_ENABLED
};

/// Permission / safety mode settings (Decision of Record #6).
struct SecurityConfig {
    PermissionMode permission_mode   = PermissionMode::Default; ///< BATBOX_PERMISSION_MODE
    bool           auto_approve_reads = true;                   ///< BATBOX_AUTO_APPROVE_READS
};

/// Context-window auto-compact behaviour settings.
struct CompactConfig {
    int  auto_compact_at_pct        = 80; ///< BATBOX_AUTO_COMPACT_AT_PCT
    int  keep_last_n_turns_verbatim = 10; ///< BATBOX_KEEP_LAST_N_TURNS_VERBATIM
};

/// Closed tool-subagent distillation settings (S1+S4, DIS-980).
///
/// When a tool result exceeds max_tool_response_size bytes, the subagent
/// dispatch envelope engulfs it into a ONE-SHOT distillation call on a LOCAL
/// OpenAI-compatible endpoint (the 3090s) and returns only the golden line.
/// The local endpoint is deliberately SEPARATE from ApiConfig (the main, often
/// cloud, model) — distillation is free local compute.
struct DistillConfig {
    /// Install the threshold decider + distiller at startup.  When false the
    /// dispatch envelope stays pure pass-through (byte-identical to S7).
    bool        enabled = false;                  ///< BATBOX_DISTILL_ENABLED

    /// Local OpenAI-compatible endpoint, e.g. "http://127.0.0.1:11434/v1".
    /// Required when enabled.
    std::string base_url;                         ///< BATBOX_DISTILL_BASE_URL

    /// API key for the local endpoint (usually empty or "ollama" locally).
    std::string api_key;                          ///< BATBOX_DISTILL_API_KEY

    /// The small local model that reads the big output.  Required when enabled.
    std::string model;                            ///< BATBOX_DISTILL_MODEL

    /// Engulf threshold in bytes.  Default 200k matches goose's
    /// GOOSE_MAX_TOOL_RESPONSE_SIZE (200k chars) — the same "too big to inline"
    /// ballpark, ported as a byte count.
    std::size_t max_tool_response_size = 200'000; ///< BATBOX_MAX_TOOL_RESPONSE_SIZE

    /// Per-distill request timeout.  Bounds a hung local endpoint so distill()
    /// never blocks the parent turn forever.
    int         request_timeout_sec = 60;         ///< BATBOX_DISTILL_TIMEOUT_SEC

    /// Cap on the distilled golden line length (the local model's max_tokens).
    int         max_tokens = 512;                 ///< BATBOX_DISTILL_MAX_TOKENS
};

// ============================================================================
// Top-level Config aggregate
// ============================================================================

/// Top-level runtime configuration for batbox.
///
/// Instantiate via:
///   auto cfg = Config::load_default();
///   auto cfg = Config::load_from_env(env_map);      // env only
///   auto cfg = Config::load(env_map, settings_json); // full merge
struct Config {
    ApiConfig      api;
    GeneralConfig  general;
    UiConfig       ui;
    SearchConfig   search;
    SidecarConfig  sidecar;
    ToolsConfig    tools;
    McpConfig      mcp;
    AgentsConfig   agents;
    SecurityConfig security;
    CompactConfig  compact;
    DistillConfig  distill;

    /// Incremented on every reload so components can detect a change cheaply.
    /// Not serialised to/from JSON — purely an in-process epoch counter.
    std::atomic<uint64_t> version_seq{0};

    // -------------------------------------------------------------------------
    // Constructors / assignment
    // Explicitly provided because std::atomic is not copyable by default.
    // -------------------------------------------------------------------------
    Config()                           = default;
    Config(const Config& o);
    Config(Config&& o) noexcept;
    Config& operator=(const Config& o);
    Config& operator=(Config&& o) noexcept;

    // -------------------------------------------------------------------------
    // Factory methods
    // -------------------------------------------------------------------------

    /// Return a Config populated entirely with built-in defaults.
    /// Never fails — no I/O.
    [[nodiscard]] static Config load_default();

    /// Parse every BATBOX_* key found in 'env' and overlay it on defaults.
    /// Missing keys retain their defaults. Invalid typed values (e.g. non-integer
    /// where int is expected) return Err with the offending key name.
    /// Does NOT read settings.json.
    [[nodiscard]] static Result<Config, std::string>
    load_from_env(const EnvMap& env);

    /// Full merge: env > settings_json > defaults.
    ///
    /// 'settings_json' should be the parsed ~/.batbox/settings.json content.
    /// If the JSON is null/empty the settings layer is skipped silently.
    /// After merging, validate() is called; any validation error is returned.
    [[nodiscard]] static Result<Config, std::string>
    load(const EnvMap& env, const Json& settings_json);

    // -------------------------------------------------------------------------
    // Validation
    // -------------------------------------------------------------------------

    /// Run all field-level validation rules.
    /// Returns Err("<BATBOX_VAR_NAME>: <reason>") on first failure.
    [[nodiscard]] Result<void, std::string> validate() const;

    // -------------------------------------------------------------------------
    // JSON serialisation
    // -------------------------------------------------------------------------

    /// Serialise the full config to a Json object (for /config show).
    /// All fields are included including secrets.
    [[nodiscard]] Json to_json() const;

    /// Same as to_json() but replaces secret fields (api_key) with "****".
    /// Safe to display to the user or write to logs.
    [[nodiscard]] Json redacted_for_display() const;

    // -------------------------------------------------------------------------
    // Enum round-trip helpers (used by load_from_env and from_json)
    // -------------------------------------------------------------------------
    static Result<LogLevel,      std::string> parse_log_level(std::string_view s);
    static Result<Theme,         std::string> parse_theme(std::string_view s);
    static Result<StatusLine,    std::string> parse_statusline(std::string_view s);
    static Result<SearchEngine,  std::string> parse_search_engine(std::string_view s);
    static Result<PermissionMode,std::string> parse_permission_mode(std::string_view s);

    static std::string to_string(LogLevel v);
    static std::string to_string(Theme v);
    static std::string to_string(StatusLine v);
    static std::string to_string(SearchEngine v);
    static std::string to_string(PermissionMode v);
};

// ============================================================================
// Model alias resolution
// ============================================================================

/// Resolve a model alias from an agent spec to a concrete model name.
///
/// Aliases recognised (case-sensitive):
///   "opus"   → cfg.api.opus_model   (falls back to cfg.api.default_model)
///   "sonnet" → cfg.api.sonnet_model (falls back to cfg.api.default_model)
///   "haiku"  → cfg.api.haiku_model  (falls back to cfg.api.default_model)
///
/// Any other value (including empty string) is returned verbatim so that
/// fully-qualified model names (e.g. "llama3.2:3b-cloud") pass through unchanged.
///
/// @param name  Raw model string from the agent spec frontmatter.
/// @param cfg   Loaded runtime config (already populated with alias fields).
/// @returns     Concrete model name suitable for passing to the inference client.
[[nodiscard]] std::string resolve_model_alias(std::string_view name, const Config& cfg);

} // namespace batbox::config
