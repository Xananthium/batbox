# =============================================================================
# src/CMakeLists.txt — batbox component library subdirectories
#
# Add-subdirectory order strictly follows component dependency graph from
# ned-cpp.md §4. Each target below is an OBJECT library (batbox_<name>)
# populated by its per-component task series (CPP <N>.x).
#
# Do NOT reorder without updating the dependency graph — each component's
# target_link_libraries() assumes its dependencies are already defined.
# =============================================================================

# ---------------------------------------------------------------------------
# Layer 0 — shared utilities (no batbox deps)
# ---------------------------------------------------------------------------
add_subdirectory(core)          # C0  Result/Json/Logging/Paths/CancelToken/Uuid
add_subdirectory(perf)          # TUI-FLOW-T3  PerfStore singleton (first_token, paint, frame latency)

# ---------------------------------------------------------------------------
# Layer 1 — style + configuration (depend only on core)
# ---------------------------------------------------------------------------
add_subdirectory(theme)         # Theme    colour/style tokens, palette definitions
add_subdirectory(config)        # C10  .env / settings.json / agents.json / mcp.json

# ---------------------------------------------------------------------------
# Layer 2 — stateful services (depend on core + config)
# ---------------------------------------------------------------------------
add_subdirectory(session)       # C9   JSON-per-session, gzip, resume, recovery
add_subdirectory(permissions)   # C12  4-mode gate, pattern matcher, persistence

# ---------------------------------------------------------------------------
# Layer 3 — network / IPC (depend on core + config)
# ---------------------------------------------------------------------------
add_subdirectory(inference)     # C4   HTTP+SSE Claude API client
add_subdirectory(sidecar)       # C7   Python Scrapling subprocess IPC
add_subdirectory(mcp)           # C8   MCP 4-transport client, JSON-RPC 2.0

# ---------------------------------------------------------------------------
# Layer 4 — tool + agent engines (depend on permissions, mcp, inference)
# ---------------------------------------------------------------------------
add_subdirectory(tools)         # C5   39 tool implementations (includes tools/bash/)
add_subdirectory(agents)        # C6   sub-agent supervisor, thread pool, /demon

# ---------------------------------------------------------------------------
# Layer 5 — conversation brain (depends on inference + tools + agents)
# ---------------------------------------------------------------------------
add_subdirectory(conversation)  # C3   message history, context window, plan-mode

# ---------------------------------------------------------------------------
# Layer 6 — skill + command + plugin layers
# ---------------------------------------------------------------------------
add_subdirectory(skills)        # skills  13 bundled skill runners + embedded .md
add_subdirectory(commands)      # cmds    38 slash command implementations
add_subdirectory(plugins)       # C11  plugin/skill/agent/command discovery

# ---------------------------------------------------------------------------
# Layer 7 — input loop (depends on conversation + commands + plugins)
# ---------------------------------------------------------------------------
add_subdirectory(repl)          # C2   REPL loop, history, autocomplete, vim-mode

# ---------------------------------------------------------------------------
# Layer 8 — TUI (depends on everything above, drawn last)
# ---------------------------------------------------------------------------
add_subdirectory(tui)           # C1   FTXUI widgets, streaming render, modals

# ---------------------------------------------------------------------------
# Layer 9 — batbox executable (CPP B.11)
#
# Links all OBJECT libraries into the final binary.
# App.cpp + main.cpp + setup_sidecar.cpp + migrate.cpp are direct sources
# (not in any component OBJECT lib) because they are the composition root.
#
# The CPP A wiring task will extend target_link_libraries when the full
# component tree is functional.  For now we link batbox_core and the
# dependencies aggregate target so the scaffold builds cleanly.
# ---------------------------------------------------------------------------
add_executable(batbox
    main.cpp
    App.cpp
    AppShutdown.cpp
    app/McpStatusPoller.cpp
    app/WireTools.cpp
    app/WireTui.cpp
    app/WireCommands.cpp
    setup_sidecar.cpp
    migrate.cpp
)

target_include_directories(batbox
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}             # src/ — for App.hpp, setup_sidecar.hpp, migrate.hpp
        ${PROJECT_SOURCE_DIR}/include           # include/batbox/...
)

target_link_libraries(batbox
    PRIVATE
        # Aggregate dep target from cmake/Dependencies.cmake (CLI11, spdlog, fmt, etc.)
        batbox::deps
        # Core utilities (Logging, Paths, Result, etc.)
        batbox_core
        batbox_perf
        # Config loader (.env parsing)
        batbox_config
        # Permission mode (PermissionMode enum, to_string, etc.)
        batbox_permissions
        # Sidecar lifecycle manager — required for prewarm_async in App.cpp (CPP 7.5)
        batbox_sidecar
        # MCP client — required by App.cpp (McpClient, transports)
        batbox_mcp
        # Inference client — required by Conversation/SseParser
        batbox_inference
        # Session store — required by App.cpp run_headless (CPP A.1)
        batbox_session
        # Conversation engine — required by App.cpp run_headless (CPP A.1)
        batbox_conversation
        # Agent engine — AgentSupervisor, SubAgent, Team, Workflow, TeamRegistry
        batbox_agents
        # Tool engine — ITool, ToolRegistry, all 39 tool implementations
        batbox_tools
        # batbox_tools_bash is an OBJECT library; must be listed explicitly so
        # the linker sees its object files (OBJECT libs do not propagate objects
        # through PUBLIC target_link_libraries of another OBJECT library)
        batbox_tools_bash
        # REPL input loop — Repl, history, autocomplete, vim-mode
        batbox_repl
        # Slash command implementations — SlashCommandRegistry, 38 commands
        batbox_commands
        # TUI layer — DemonPanel, Splash, InputBar, DiffCard, ModalPicker, etc.
        batbox_tui
        # Plugin/skill/agent/command discovery — PluginRegistry, SkillLoader
        batbox_plugins
        # Theme system — colour/style tokens, palette definitions
        batbox_theme
        # Bundled skills registry — 13 bundled skill runners + embedded .md
        batbox_skills
)
