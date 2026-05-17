# =============================================================================
# Component C7 — Sidecar Manager (batbox_sidecar)
# Spawn / healthcheck / restart / shutdown of Python Scrapling subprocess.
# WebFetch/WebSearch HTTP client to sidecar FastAPI server.
# Depends on: batbox_core, batbox_config
# =============================================================================

add_library(batbox_sidecar OBJECT)

set_target_properties(batbox_sidecar PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

target_include_directories(batbox_sidecar
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(batbox_sidecar
    PUBLIC batbox_core batbox_config cpr::cpr
)

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
target_sources(batbox_sidecar
    PRIVATE
        .placeholder.cpp
        SidecarManager.cpp   # CPP 7.4 — posix_spawn lifecycle; health polling
        ScraplingClient.cpp  # CPP 7.2 — HTTP client to /fetch /search /select
        ScraplingProto.cpp   # CPP 7.1 — request/response structs + JSON serialisation
        SidecarState.cpp     # CPP 7.3 — atomic SidecarState enum + SidecarStateMachine
)
