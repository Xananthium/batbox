# =============================================================================
# Component C12 — Permission Gate (batbox_permissions)
# Four modes: default / plan / acceptEdits / nuclear.
# Pattern matcher, ask/allow/deny decisions, persistence to settings.json.
# Depends on: batbox_core, batbox_config
# =============================================================================

add_library(batbox_permissions OBJECT)

set_target_properties(batbox_permissions PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

target_include_directories(batbox_permissions
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(batbox_permissions
    PUBLIC batbox_core batbox_config
)

# ---------------------------------------------------------------------------
# Sources — uncomment as each CPP 12.x task lands:
#   PermissionGate.cpp  (CPP 12.4) — main gate; ask/allow/deny dispatch
#   PermissionMode.cpp  (CPP 12.1) — 4-mode state + transitions
#   PermissionRule.cpp  (CPP 12.3) — rule struct, serialise/deserialise
#   PatternMatcher.cpp  (CPP 12.2) — "Bash(npm test:*)" / "Read(./src/**)" glob
#   PermissionStore.cpp (CPP 12.3) — persist allow-list to settings.json
# ---------------------------------------------------------------------------
target_sources(batbox_permissions
    PRIVATE
        .placeholder.cpp
        PermissionGate.cpp
        PermissionMode.cpp
        PermissionRule.cpp
        PatternMatcher.cpp
        PermissionStore.cpp
)
