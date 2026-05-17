# =============================================================================
# Component Theme — Colour/Style Tokens (batbox_theme)
# Task: CPP B.9 generates Themes.hpp at configure time via cmake/Theme.cmake.
# Per-component implementation tasks (CPP 1.x theme) fill in the source list.
# Depends on: batbox_core
# =============================================================================

add_library(batbox_theme OBJECT)

set_target_properties(batbox_theme PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

target_include_directories(batbox_theme
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
    # Generated header dir added by cmake/Theme.cmake once batbox_generate_themes_header() is called:
    # PRIVATE ${CMAKE_BINARY_DIR}/generated/include
)

target_link_libraries(batbox_theme
    PUBLIC batbox_core
)

# ---------------------------------------------------------------------------
# Sources — CPP 1.1 landed: Theme.cpp + themes.cpp
# ---------------------------------------------------------------------------
target_sources(batbox_theme
    PRIVATE
        Theme.cpp
        themes.cpp
)
