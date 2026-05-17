# =============================================================================
# Component C2 — REPL / Input (batbox_repl)
# Input loop, history, autocomplete, vim mode, !/#/@// prefix handling,
# Ctrl+R search, keybindings.
# Depends on: batbox_core, batbox_config, batbox_conversation,
#             batbox_commands, batbox_skills, batbox_plugins
# =============================================================================

add_library(batbox_repl OBJECT)

set_target_properties(batbox_repl PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

target_include_directories(batbox_repl
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(batbox_repl
    PUBLIC
        batbox_core
        batbox_config
        batbox_conversation
        batbox_commands
        batbox_skills
        batbox_plugins
)

# ---------------------------------------------------------------------------
# Sources — uncomment as each CPP 2.x task lands:
#   Repl.cpp          (CPP 2.1) — main input loop; bridges InputBar to Conversation
#   History.cpp       (CPP 2.2) — in-memory deque + ~/.batbox/history persistence
#   Autocomplete.cpp  (CPP 2.3) — file paths, slash commands, @-mentions
#   VimMode.cpp       (CPP 2.4) — vim-mode state machine (Normal/Insert/Visual)
#   HistorySearch.cpp (CPP 2.5) — Ctrl+R fzy search over history deque
#   Keybindings.cpp   (CPP 2.6) — default + override resolver
# ---------------------------------------------------------------------------
target_sources(batbox_repl
    PRIVATE
        .placeholder.cpp
        Repl.cpp
        History.cpp
        Autocomplete.cpp
        VimMode.cpp
        HistorySearch.cpp
        Keybindings.cpp
)
