# =============================================================================
# Component C10 — Config Loader (batbox_config)
# Loads .env, settings.json, agents.json, mcp.json, keybindings.json.
# Env var resolution, hot reload via /config reload.
# Depends on: batbox_core
# =============================================================================

add_library(batbox_config OBJECT)

set_target_properties(batbox_config PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

target_include_directories(batbox_config
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(batbox_config
    PUBLIC batbox_core
)

# ---------------------------------------------------------------------------
# Sources — uncomment as each CPP 10.x task lands:
#   Config.cpp            (CPP 10.2) — top-level Config struct + load_from_env
#   EnvLoader.cpp         (CPP 10.1) — .env file parsing, env var resolution
#   SettingsLoader.cpp    (CPP 10.3) — settings.json r/w
#   AgentsConfig.cpp      (CPP 10.4) — agents.json
#   McpConfig.cpp         (CPP 10.5) — mcp.json server registry config
#   KeybindingsConfig.cpp (CPP 10.6) — keybindings.json overlay on defaults
#   ConfigReload.cpp      (CPP 10.7) — hot-reload watcher, /config reload handler
# ---------------------------------------------------------------------------
target_sources(batbox_config
    PRIVATE
        .placeholder.cpp
        Config.cpp
        EnvLoader.cpp
        SettingsLoader.cpp
        AgentsConfig.cpp
        McpConfig.cpp
        KeybindingsConfig.cpp
        ConfigReload.cpp
)
