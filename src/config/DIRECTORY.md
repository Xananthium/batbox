# src/config

Configuration subsystem implementations: env loading, settings parsing, config struct assembly, hot-reload diffing, and per-subsystem config loaders.

## Files

### Config.cpp
`Config::load_default()`, `load_from_env()`, `load()`, `validate()`, `to_json()`, `redacted_for_display()`, `resolve_model_alias()` implementations; assembles the final merged Config from defaults, BATBOX_* env vars, and settings.json.

### ConfigReload.cpp
`reload_config()` implementation: reloads settings and env; diffs against current config using field-by-field comparison; fires ConfigReloadBus subscribers; returns ReloadReport.

### EnvLoader.cpp
`load_env_file()` implementation: line-by-line .env parser; handles KEY=VALUE, KEY="VALUE" (double-quoted), and # comments; returns EnvMap. `merge_with_process_env()` and `get()` implementations.

### AgentsConfig.cpp
`load_agents_config()` implementation: reads agents.json; parses agent-name→model-string map.

### KeybindingsConfig.cpp
`default_keybindings()` and `load_keybindings()` implementations: built-in action→descriptor table; merges user overrides; warns on unknown action names.

### McpConfig.cpp
`load_mcp_config()` and `load_mcp_configs()` implementations: reads mcp.json; dispatches to StdioConfig/HttpConfig/SseConfig/WsConfig parsers based on "type" field.

### SettingsLoader.cpp
`load_settings()` and `write_settings()` implementations: reads/writes settings.json; atomic write via tmp+rename; extracts permissions, theme, plugins.disabled, output_style.

### Config.cpp — DistillConfig (S1+S4, DIS-980)
`Config.cpp` gained the `distill` group: env parse (`BATBOX_DISTILL_*` + `BATBOX_MAX_TOOL_RESPONSE_SIZE`), settings.json merge, `to_json()`/`redacted_for_display()` (distill `api_key` masked), and `validate()` rules (enabled requires URL-shaped base_url + model; thresholds/timeouts > 0). The explicit copy/move ctors + assignments were updated to carry the new `distill` member (else env-parsed distill settings drop on the value-return).

### CMakeLists.txt
Build rules for the config static library.
