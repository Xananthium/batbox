# =============================================================================
# Component C9 — Session Store (batbox_session)
# JSON-per-session + append-only index.json, gzip on >1 MB, resume, recovery.
# Depends on: batbox_core, batbox_config, zlib
# =============================================================================

add_library(batbox_session STATIC)

set_target_properties(batbox_session PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

target_include_directories(batbox_session
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

# zlib is a vcpkg dependency (available in vcpkg_installed)
find_package(ZLIB REQUIRED)

target_link_libraries(batbox_session
    PUBLIC batbox_core batbox_config ZLIB::ZLIB
)

# ---------------------------------------------------------------------------
# Sources — uncomment as each CPP 9.x task lands:
#   SessionFile.cpp     (CPP 9.1) — per-session JSON file r/w + gzip
#   SessionIndex.cpp    (CPP 9.2) — append-only JSONL index management
#   SessionStore.cpp    (CPP 9.3) — create/open/close sessions (future)
#   SessionRecovery.cpp (CPP 9.4) — crash-recovery / partial-write detection (future)
# ---------------------------------------------------------------------------
target_sources(batbox_session
    PRIVATE
        SessionFile.cpp
        SessionIndex.cpp
        SessionStore.cpp       # CPP 9.3
        SessionRecovery.cpp    # CPP 9.4
        State.cpp              # TUI-FLOW-T10 — state.json read/write
)
