# =============================================================================
# Component C5 sub-library — BashRunner backends (batbox_tools_bash)
# forkpty + posix_spawn execution backends for BashTool.
# This is an internal subdir of batbox_tools; sources are included via
# $<TARGET_OBJECTS:batbox_tools_bash> in src/tools/CMakeLists.txt.
# Depends on: batbox_core, batbox_permissions
# =============================================================================

add_library(batbox_tools_bash OBJECT)

set_target_properties(batbox_tools_bash PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

target_include_directories(batbox_tools_bash
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}   # for *Internal.hpp headers
)

target_link_libraries(batbox_tools_bash
    PUBLIC batbox_core batbox_permissions
)

# macOS: forkpty is in libutil; Linux: it is in libutil or libpty
# We guard with generator expression so the same CMakeLists works on both.
if(APPLE)
    target_link_libraries(batbox_tools_bash PUBLIC "-framework CoreFoundation")
    # forkpty is in the default libc on macOS (no extra -lutil needed)
elseif(UNIX)
    target_link_libraries(batbox_tools_bash PUBLIC util)
endif()

# ---------------------------------------------------------------------------
# CPP 5.8 — BashRunner (forkpty backend)
# CPP 5.9 — PipesBackend (pipe-pair fallback backend)
# ---------------------------------------------------------------------------
target_sources(batbox_tools_bash
    PRIVATE
        BashRunner.cpp
        PtyBackend.cpp
        PipesBackend.cpp
        EnvScrub.cpp
        AnsiStrip.cpp
)
