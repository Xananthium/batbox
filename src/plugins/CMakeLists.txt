# =============================================================================
# Component C11 — Plugin / Skill Loader (batbox_plugins)
# Discover plugins under 4 roots, parse marketplace.json,
# YAML-frontmatter markdown for skills/agents/commands, /plugin operations.
# Depends on: batbox_core, batbox_config, batbox_skills, batbox_agents
# =============================================================================

add_library(batbox_plugins OBJECT)

set_target_properties(batbox_plugins PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

target_include_directories(batbox_plugins
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(batbox_plugins
    PUBLIC batbox_core batbox_config batbox_skills batbox_agents
)

# ---------------------------------------------------------------------------
# Sources — uncomment as each CPP 11.x task lands:
#   PluginLoader.cpp      (CPP 11.1) — discover + load plugins from 4 roots
#   MarketplaceJson.cpp   (CPP 11.2) — parse marketplace.json
#   SkillLoader.cpp       (CPP 11.4) — skills from user dirs + plugins
#   AgentLoader.cpp       (CPP 11.5) — plugin-bundled agent loader (calls agents/)
#   CommandLoader.cpp     (CPP 11.6) — user-defined slash commands from plugins
#   FrontmatterParser.cpp (CPP 11.1) — YAML frontmatter markdown parser
#   PluginRegistry.cpp    (CPP 11.3) — runtime registry of loaded plugins
# ---------------------------------------------------------------------------
target_sources(batbox_plugins
    PRIVATE
        .placeholder.cpp
        PluginLoader.cpp
        MarketplaceJson.cpp
        SkillLoader.cpp
        AgentLoader.cpp
        CommandLoader.cpp
        FrontmatterParser.cpp
        PluginRegistry.cpp
)
