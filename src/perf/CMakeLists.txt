# =============================================================================
# Component perf — Performance measurement store (TUI-FLOW-T3)
# Thread-safe PerfStore singleton that records first_token_ms,
# stream_to_paint_ms, and frame_ms for the live perf HUD.
# Depends on: nothing (pure C++20 atomics).
# =============================================================================

add_library(batbox_perf STATIC)

set_target_properties(batbox_perf PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

target_include_directories(batbox_perf
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_sources(batbox_perf
    PRIVATE
        PerfSnapshot.cpp
)
