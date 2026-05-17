# =============================================================================
# Component C1 — TUI Layer (batbox_tui)
# FTXUI screens, splash, chat scrollback, streaming token render,
# modal cards (perm/diff/picker), sub-agent panel, /demon panel, theme application.
# Depends on: batbox_core, batbox_config, batbox_theme, batbox_conversation,
#             batbox_agents, batbox_permissions, batbox_repl
# =============================================================================

add_library(batbox_tui OBJECT)

set_target_properties(batbox_tui PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

target_include_directories(batbox_tui
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
    # Generated themes header dir — added once batbox_generate_themes_header() fires (CPP B.9):
    # PRIVATE ${CMAKE_BINARY_DIR}/generated/include
)

target_link_libraries(batbox_tui
    PUBLIC
        batbox_core
        batbox_perf
        batbox_config
        batbox_theme
        batbox_conversation
        batbox_agents
        batbox_permissions
        batbox_repl
    # ftxui::screen ftxui::dom ftxui::component added by cmake/Dependencies.cmake (CPP B.5)
    # tree-sitter vendor targets added when BATBOX_SYNTAX=ON (CPP B.2)
)

# ---------------------------------------------------------------------------
# Sources — uncomment as each CPP 1.x task lands:
#   Screen.cpp               (CPP 1.6)  — ScreenManager, FTXUI ScreenInteractive
#   ChatView.cpp             (CPP 1.2)  — append-only scrollback Component
#   StreamingMessageView.cpp (CPP 1.3)  — streaming assistant message (shared_mutex)
#   InputBar.cpp             (CPP 1.4)  — bottom prompt + status line + slash palette
#   PermissionCard.cpp       (CPP 1.5)  — modal: tool-confirmation card
#   DiffCard.cpp             (CPP 1.6)  — modal: side-by-side coloured diff
#   SubAgentPanel.cpp        (CPP 1.7)  — right sidebar, change-driven 10Hz
#   DemonPanel.cpp           (CPP 1.8)  — 5Hz magenta easter-egg panel
#   ModalPicker.cpp          (CPP 1.9)  — generic list-pick modal
#   Splash.cpp               (CPP 1.10) — boot splash: bat ASCII + rotating tagline
#   MarkdownRender.cpp       (CPP 1.11) — incremental markdown -> FTXUI Element
#   SyntaxHighlight.cpp      (CPP 1.4)  — tree-sitter code block coloring
#   ThemeApply.cpp           (CPP 1.13) — Theme <-> ftxui::Color mapping
#   Events.cpp               (CPP 1.14) — custom ftxui::Event subtypes
# ---------------------------------------------------------------------------
target_sources(batbox_tui
    PRIVATE
        .placeholder.cpp
        Screen.cpp      # CPP 1.6 — ScreenManager
        ThemeApply.cpp  # CPP 1.1 — Theme <-> ftxui::Color mapping
        SyntaxHighlight.cpp  # CPP 1.4 — tree-sitter + manual fallback
        ChatView.cpp  # CPP 1.7 — append-only scrollback Component
        StreamingMessageView.cpp  # CPP 1.8 — in-flight assistant message (shared_mutex + incremental markdown)
        InputBar.cpp
        PermissionCard.cpp  # CPP 1.10 — modal tool-confirmation card [a/A/n/N/e]
        PlanApprovalCard.cpp  # TUI-PLAN-T2 — plan approval modal [A/R/E]
        QuestionCard.cpp  # TUI-ASKQ-T2 — AskUserQuestion modal (renderer)
        DiffCard.cpp
        SubAgentPanel.cpp  # CPP 1.13 — right-side agent panel, change-driven 10Hz
        DemonPanel.cpp  # CPP 1.14 — 5Hz magenta easter-egg panel
        ModalPicker.cpp
        Changelog.cpp  # TUI-FLOW-T10 — changelog parser + disk loader
        Splash.cpp
        MarkdownRender.cpp  # CPP 1.3 — incremental markdown -> FTXUI Element
        Events.cpp
        PermissionBanner.cpp  # CPP 1.16 — permission mode top banner + Nuclear confirmation
        AccountLabel.cpp     # TUI-FIX-T8 — resolve_account_label: $USER@hostname fallback
        ${PROJECT_SOURCE_DIR}/src/util/EditorLaunch.cpp  # TUI-FIX-T7 — resolve_editor + edit_string_in_editor
)

# ---------------------------------------------------------------------------
# SyntaxHighlight: tree-sitter (ON) or manual lexers (OFF)
# ---------------------------------------------------------------------------
if(BATBOX_SYNTAX)
    # Tree-sitter backend — link vendored grammar library and define the macro.
    target_link_libraries(batbox_tui PRIVATE batbox_treesitter)
    target_compile_definitions(batbox_tui PRIVATE BATBOX_SYNTAX=1)
else()
    # Manual lexer fallback — compile the 5 hand-rolled lexers directly into
    # the batbox_tui OBJECT library.
    target_sources(batbox_tui
        PRIVATE
            manual_lexers/cpp_lexer.cpp
            manual_lexers/python_lexer.cpp
            manual_lexers/js_lexer.cpp
            manual_lexers/rust_lexer.cpp
            manual_lexers/go_lexer.cpp
    )
    target_compile_definitions(batbox_tui PRIVATE BATBOX_SYNTAX=0)
endif()
