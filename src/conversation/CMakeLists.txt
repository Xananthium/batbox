# =============================================================================
# Component C3 — Conversation Engine (batbox_conversation)
# Message history, context-window manager, /compact, tool-call orchestration,
# plan-mode state machine.
# Depends on: batbox_core, batbox_config, batbox_inference, batbox_tools,
#             batbox_permissions, batbox_agents
# =============================================================================

add_library(batbox_conversation STATIC)

set_target_properties(batbox_conversation PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

target_include_directories(batbox_conversation
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(batbox_conversation
    PUBLIC
        batbox_core
        batbox_perf
        batbox_config
        batbox_inference
        batbox_tools
        batbox_permissions
        batbox_agents
)

# ---------------------------------------------------------------------------
# Sources — uncomment as each CPP 3.x task lands:
#   Conversation.cpp         (CPP 3.1) — owns messages_, drives one turn
#   Message.cpp              (CPP 3.2) — Message / ToolCall / UsageDelta structs
#   ContextWindow.cpp        (CPP 3.x) — token counting, auto-compact trigger
#   ToolCallOrchestrator.cpp (CPP 3.4) — decode streamed deltas, dispatch tools
#   PlanMode.cpp             (CPP 3.3) — /plan state machine (read-only until approve)
#   Compactor.cpp            (CPP 3.x) — summarise-by-LLM + verbatim tail
# ---------------------------------------------------------------------------
target_sources(batbox_conversation
    PRIVATE
        .placeholder.cpp
        Conversation.cpp   # CPP 3.6 — owns messages_, drives one turn
        SystemPrompt.cpp     # CPP 3.8 — system prompt composition (BATBOX.md merge)
        Message.cpp          # CPP 3.1 — Message / ToolCall / UsageDelta structs
        ContextWindow.cpp        # CPP 3.2 — token counting + auto-compact trigger
        ToolCallOrchestrator.cpp
        PlanMode.cpp          # CPP 3.3 — plan-mode state machine
        Compactor.cpp         # CPP 3.5 — summarise-by-LLM + verbatim tail
        NotepadReminder.cpp   # DIS-981 S6 — per-turn notepad tail re-injection
)
