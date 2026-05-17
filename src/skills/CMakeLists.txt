# =============================================================================
# Component Skills — Bundled Skill Registry (batbox_skills)
# 13 bundled skills as embedded markdown resources + thin Skill* runners.
# skills/bundled/ directory (project root) contains .md skill files embedded
# via CMake embed.cmake at configure time.
# Depends on: batbox_core, batbox_config
# =============================================================================

# ---------------------------------------------------------------------------
# Embed step: generate ${CMAKE_BINARY_DIR}/generated/include/batbox/skills/
#             skills_embed.inc from skills/bundled/*.md
# Must run before add_library() sources are declared so the .inc is present
# when BundledSkillsRegistry.cpp is compiled.
# ---------------------------------------------------------------------------
include(${CMAKE_CURRENT_LIST_DIR}/embed.cmake)
batbox_generate_skills_embed("${CMAKE_BINARY_DIR}/generated/include")

add_library(batbox_skills OBJECT)

set_target_properties(batbox_skills PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

target_include_directories(batbox_skills
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
    PRIVATE
        # Generated embed header (skills_embed.inc)
        ${CMAKE_BINARY_DIR}/generated/include
)

target_link_libraries(batbox_skills
    PUBLIC batbox_core batbox_config
)

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
target_sources(batbox_skills
    PRIVATE
        BundledSkillsRegistry.cpp
)
