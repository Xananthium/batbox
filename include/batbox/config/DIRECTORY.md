# include/batbox/config

Configuration subsystem headers: the main Config struct, hot-reload bus, env file loader, agents/keybindings/MCP/settings loaders, and persistent state store.

## Files

### Config.hpp
Root configuration struct and loader.

- `Config::load_default() -> Config` — constructs Config with hardcoded defaults; no env or file reads
- `Config::load_from_env(env) -> Config` — overlays BATBOX_* env vars onto defaults; returns merged Config
- `Config::load(env, settings_json) -> Config` — full load: defaults + env vars + settings.json fields merged in priority order
- `Config::validate() -> vector<string>` — checks field invariants (e.g. timeout > 0); returns list of error strings
- `Config::to_json() -> Json` — serialises Config to JSON object for /config dump
- `Config::redacted_for_display() -> Config` — returns copy with API keys replaced by "***"
- `resolve_model_alias(name, cfg) -> string` — expands model alias (e.g. "fast") to canonical model name using cfg.agents table

### ConfigReload.hpp
Hot-reload bus: diff detection and subscriber notification.

- `reload_config(cfg, config_dir={}) -> Result<ReloadReport, string>` — reloads settings.json and env, diffs against current cfg, fires ConfigReloadBus::fire, returns report of changed fields
- `ConfigReloadBus::instance() -> ConfigReloadBus&` — returns process-global singleton
- `ConfigReloadBus::subscribe(cb) -> shared_ptr<ReloadSubscription>` — registers a callback invoked on every successful reload; returns RAII handle that auto-unsubscribes on destruction
- `ConfigReloadBus::fire(cfg, report)` — calls all active subscriber callbacks with new config and diff report
- `ReloadReport::is_unchanged() -> bool` — returns true when changed_fields and errors are both empty
- `ReloadReport::empty() -> bool` — returns true when report carries no changes or errors

### EnvLoader.hpp
.env file parser and process environment merger.

- `load_env_file(path) -> Result<EnvMap, string>` — reads KEY=VALUE pairs from path; ignores blank lines and # comments; returns Err on file open failure
- `merge_with_process_env(map, process_env_wins=false) -> void` — merges map with the process environment; when process_env_wins=true existing vars are not overwritten
- `get(map, key, default_value="") -> string` — looks up key in map; returns default_value if absent

### AgentsConfig.hpp
Per-agent model override loader.

- `load_agents_config(path) -> Result<AgentModelMap, string>` — reads agents.json mapping agent names to model strings; returns Err on parse failure; returns empty map when file is absent

### KeybindingsConfig.hpp
Keybinding configuration loader.

- `default_keybindings() -> KeybindingMap` — returns the built-in action-to-key-descriptor map (e.g. "send" -> "Ctrl+Enter")
- `load_keybindings(path) -> Result<KeybindingMap, string>` — reads keybindings.json; merges with defaults; logs WARN on unknown action names

### McpConfig.hpp
MCP server configuration loader.

- `load_mcp_config(path) -> Result<vector<McpServerConfig>>` — reads mcp.json; constructs typed McpServerConfig entries with the correct transport variant (StdioConfig, HttpConfig, SseConfig, WsConfig)
- `load_mcp_configs() -> vector<McpServerConfig>` — loads from the default config path; returns empty vector on any error

### SettingsLoader.hpp
Persistent settings.json reader and writer.

- `load_settings(path) -> Result<Settings, string>` — reads settings.json; extracts permissions, theme, plugins.disabled, output_style; returns Err on JSON parse failure
- `write_settings(path, s) -> Result<void, string>` — serialises Settings to JSON and writes atomically via tmp+rename; returns Err on I/O failure

### StateStore.hpp
Lightweight persistent state for changelog tracking.

- `read_last_seen_changelog_version() -> optional<string>` — reads the last-acknowledged changelog version from ~/.batbox/state.json; returns nullopt when absent
- `write_last_seen_changelog_version(version) -> void` — writes version string to ~/.batbox/state.json; used by the changelog dialog to suppress already-seen entries
