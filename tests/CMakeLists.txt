# =============================================================================
# tests/CMakeLists.txt — batbox test suite
#
# Enabled when BATBOX_TESTS=ON (default ON for active dev builds).
# Uses doctest (locked dep per pmdraft.md + cmake/Dependencies.cmake).
# Unit tests live in tests/unit/; perf micro-benchmarks in tests/perf/.
#
# Per-component test files are added by their respective CPP <N>.x tasks.
# This file provides the framework wiring so the first test can land without
# boilerplate duplication.
# =============================================================================

if(NOT BATBOX_TESTS)
    return()
endif()

enable_testing()

# ---------------------------------------------------------------------------
# doctest — test framework (locked per pmdraft.md, vcpkg.json, Dependencies.cmake)
# cmake/Dependencies.cmake calls find_package(doctest CONFIG REQUIRED) so the
# target doctest::doctest is available here.
# ---------------------------------------------------------------------------
if(NOT TARGET doctest::doctest)
    find_package(doctest CONFIG QUIET)
endif()

if(NOT TARGET doctest::doctest)
    message(STATUS "  doctest not found — test targets skipped (vcpkg install needed)")
    return()
endif()

include(CTest)
include(doctest)

# ---------------------------------------------------------------------------
# Helper macro: batbox_add_unit_test(<name> <source> [link_libs...])
# Creates a test executable, registers it with CTest, and links doctest + the
# supplied component libraries.  Per-component tasks call this macro.
# Each test source is expected to define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
# in exactly one TU (typical doctest pattern — the test file itself).
# ---------------------------------------------------------------------------
macro(batbox_add_unit_test _name _source)
    add_executable(${_name} ${_source})
    set_target_properties(${_name} PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )
    target_include_directories(${_name}
        PRIVATE ${PROJECT_SOURCE_DIR}/include
    )
    target_link_libraries(${_name}
        PRIVATE doctest::doctest ${ARGN}
    )
    doctest_discover_tests(${_name})
endmacro()

# ---------------------------------------------------------------------------
# Unit tests — added as each CPP N.x task lands its test file:
# ---------------------------------------------------------------------------

# CPP 0.5 — CancelToken
batbox_add_unit_test(test_cancel_token
    unit/test_cancel_token.cpp  batbox_core)

# CPP 0.x — CancelToken (legacy name kept for compatibility)
batbox_add_unit_test(test_cancel
    unit/test_cancel.cpp        batbox_core)

# CPP 0.x — Paths
batbox_add_unit_test(test_paths
    unit/test_paths.cpp         batbox_core)

# CPP 0.x — Result
batbox_add_unit_test(test_result
    unit/test_result.cpp        batbox_core)

# CPP 0.3 — Logging
batbox_add_unit_test(test_logging
    unit/test_logging.cpp       batbox_core)

# CPP 0.x — Uuid
batbox_add_unit_test(test_uuid
    unit/test_uuid.cpp          batbox_core)

# CPP 2.4 — VimMode state machine (Normal/Insert/Visual, hjkl, dd/yy/p, w/b/e, text objects)
# Compile VimMode.cpp directly so we don't depend on batbox_repl being fully linked.
batbox_add_unit_test(test_vim_mode
    unit/test_vim_mode.cpp      batbox_core)
target_sources(test_vim_mode PRIVATE
    ${PROJECT_SOURCE_DIR}/src/repl/VimMode.cpp)

# CPP 3.3 — PlanMode state machine
# Compile PlanMode.cpp directly (same pattern as test_vim_mode / test_message_model)
# so we avoid linking batbox_conversation, which transitively requires
# ToolCallOrchestrator, SessionStore, Client and other in-flight symbols.
# Uuid.cpp and CancelToken.cpp come from batbox_core.
# simdjson is needed because Json.hpp is included transitively.
batbox_add_unit_test(test_plan_mode_state
    unit/test_plan_mode_state.cpp  batbox_core)
target_sources(test_plan_mode_state PRIVATE
    ${PROJECT_SOURCE_DIR}/src/conversation/PlanMode.cpp)
target_link_libraries(test_plan_mode_state PRIVATE simdjson::simdjson)

# CPP 3.1 — Message canonical model (Message / ToolCall / UsageDelta structs)
# Compile Message.cpp directly; link simdjson because Json.hpp pulls it in.
batbox_add_unit_test(test_message_model
    unit/test_message_model.cpp  batbox_core)
target_sources(test_message_model PRIVATE
    ${PROJECT_SOURCE_DIR}/src/conversation/Message.cpp)
target_link_libraries(test_message_model PRIVATE simdjson::simdjson)

# CPP 8.2 — MCP framing (LSP Content-Length stdio transport)
batbox_add_unit_test(test_mcp_framing
    unit/test_mcp_framing.cpp   batbox_mcp)
target_link_libraries(test_mcp_framing PRIVATE batbox_core batbox_config batbox_inference simdjson::simdjson)

# ---------------------------------------------------------------------------
# Commented-out future tests — uncomment as each CPP N.x task lands:
#
#   batbox_add_unit_test(test_sse_parser
#       unit/test_sse_parser.cpp          batbox_inference)
#
#   batbox_add_unit_test(test_tool_call_accumulator
#       unit/test_tool_call_accumulator.cpp batbox_inference)
#
#   batbox_add_unit_test(test_pattern_matcher
#       unit/test_pattern_matcher.cpp     batbox_permissions)
#
#   batbox_add_unit_test(test_session_index
#       unit/test_session_index.cpp       batbox_session)
#
#   batbox_add_unit_test(test_session_recovery
#       unit/test_session_recovery.cpp    batbox_session)
#
#   batbox_add_unit_test(test_frontmatter_parser
#       unit/test_frontmatter_parser.cpp  batbox_plugins)
#
#   batbox_add_unit_test(test_plugin_loader
#       unit/test_plugin_loader.cpp       batbox_plugins)
#
#   batbox_add_unit_test(test_markdown_render
#       unit/test_markdown_render.cpp     batbox_tui)
#
#   batbox_add_unit_test(test_context_window
#       unit/test_context_window.cpp      batbox_conversation)
#
#   batbox_add_unit_test(test_config_loader
#       unit/test_config_loader.cpp       batbox_config)
# ---------------------------------------------------------------------------

# Perf benchmarks (opt-in, not part of CTest run by default):
# add_subdirectory(perf)

# CPP 7.3 — SidecarState atomic enum + transitions
batbox_add_unit_test(test_sidecar_state
    unit/test_sidecar_state.cpp  batbox_sidecar)
target_link_libraries(test_sidecar_state PRIVATE batbox_core)

# CPP 7.4 — SidecarManager: spawn / healthcheck / restart / shutdown
batbox_add_unit_test(test_sidecar_manager
    unit/test_sidecar_manager.cpp  batbox_sidecar)
target_link_libraries(test_sidecar_manager PRIVATE
    batbox_core
    cpr::cpr
    simdjson::simdjson)

# CPP S.0 — SlashCommandRegistry + ISlashCommand interface
# Compile SlashCommandRegistry.cpp directly into the test executable so we avoid
# linking batbox_commands (which has unbuilt deps at this stage).
batbox_add_unit_test(test_slash_command_registry
    unit/test_slash_command_registry.cpp  batbox_core)
target_sources(test_slash_command_registry PRIVATE
    ${PROJECT_SOURCE_DIR}/src/commands/SlashCommandRegistry.cpp)

# CPP 2.1 — History in-memory + persisted
# Compile History.cpp + Paths.cpp directly (batbox_repl not fully linked yet).
batbox_add_unit_test(test_history
    unit/test_history.cpp  batbox_core)
target_sources(test_history PRIVATE
    ${PROJECT_SOURCE_DIR}/src/repl/History.cpp)

# CPP 2.2 — HistorySearch Ctrl+R fuzzy search
# Compile HistorySearch.cpp + History.cpp + Paths.cpp directly.
batbox_add_unit_test(test_history_search
    unit/test_history_search.cpp  batbox_core)
target_sources(test_history_search PRIVATE
    ${PROJECT_SOURCE_DIR}/src/repl/HistorySearch.cpp
    ${PROJECT_SOURCE_DIR}/src/repl/History.cpp)

# CPP 1.5 — TUI Events: custom ftxui::Event subtypes
# Compile Events.cpp directly to avoid full batbox_tui link.
batbox_add_unit_test(test_tui_events
    unit/test_tui_events.cpp  batbox_core)
target_sources(test_tui_events PRIVATE
    ${PROJECT_SOURCE_DIR}/src/tui/Events.cpp)
target_link_libraries(test_tui_events PRIVATE
    ftxui::component  ftxui::dom  ftxui::screen)

# TUI-T15 — ThinkingStarted / ThinkingStopped event subtypes (thinking indicator)
# Compile Events.cpp directly (same pattern as test_tui_events above).
batbox_add_unit_test(test_tui_thinking_indicator
    unit/test_tui_thinking_indicator.cpp  batbox_core)
target_sources(test_tui_thinking_indicator PRIVATE
    ${PROJECT_SOURCE_DIR}/src/tui/Events.cpp)
target_link_libraries(test_tui_thinking_indicator PRIVATE
    ftxui::component  ftxui::dom  ftxui::screen)

# CPP 1.6 — ScreenManager: FTXUI ScreenInteractive lifecycle + threading contract
# Compile Screen.cpp and Events.cpp directly so we only pull in what we need.
batbox_add_unit_test(test_screen_manager
    unit/test_screen_manager.cpp  batbox_core)
target_sources(test_screen_manager PRIVATE
    ${PROJECT_SOURCE_DIR}/src/tui/Screen.cpp
    ${PROJECT_SOURCE_DIR}/src/tui/Events.cpp)
target_link_libraries(test_screen_manager PRIVATE
    ftxui::component  ftxui::dom  ftxui::screen)

# CPP 5.1 — ITool.hpp + ToolResult.hpp + ToolContext.hpp interface contract
# Headers-only interface test. Links simdjson for Json.hpp and
# batbox_permissions for PermissionMode.  CancelToken.cpp is provided
# via batbox_core.
batbox_add_unit_test(test_tool_interfaces
    unit/test_tool_interfaces.cpp  batbox_core)
target_link_libraries(test_tool_interfaces PRIVATE
    batbox_permissions
    batbox_config
    simdjson::simdjson)

# CPP 8.1 — JSON-RPC 2.0 envelope helpers
# Link simdjson explicitly: batbox_mcp depends on batbox_core which uses
# simdjson in implementation TUs; the OBJECT library transitive link does
# not carry simdjson::simdjson through to test executables automatically.
batbox_add_unit_test(test_json_rpc
    unit/test_json_rpc.cpp   batbox_mcp)
target_link_libraries(test_json_rpc PRIVATE batbox_core batbox_config batbox_inference simdjson::simdjson)

# CPP 4.2 — SseParser: SSE line-splitting + event reassembly
# Compile SseParser.cpp and Logging.cpp directly into the test executable so we
# don't depend on OBJECT library transitive linking (same pattern as test_vim_mode).
batbox_add_unit_test(test_sse_parser
    unit/test_sse_parser.cpp  batbox_core)
target_sources(test_sse_parser PRIVATE
    ${PROJECT_SOURCE_DIR}/src/inference/SseParser.cpp)


# CPP 4.3 — ToolCallAccumulator: per-index tool_call delta accumulation + finalize()
# Compile ToolCallAccumulator.cpp and ChatRequest.cpp directly so we avoid
# depending on OBJECT library transitive linking (same pattern as test_sse_parser).
# Link simdjson explicitly because Json.hpp / nlohmann pull it in on this path.
batbox_add_unit_test(test_tool_call_accumulator
    unit/test_tool_call_accumulator.cpp  batbox_core)
target_sources(test_tool_call_accumulator PRIVATE
    ${PROJECT_SOURCE_DIR}/src/inference/ToolCallAccumulator.cpp
    ${PROJECT_SOURCE_DIR}/src/inference/ChatRequest.cpp)
target_link_libraries(test_tool_call_accumulator PRIVATE simdjson::simdjson)


# CPP 9.1 — SessionFile: per-session JSON shape, gzip, crash recovery
# batbox_session is an OBJECT library; link batbox_core explicitly for Uuid,
# Paths, Logging symbols.  Also link simdjson for Json.hpp and ZLIB::ZLIB for gzip.
batbox_add_unit_test(test_session_file
    unit/test_session_file.cpp  batbox_session)
target_link_libraries(test_session_file PRIVATE
    batbox_core
    simdjson::simdjson
    spdlog::spdlog
    ZLIB::ZLIB)

# CPP 9.2 — SessionIndex: append-only JSONL index management
# batbox_session is an OBJECT library; link batbox_core explicitly for Uuid,
# Paths, Logging symbols.  Also link simdjson for Json.hpp.
batbox_add_unit_test(test_session_index
    unit/test_session_index.cpp  batbox_session)
target_link_libraries(test_session_index PRIVATE
    batbox_core
    simdjson::simdjson
    spdlog::spdlog)

# ---------------------------------------------------------------------------
# Integration tests (CPP A.2 and later)
# ---------------------------------------------------------------------------
add_subdirectory(integration)

# CPP S.1 — /help, /exit, /clear, /init slash commands
# Compile the four command .cpp files directly into the test executable so we
# avoid depending on batbox_commands (which requires batbox_repl + batbox_conversation,
# both in-flight). The MockContext defined in test_commands_s1.cpp satisfies
# the CommandContext forward declaration used by each command.
batbox_add_unit_test(test_commands_s1
    unit/test_commands_s1.cpp  batbox_core)
target_sources(test_commands_s1 PRIVATE
    ${PROJECT_SOURCE_DIR}/src/commands/SlashCommandRegistry.cpp
    ${PROJECT_SOURCE_DIR}/src/commands/HelpCmd.cpp
    ${PROJECT_SOURCE_DIR}/src/commands/ExitCmd.cpp
    ${PROJECT_SOURCE_DIR}/src/commands/ClearCmd.cpp
    ${PROJECT_SOURCE_DIR}/src/commands/InitCmd.cpp)
target_link_libraries(test_commands_s1 PRIVATE simdjson::simdjson)

# TUI-T7 — /resume slash command unit tests
# Tests ResumeCmd: session load by UUID prefix, /resume last, /resume cwd,
# no-sessions error, bad-UUID error, set_messages_json call verification.
# Compile ResumeCmd.cpp and SlashCommandRegistry.cpp directly (same pattern
# as test_commands_s1) to avoid pulling in all 38 commands and their
# wide transitive dependency chains.
batbox_add_unit_test(test_resume_cmd
    unit/test_resume_cmd.cpp  batbox_session batbox_core)
target_sources(test_resume_cmd PRIVATE
    ${PROJECT_SOURCE_DIR}/src/commands/SlashCommandRegistry.cpp
    ${PROJECT_SOURCE_DIR}/src/commands/ResumeCmd.cpp)
target_link_libraries(test_resume_cmd PRIVATE simdjson::simdjson)

# CPP 11.2 — MarketplaceJson parser (.claude-plugin / .batbox-plugin marketplace.json)
# Link simdjson explicitly: batbox_plugins->batbox_core uses simdjson in impl TUs.
# CommandLoader.cpp (in batbox_plugins) calls SlashCommandRegistry::register_command
# and ::lookup.  Compile SlashCommandRegistry.cpp directly to avoid pulling in all
# of batbox_commands and its wide transitive dependency chain.
batbox_add_unit_test(test_marketplace_json
    unit/test_marketplace_json.cpp  batbox_plugins batbox_core)
target_sources(test_marketplace_json PRIVATE
    ${PROJECT_SOURCE_DIR}/src/commands/SlashCommandRegistry.cpp)
target_link_libraries(test_marketplace_json PRIVATE simdjson::simdjson)

# CPP 10.2 — Config struct + load_from_env / load (env>settings_json>defaults)
# Link simdjson explicitly: batbox_config OBJECT lib uses simdjson via batbox_core
# (Json.hpp), which doesn't auto-propagate through OBJECT library chains.
batbox_add_unit_test(test_config_load
    unit/test_config_load.cpp  batbox_config batbox_core)
target_link_libraries(test_config_load PRIVATE simdjson::simdjson)

# CPP 10.4 — AgentsConfig: parse ~/.batbox/agents.json (per-agent model overrides)
# Link simdjson explicitly: batbox_config OBJECT lib uses simdjson via batbox_core
# (Json.hpp), which doesn't auto-propagate through OBJECT library chains.
batbox_add_unit_test(test_agents_config
    unit/test_agents_config.cpp  batbox_config batbox_core)
target_link_libraries(test_agents_config PRIVATE simdjson::simdjson)

# CPP 5.24 — CtxInspectTool: read-only context introspection tool
# Compile CtxInspectTool.cpp directly. Link batbox_permissions for
# to_string(PermissionMode) and simdjson for Json.hpp.  CancelToken.cpp
# is provided via batbox_core.
batbox_add_unit_test(test_ctx_inspect
    unit/test_ctx_inspect.cpp  batbox_core)
target_sources(test_ctx_inspect PRIVATE
    ${PROJECT_SOURCE_DIR}/src/tools/CtxInspectTool.cpp)
target_link_libraries(test_ctx_inspect PRIVATE
    batbox_permissions
    batbox_config
    simdjson::simdjson)

# CPP 5.3 — ReadTool: read file with cat -n line numbers, offset/limit, 5MB cap
# Compile ReadTool.cpp directly into the test executable.
# Link batbox_permissions for PermissionMode and simdjson for Json.hpp.
# CancelToken.cpp is provided via batbox_core.
batbox_add_unit_test(test_read_tool
    unit/test_read_tool.cpp  batbox_core)
target_sources(test_read_tool PRIVATE
    ${PROJECT_SOURCE_DIR}/src/tools/ReadTool.cpp)
target_link_libraries(test_read_tool PRIVATE
    batbox_permissions
    batbox_config
    simdjson::simdjson)

# CPP 4.4 — UsageTracker + ModelPricing: per-session/turn token accumulation + cost
# Compile UsageTracker.cpp and ModelPricing.cpp directly into the test executable
# so we avoid depending on OBJECT library transitive linking (same pattern as
# test_sse_parser and test_tool_call_accumulator).
# Link simdjson explicitly because ModelPricing.cpp uses Json.hpp / nlohmann parse.
batbox_add_unit_test(test_usage_tracker
    unit/test_usage_tracker.cpp  batbox_core)
target_sources(test_usage_tracker PRIVATE
    ${PROJECT_SOURCE_DIR}/src/inference/UsageTracker.cpp
    ${PROJECT_SOURCE_DIR}/src/inference/ModelPricing.cpp)
target_link_libraries(test_usage_tracker PRIVATE simdjson::simdjson)

# CPP 5.15 — TodoWriteTool: per-session todo list management
# Compile TodoWriteTool.cpp directly to avoid full batbox_tools link (not all
# tools are built yet). Link simdjson for Json.hpp and batbox_permissions for
# PermissionMode.  CancelToken.cpp is provided via batbox_core.
batbox_add_unit_test(test_todo_write
    unit/test_todo_write.cpp  batbox_core)
target_sources(test_todo_write PRIVATE
    ${PROJECT_SOURCE_DIR}/src/tools/TodoWriteTool.cpp)
target_link_libraries(test_todo_write PRIVATE
    batbox_permissions
    batbox_config
    simdjson::simdjson)

# CPP 5.2 — ToolRegistry: register_tool, find_by_name, available_tool_schemas, dispatch
# Link batbox_tools (which includes ToolRegistry.cpp) and its dependencies.
# CancelToken.cpp is provided via batbox_core.
# batbox_mcp, batbox_inference, and batbox_tools_bash are OBJECT libs whose objects
# don't propagate transitively through batbox_tools into the test executable —
# add them explicitly so McpServerRegistry, SseParser, and BashRunner symbols resolve.
batbox_add_unit_test(test_tool_registry
    unit/test_tool_registry.cpp  batbox_tools batbox_core)
target_link_libraries(test_tool_registry PRIVATE
    batbox_mcp
    batbox_inference
    batbox_tools_bash
    batbox_permissions
    batbox_config
    simdjson::simdjson)


# CPP 7.2 — ScraplingClient: cpr POST helpers to sidecar (/fetch /search /select)
# Link cpr::cpr, simdjson, and batbox_core for remaining symbols.
# CancelToken.cpp and Json.cpp are provided via batbox_core.
batbox_add_unit_test(test_scrapling_client
    unit/test_scrapling_client.cpp  batbox_sidecar)
target_link_libraries(test_scrapling_client PRIVATE
    batbox_core
    cpr::cpr
    simdjson::simdjson)

# CPP 5.25 — SnipTool: named snippet save/load/list/delete to ~/.batbox/snippets/
# Compile SnipTool.cpp directly to avoid full batbox_tools link (not all tools
# are built yet). Link batbox_permissions for PermissionMode and simdjson for
# Json.hpp.  CancelToken.cpp is provided via batbox_core.
batbox_add_unit_test(test_snip_tool
    unit/test_snip_tool.cpp  batbox_core)
target_sources(test_snip_tool PRIVATE
    ${PROJECT_SOURCE_DIR}/src/tools/SnipTool.cpp)
target_link_libraries(test_snip_tool PRIVATE
    batbox_permissions
    batbox_config
    simdjson::simdjson)

# CPP 5.4 — WriteTool: atomic write (tmp+rename), parent dir creation, unified diff
# CancelToken.cpp is provided via batbox_core.
# batbox_mcp, batbox_inference, and batbox_tools_bash are OBJECT libs whose objects
# don't propagate transitively through batbox_tools into the test executable —
# add them explicitly so McpServerRegistry, SseParser, and BashRunner symbols resolve.
batbox_add_unit_test(test_write_tool
    unit/test_write_tool.cpp  batbox_tools batbox_core)
target_link_libraries(test_write_tool PRIVATE
    batbox_mcp
    batbox_inference
    batbox_tools_bash
    batbox_permissions
    batbox_config
    simdjson::simdjson)

# CPP 5.11 — PowerShellTool: no-op on macOS/Linux, returns platform error
# Compile PowerShellTool.cpp directly following the pattern of test_snip_tool
# (not all tools are built into batbox_tools yet). Link batbox_permissions for
# PermissionMode and simdjson for Json.hpp.  CancelToken.cpp is provided
# via batbox_core.
batbox_add_unit_test(test_powershell_stub
    unit/test_powershell_stub.cpp  batbox_core)
target_sources(test_powershell_stub PRIVATE
    ${PROJECT_SOURCE_DIR}/src/tools/PowerShellTool.cpp)
target_link_libraries(test_powershell_stub PRIVATE
    batbox_permissions
    batbox_config
    simdjson::simdjson)


# CPP 5.10 — BashTool: ITool adapter around BashRunner (schema, permission gates,
# arg validation, timeout clamping, plan-mode refusal, structured_payload).
# Compile BashTool.cpp + BashRunner.cpp + PtyBackend.cpp + PipesBackend.cpp +
# EnvScrub.cpp + AnsiStrip.cpp directly so this unit test exercises the full
# execution path without needing the full batbox_tools link.
# Link batbox_permissions for PermissionMode and simdjson for Json.hpp.
# CancelToken.cpp is provided via batbox_core.
# macOS ships forkpty in libc; Linux needs -lutil.
batbox_add_unit_test(test_bash_tool_itool
    unit/test_bash_tool_itool.cpp  batbox_core)
target_sources(test_bash_tool_itool PRIVATE
    ${PROJECT_SOURCE_DIR}/src/tools/BashTool.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/bash/BashRunner.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/bash/PtyBackend.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/bash/PipesBackend.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/bash/EnvScrub.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/bash/AnsiStrip.cpp)
target_include_directories(test_bash_tool_itool PRIVATE
    ${PROJECT_SOURCE_DIR}/src/tools/bash)
target_link_libraries(test_bash_tool_itool PRIVATE
    batbox_permissions
    batbox_config
    simdjson::simdjson)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_bash_tool_itool PRIVATE util)
endif()

# CPP 10.5 — McpConfig: load_mcp_config (stdio/sse/http/ws variants, ${env:NAME} expansion, non-fatal per-entry errors)
# Link simdjson explicitly: batbox_config OBJECT lib uses simdjson via batbox_core
# (Json.hpp), which doesn't auto-propagate through OBJECT library chains.
batbox_add_unit_test(test_mcp_config
    unit/test_mcp_config.cpp  batbox_config batbox_core)
target_link_libraries(test_mcp_config PRIVATE simdjson::simdjson)

# CPP 10.6 — KeybindingsConfig: load_keybindings, default_keybindings, overlay, warnings
# Link simdjson explicitly: batbox_config OBJECT lib uses simdjson via batbox_core
# (Json.hpp), which doesn't auto-propagate through OBJECT library chains.
batbox_add_unit_test(test_keybindings_config
    unit/test_keybindings_config.cpp  batbox_config batbox_core)
target_link_libraries(test_keybindings_config PRIVATE simdjson::simdjson spdlog::spdlog)

# CPP 10.3 — SettingsLoader: load/write ~/.batbox/settings.json, atomic write, defaults
# Link simdjson explicitly: batbox_config OBJECT lib uses simdjson via batbox_core
# (Json.hpp), which doesn't auto-propagate through OBJECT library chains.
batbox_add_unit_test(test_settings_loader
    unit/test_settings_loader.cpp  batbox_config batbox_core)
target_link_libraries(test_settings_loader PRIVATE simdjson::simdjson)

# CPP 3.2 — ContextWindow: token counting + auto-compact trigger
# Compile ContextWindow.cpp and Message.cpp directly (same pattern as
# test_message_model) to avoid linking batbox_conversation, which transitively
# requires ToolCallOrchestrator, SessionStore, Client and other in-flight syms.
# Link batbox_config for Config, batbox_core for Uuid/CancelToken/Logging,
# and simdjson explicitly because Json.hpp pulls it in.
batbox_add_unit_test(test_context_window_estimator
    unit/test_context_window_estimator.cpp  batbox_config batbox_core)
target_sources(test_context_window_estimator PRIVATE
    ${PROJECT_SOURCE_DIR}/src/conversation/ContextWindow.cpp
    ${PROJECT_SOURCE_DIR}/src/conversation/Message.cpp)
target_link_libraries(test_context_window_estimator PRIVATE simdjson::simdjson)

# CPP 5.14 — ToolSearchTool: fuzzy/substring search over ToolRegistry
# Compile ToolSearchTool.cpp and ToolRegistry.cpp directly so the test does not
# depend on batbox_tools being fully linked (avoids the batbox_mcp dependency
# chain that includes the in-flight WsTransport.cpp).  CancelToken.cpp is
# provided via batbox_core.
batbox_add_unit_test(test_tool_search
    unit/test_tool_search.cpp  batbox_core)
target_sources(test_tool_search PRIVATE
    ${PROJECT_SOURCE_DIR}/src/tools/ToolSearchTool.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/ToolRegistry.cpp)
target_link_libraries(test_tool_search PRIVATE
    batbox_permissions
    batbox_config
    batbox_config
    simdjson::simdjson)

# CPP K.0 — BundledSkillsRegistry: 13 embedded bundled skills
# Compile BundledSkillsRegistry.cpp and FrontmatterParser.cpp directly into the
# test executable so we do not depend on either batbox_skills or batbox_plugins
# OBJECT library targets (batbox_plugins pulls in CommandLoader which requires
# SlashCommandRegistry — a dependency not yet wired at this build stage).
# The generated include dir provides skills_embed.inc via the build-time embed step.
batbox_add_unit_test(test_bundled_skills
    unit/test_bundled_skills.cpp  batbox_core)
target_sources(test_bundled_skills PRIVATE
    ${PROJECT_SOURCE_DIR}/src/skills/BundledSkillsRegistry.cpp
    ${PROJECT_SOURCE_DIR}/src/plugins/FrontmatterParser.cpp)
target_include_directories(test_bundled_skills PRIVATE
    ${CMAKE_BINARY_DIR}/generated/include)
target_link_libraries(test_bundled_skills PRIVATE
    batbox_core simdjson::simdjson)

# CPP 1.1 — Theme struct, ThemeRole, color_for(), theme_from_name(), load_theme()
# Compile Theme sources directly: batbox_theme is an OBJECT lib; themes.cpp and
# Theme.cpp are now wired into it, but following the established pattern we
# compile them directly to avoid depending on OBJECT library transitive linking.
# Link ftxui::screen for ftxui::Color and batbox_config for Settings.
batbox_add_unit_test(test_theme
    unit/test_theme.cpp  batbox_core)
target_sources(test_theme PRIVATE
    ${PROJECT_SOURCE_DIR}/src/theme/Theme.cpp
    ${PROJECT_SOURCE_DIR}/src/theme/themes.cpp
    ${PROJECT_SOURCE_DIR}/src/tui/ThemeApply.cpp)
target_link_libraries(test_theme PRIVATE
    batbox_config
    ftxui::screen
    simdjson::simdjson)

# CPP 1.2 — Splash: bat ASCII art + rotating Miss Kittin tagline + BATBOX_NO_SPLASH skip
# Compile Splash.cpp directly into the test executable.
# Add src/tui to the include path so splash_taglines.hpp is found by the test.
# Link ftxui::component for Component/ScreenInteractive and ftxui::dom/screen
# for Element rendering.
batbox_add_unit_test(test_splash
    unit/test_splash.cpp  batbox_core)
target_sources(test_splash PRIVATE
    ${PROJECT_SOURCE_DIR}/src/tui/Splash.cpp
    ${PROJECT_SOURCE_DIR}/src/tui/ThemeApply.cpp
    ${PROJECT_SOURCE_DIR}/src/theme/Theme.cpp
    ${PROJECT_SOURCE_DIR}/src/theme/themes.cpp)
target_include_directories(test_splash PRIVATE
    ${PROJECT_SOURCE_DIR}/src/tui)
target_link_libraries(test_splash PRIVATE
    batbox_config
    ftxui::component
    ftxui::dom
    ftxui::screen
    simdjson::simdjson)

# CPP 1.12 — ModalPicker: generic list-pick modal
# Compile ModalPicker.cpp and its theme dependencies directly.
# Link ftxui::component, ftxui::dom, ftxui::screen for FTXUI types.
batbox_add_unit_test(test_modal_picker
    unit/test_modal_picker.cpp  batbox_core)
target_sources(test_modal_picker PRIVATE
    ${PROJECT_SOURCE_DIR}/src/tui/ModalPicker.cpp
    ${PROJECT_SOURCE_DIR}/src/theme/Theme.cpp
    ${PROJECT_SOURCE_DIR}/src/theme/themes.cpp
    ${PROJECT_SOURCE_DIR}/src/tui/ThemeApply.cpp)
target_link_libraries(test_modal_picker PRIVATE
    batbox_config
    ftxui::component  ftxui::dom  ftxui::screen
    simdjson::simdjson)

# CPP 1.11 — DiffCard: unified diff modal with theme-aware coloring, accept/reject
# Compile DiffCard.cpp, Events.cpp, ThemeApply.cpp, Theme.cpp, themes.cpp directly.
# Link ftxui::component ftxui::dom ftxui::screen for FTXUI types and rendering.
# Link batbox_config for Settings (required by load_theme/theme_from_name).
# Link simdjson for Json.hpp.
batbox_add_unit_test(test_diff_card
    unit/test_diff_card.cpp  batbox_core)
target_sources(test_diff_card PRIVATE
    ${PROJECT_SOURCE_DIR}/src/tui/DiffCard.cpp
    ${PROJECT_SOURCE_DIR}/src/tui/Events.cpp
    ${PROJECT_SOURCE_DIR}/src/tui/ThemeApply.cpp
    ${PROJECT_SOURCE_DIR}/src/theme/Theme.cpp
    ${PROJECT_SOURCE_DIR}/src/theme/themes.cpp)
target_link_libraries(test_diff_card PRIVATE
    batbox_config
    ftxui::component  ftxui::dom  ftxui::screen
    simdjson::simdjson)

# CPP 1.3 — MarkdownRenderer: incremental markdown → FTXUI Element
# Compile MarkdownRender.cpp, ThemeApply.cpp, Theme.cpp, themes.cpp directly.
# Link ftxui::dom ftxui::screen for FTXUI Element types.
# Link batbox_config for Settings (used by theme_from_name).
# Link simdjson for Json.hpp.
batbox_add_unit_test(test_markdown_render
    unit/test_markdown_render.cpp  batbox_core)
target_sources(test_markdown_render PRIVATE
    ${PROJECT_SOURCE_DIR}/src/tui/MarkdownRender.cpp
    ${PROJECT_SOURCE_DIR}/src/tui/ThemeApply.cpp
    ${PROJECT_SOURCE_DIR}/src/theme/Theme.cpp
    ${PROJECT_SOURCE_DIR}/src/theme/themes.cpp)
target_link_libraries(test_markdown_render PRIVATE
    batbox_config
    ftxui::component  ftxui::dom  ftxui::screen
    simdjson::simdjson)

# CPP 1.4 — SyntaxHighlight: tree-sitter wrapper + manual lexer fallback
# Compiled with BATBOX_SYNTAX=0 so tests always use the manual-lexer path —
# the tree-sitter path requires vendored git submodules not present in a
# bare checkout.  The manual-lexer .cpp files are compiled directly into the
# test executable.
# Link ftxui::dom + ftxui::screen for Element construction and rendering.
batbox_add_unit_test(test_syntax_highlight
    unit/test_syntax_highlight.cpp  batbox_core)
target_sources(test_syntax_highlight PRIVATE
    ${PROJECT_SOURCE_DIR}/src/tui/SyntaxHighlight.cpp
    ${PROJECT_SOURCE_DIR}/src/tui/ThemeApply.cpp
    ${PROJECT_SOURCE_DIR}/src/tui/manual_lexers/cpp_lexer.cpp
    ${PROJECT_SOURCE_DIR}/src/tui/manual_lexers/python_lexer.cpp
    ${PROJECT_SOURCE_DIR}/src/tui/manual_lexers/js_lexer.cpp
    ${PROJECT_SOURCE_DIR}/src/tui/manual_lexers/rust_lexer.cpp
    ${PROJECT_SOURCE_DIR}/src/tui/manual_lexers/go_lexer.cpp)
target_include_directories(test_syntax_highlight PRIVATE
    ${PROJECT_SOURCE_DIR}/src/tui)
target_compile_definitions(test_syntax_highlight PRIVATE BATBOX_SYNTAX=0)
target_link_libraries(test_syntax_highlight PRIVATE
    ftxui::component  ftxui::dom  ftxui::screen)

# CPP 5.16 — TaskStore + TaskCreate/TaskList/TaskGet/TaskUpdate tools
# Compile TaskStore.cpp and all four tool .cpp files directly so this test
# does not depend on the full batbox_tools OBJECT library being complete.
# Uses the same pattern as test_snip_tool and test_todo_write.
# CancelToken.cpp is provided via batbox_core.
batbox_add_unit_test(test_task_crud
    unit/test_task_crud.cpp  batbox_core)
target_sources(test_task_crud PRIVATE
    ${PROJECT_SOURCE_DIR}/src/tools/TaskStore.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/TaskCreateTool.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/TaskListTool.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/TaskGetTool.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/TaskUpdateTool.cpp)
target_link_libraries(test_task_crud PRIVATE
    batbox_permissions
    batbox_config
    batbox_config
    simdjson::simdjson)

# CPP 1.7 — ChatView: append-only scrollable message-history Component
# Compile ChatView.cpp, MarkdownRender.cpp, ThemeApply.cpp, Theme.cpp,
# themes.cpp, Message.cpp, and Uuid.cpp directly so the test does not
# depend on batbox_tui / batbox_conversation being fully linked.
batbox_add_unit_test(test_chat_view
    unit/test_chat_view.cpp  batbox_core)
target_sources(test_chat_view PRIVATE
    ${PROJECT_SOURCE_DIR}/src/tui/ChatView.cpp
    ${PROJECT_SOURCE_DIR}/src/tui/Events.cpp
    ${PROJECT_SOURCE_DIR}/src/tui/MarkdownRender.cpp
    ${PROJECT_SOURCE_DIR}/src/tui/ThemeApply.cpp
    ${PROJECT_SOURCE_DIR}/src/tui/QuestionCard.cpp
    ${PROJECT_SOURCE_DIR}/src/tui/InputBar.cpp
    ${PROJECT_SOURCE_DIR}/src/theme/Theme.cpp
    ${PROJECT_SOURCE_DIR}/src/theme/themes.cpp
    ${PROJECT_SOURCE_DIR}/src/conversation/Message.cpp)
# TUI-FLOW-T6: PerfSnapshot.cpp required because InputBar.cpp #includes it.
# TUI-FLOW-T6: repl sources required because InputBar depends on History + Keybindings + VimMode.
target_sources(test_chat_view PRIVATE
    ${PROJECT_SOURCE_DIR}/src/perf/PerfSnapshot.cpp
    ${PROJECT_SOURCE_DIR}/src/repl/History.cpp
    ${PROJECT_SOURCE_DIR}/src/repl/Keybindings.cpp
    ${PROJECT_SOURCE_DIR}/src/repl/VimMode.cpp
    ${PROJECT_SOURCE_DIR}/src/repl/Autocomplete.cpp
    ${PROJECT_SOURCE_DIR}/src/commands/SlashCommandRegistry.cpp
    ${PROJECT_SOURCE_DIR}/src/plugins/PluginRegistry.cpp
    ${PROJECT_SOURCE_DIR}/src/plugins/MarketplaceJson.cpp
    ${PROJECT_SOURCE_DIR}/src/plugins/FrontmatterParser.cpp)
target_link_libraries(test_chat_view PRIVATE
    batbox_config
    batbox_perf
    batbox_permissions
    ftxui::component  ftxui::dom  ftxui::screen
    simdjson::simdjson
    spdlog::spdlog)

# CPP 1.14 — DemonPanel: 5Hz magenta easter-egg panel + TuiDemonTickerThread
# Compile DemonPanel.cpp, Events.cpp, ThemeApply.cpp, Theme.cpp, themes.cpp
# directly so the test does not depend on batbox_tui being fully linked.
# Link ftxui::component for ScreenInteractive (ticker PostEvent), ftxui::dom
# and ftxui::screen for Element rendering.
batbox_add_unit_test(test_demon_panel
    unit/test_demon_panel.cpp  batbox_core)
target_sources(test_demon_panel PRIVATE
    ${PROJECT_SOURCE_DIR}/src/tui/DemonPanel.cpp
    ${PROJECT_SOURCE_DIR}/src/tui/Events.cpp
    ${PROJECT_SOURCE_DIR}/src/tui/ThemeApply.cpp
    ${PROJECT_SOURCE_DIR}/src/theme/Theme.cpp
    ${PROJECT_SOURCE_DIR}/src/theme/themes.cpp)
target_link_libraries(test_demon_panel PRIVATE
    batbox_config
    ftxui::component  ftxui::dom  ftxui::screen
    simdjson::simdjson)

# CPP 5.17 — CronScheduler + CronCreateTool / CronDeleteTool / CronListTool
# CancelToken.cpp is provided via batbox_core.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/unit/test_cron_expr.cpp)
    batbox_add_unit_test(test_cron_expr
        unit/test_cron_expr.cpp  batbox_core)
    target_sources(test_cron_expr PRIVATE
        ${PROJECT_SOURCE_DIR}/src/tools/CronScheduler.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/CronCreateTool.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/CronDeleteTool.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/CronListTool.cpp)
    target_link_libraries(test_cron_expr PRIVATE
        batbox_permissions
        batbox_config
        simdjson::simdjson)
endif()

# ---------------------------------------------------------------------------
# CPP T.1 — Static-egress CI gate
#
# test_egress_static_gate: reads the compiled batbox binary and asserts that
# none of the ten forbidden hostname/telemetry tokens from pmdraft.md
# §Static-Egress Test are present as compiled-in strings.
#
# The test source lives at tests/test_egress_static.cpp (top-level, not
# unit/) because it is a release-binary gate — not a component unit test.
#
# BATBOX_BINARY_DEFAULT is injected at compile time so the test knows
# where CMake placed the final batbox executable without requiring the
# caller to set an environment variable.
#
# CTest properties:
#   - Labels: egress, security, static-analysis (matches EgressCheck.cmake)
#   - Timeout: 30 s (reading a stripped binary is fast on all platforms)
# ---------------------------------------------------------------------------
add_executable(test_egress_static_gate
    ${CMAKE_CURRENT_SOURCE_DIR}/test_egress_static.cpp)

set_target_properties(test_egress_static_gate PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF)

target_include_directories(test_egress_static_gate
    PRIVATE ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(test_egress_static_gate
    PRIVATE doctest::doctest)

# Inject the binary path at compile time so the test finds the binary without
# needing an environment variable.  $<TARGET_FILE:batbox> expands to the full
# path to the linked batbox executable.  The outer TARGET check guards against
# configurations where the main batbox target is not yet defined (e.g., when
# BATBOX_TESTS is enabled from a sub-directory build).
if(TARGET batbox)
    target_compile_definitions(test_egress_static_gate PRIVATE
        BATBOX_BINARY_DEFAULT="$<TARGET_FILE:batbox>")

    # Ensure the gate runs after the binary is linked so $<TARGET_FILE:batbox>
    # is valid when the test executable itself is built.
    add_dependencies(test_egress_static_gate batbox)
else()
    # Fallback: point at the standard build-dir location.
    target_compile_definitions(test_egress_static_gate PRIVATE
        BATBOX_BINARY_DEFAULT="${CMAKE_BINARY_DIR}/batbox")
endif()

doctest_discover_tests(test_egress_static_gate)

# Register as a named CTest test so `ctest -R egress` runs it alongside the
# POST_BUILD check from EgressCheck.cmake.
add_test(
    NAME    egress_static_gate
    COMMAND test_egress_static_gate)

set_tests_properties(egress_static_gate PROPERTIES
    LABELS              "egress;security;static-analysis"
    TIMEOUT             30
    FAIL_REGULAR_EXPRESSION "FAILED")

# CPP 6.2 — AgentSpec: frontmatter-driven agent config, from_file, from_type
# Compile AgentSpec.cpp and FrontmatterParser.cpp directly into the test executable
# to avoid the batbox_agents ↔ batbox_plugins circular library dependency.
# Link simdjson for Json.hpp.
batbox_add_unit_test(test_agent_spec
    unit/test_agent_spec.cpp  batbox_core)
target_sources(test_agent_spec PRIVATE
    ${PROJECT_SOURCE_DIR}/src/agents/AgentSpec.cpp
    ${PROJECT_SOURCE_DIR}/src/plugins/FrontmatterParser.cpp)
target_link_libraries(test_agent_spec PRIVATE simdjson::simdjson)

# ---------------------------------------------------------------------------
# CPP 6.x — AgentEvent + AgentEventQueue: all 10 Kind values, MPSC queue
# Compile AgentEvent.cpp directly (same pattern as test_agent_spec).
# ---------------------------------------------------------------------------
batbox_add_unit_test(test_agent_event
    unit/test_agent_event.cpp  batbox_core)
target_link_libraries(test_agent_event PRIVATE batbox_agents)

# ---------------------------------------------------------------------------
# CPP 2.3 — Autocomplete: file paths, slash commands, @-mentions, agent [[ prefix
# Compile Autocomplete.cpp + SlashCommandRegistry.cpp + PluginRegistry.cpp +
# MarketplaceJson.cpp + FrontmatterParser.cpp directly.
# simdjson for Json.hpp / nlohmann parse paths.
# ---------------------------------------------------------------------------
batbox_add_unit_test(test_autocomplete
    unit/test_autocomplete.cpp  batbox_core)
target_sources(test_autocomplete PRIVATE
    ${PROJECT_SOURCE_DIR}/src/repl/Autocomplete.cpp
    ${PROJECT_SOURCE_DIR}/src/commands/SlashCommandRegistry.cpp
    ${PROJECT_SOURCE_DIR}/src/plugins/PluginRegistry.cpp
    ${PROJECT_SOURCE_DIR}/src/plugins/MarketplaceJson.cpp
    ${PROJECT_SOURCE_DIR}/src/plugins/FrontmatterParser.cpp)
target_link_libraries(test_autocomplete PRIVATE
    batbox_config
    simdjson::simdjson
    spdlog::spdlog)

# ---------------------------------------------------------------------------
# CPP 4.1 — ChatRequest.hpp + ChatResponse.hpp wire models round-trip
# Compile ChatRequest.cpp directly; link simdjson for Json.hpp.
# ---------------------------------------------------------------------------
batbox_add_unit_test(test_chat_wire_model
    unit/test_chat_wire_model.cpp  batbox_core)
target_sources(test_chat_wire_model PRIVATE
    ${PROJECT_SOURCE_DIR}/src/inference/ChatRequest.cpp)
target_link_libraries(test_chat_wire_model PRIVATE simdjson::simdjson)

# ---------------------------------------------------------------------------
# CPP 10.2 — EnvLoader: load_env_file + ${env:NAME} expansion
# Compile EnvLoader.cpp directly; link simdjson for Json.hpp.
# ---------------------------------------------------------------------------
batbox_add_unit_test(test_env_loader
    unit/test_env_loader.cpp  batbox_core)
target_sources(test_env_loader PRIVATE
    ${PROJECT_SOURCE_DIR}/src/config/EnvLoader.cpp)
target_link_libraries(test_env_loader PRIVATE simdjson::simdjson)

# ---------------------------------------------------------------------------
# CPP 11.1 — FrontmatterParser: YAML-like frontmatter block parse
# Compile FrontmatterParser.cpp directly; no extra link deps.
# ---------------------------------------------------------------------------
batbox_add_unit_test(test_frontmatter_parser
    unit/test_frontmatter_parser.cpp  batbox_core)
target_sources(test_frontmatter_parser PRIVATE
    ${PROJECT_SOURCE_DIR}/src/plugins/FrontmatterParser.cpp)

# ---------------------------------------------------------------------------
# CPP 5.6 — GlobTool: filesystem glob with recursive_directory_iterator
# Compile GlobTool.cpp + PatternMatcher.cpp directly.
# link simdjson for Json.hpp / PermissionMode.
# ---------------------------------------------------------------------------
batbox_add_unit_test(test_glob_tool
    unit/test_glob_tool.cpp  batbox_core)
target_sources(test_glob_tool PRIVATE
    ${PROJECT_SOURCE_DIR}/src/tools/GlobTool.cpp)
target_link_libraries(test_glob_tool PRIVATE
    batbox_permissions
    batbox_config
    simdjson::simdjson)

# ---------------------------------------------------------------------------
# CPP 1.9 — InputBar: prompt + status line + slash palette + autocomplete
# Compile InputBar.cpp + History.cpp + Keybindings.cpp + Autocomplete.cpp +
# the full theme stack + Events.cpp + SlashCommandRegistry.cpp +
# PluginRegistry.cpp + MarketplaceJson.cpp + FrontmatterParser.cpp directly.
# Link ftxui for Event / Element, simdjson for Json.hpp, spdlog for logging.
# ---------------------------------------------------------------------------
batbox_add_unit_test(test_input_bar
    unit/test_input_bar.cpp  batbox_core)
target_sources(test_input_bar PRIVATE
    ${PROJECT_SOURCE_DIR}/src/tui/InputBar.cpp
    ${PROJECT_SOURCE_DIR}/src/tui/Events.cpp
    ${PROJECT_SOURCE_DIR}/src/tui/ThemeApply.cpp
    ${PROJECT_SOURCE_DIR}/src/tui/MarkdownRender.cpp
    ${PROJECT_SOURCE_DIR}/src/theme/Theme.cpp
    ${PROJECT_SOURCE_DIR}/src/theme/themes.cpp
    ${PROJECT_SOURCE_DIR}/src/repl/History.cpp
    ${PROJECT_SOURCE_DIR}/src/repl/Keybindings.cpp
    ${PROJECT_SOURCE_DIR}/src/repl/VimMode.cpp
    ${PROJECT_SOURCE_DIR}/src/repl/Autocomplete.cpp
    ${PROJECT_SOURCE_DIR}/src/commands/SlashCommandRegistry.cpp
    ${PROJECT_SOURCE_DIR}/src/plugins/PluginRegistry.cpp
    ${PROJECT_SOURCE_DIR}/src/plugins/MarketplaceJson.cpp
    ${PROJECT_SOURCE_DIR}/src/plugins/FrontmatterParser.cpp
    ${PROJECT_SOURCE_DIR}/src/perf/PerfSnapshot.cpp)
target_link_libraries(test_input_bar PRIVATE
    batbox_config
    batbox_perf
    batbox_permissions
    ftxui::component  ftxui::dom  ftxui::screen
    simdjson::simdjson
    spdlog::spdlog)

# ---------------------------------------------------------------------------
# CPP 0.x — Json helpers: Json.hpp type alias + parse_json / to_json_string
# Compile Json.cpp directly; link simdjson explicitly.
# ---------------------------------------------------------------------------
batbox_add_unit_test(test_json_helpers
    unit/test_json_helpers.cpp  batbox_core)
target_link_libraries(test_json_helpers PRIVATE simdjson::simdjson)

# ---------------------------------------------------------------------------
# CPP 2.5 — Keybindings: defaults + override loader + event_to_action dispatch
# Compile Keybindings.cpp directly; link batbox_config for KeybindingsConfig.
# ---------------------------------------------------------------------------
batbox_add_unit_test(test_keybindings
    unit/test_keybindings.cpp  batbox_core)
target_sources(test_keybindings PRIVATE
    ${PROJECT_SOURCE_DIR}/src/repl/Keybindings.cpp)
target_link_libraries(test_keybindings PRIVATE
    batbox_config
    ftxui::component  ftxui::dom  ftxui::screen
    simdjson::simdjson
    spdlog::spdlog)

# ---------------------------------------------------------------------------
# CPP 8.3 — IMcpTransport interface + MockTransport (defined in test file)
# Compile JsonRpc.cpp directly so IMcpTransport methods resolve; link simdjson.
# ---------------------------------------------------------------------------
batbox_add_unit_test(test_mcp_mock_transport
    unit/test_mcp_mock_transport.cpp  batbox_core)
target_sources(test_mcp_mock_transport PRIVATE
    ${PROJECT_SOURCE_DIR}/src/mcp/JsonRpc.cpp)
target_link_libraries(test_mcp_mock_transport PRIVATE
    batbox_config
    simdjson::simdjson)

# ---------------------------------------------------------------------------
# CPP 12.2 — PatternMatcher: fnmatch-style + tool-arg matching
# Compile PatternMatcher.cpp + Json.cpp directly.
# ---------------------------------------------------------------------------
batbox_add_unit_test(test_pattern_matcher
    unit/test_pattern_matcher.cpp  batbox_core)
target_link_libraries(test_pattern_matcher PRIVATE
    batbox_permissions
    batbox_config
    simdjson::simdjson)

# ---------------------------------------------------------------------------
# CPP F.x — PermissionMode helpers: round-trip parse/to_string, cycle_next
# Compile PermissionMode.cpp directly; no extra link deps.
# ---------------------------------------------------------------------------
batbox_add_unit_test(test_permission_mode
    unit/test_permission_mode.cpp  batbox_core)
target_link_libraries(test_permission_mode PRIVATE
    batbox_permissions
    batbox_config
    simdjson::simdjson)

# ---------------------------------------------------------------------------
# CPP 12.3 — PermissionRule + PermissionStore: load/persist allow/deny/ask rules
# Compile PermissionRule.cpp + PermissionStore.cpp + SettingsLoader.cpp +
# Json.cpp + Paths.cpp directly; link simdjson for parse paths.
# ---------------------------------------------------------------------------
batbox_add_unit_test(test_permission_store
    unit/test_permission_store.cpp  batbox_core)
target_link_libraries(test_permission_store PRIVATE
    batbox_config
    batbox_permissions
    simdjson::simdjson
    spdlog::spdlog)

# ---------------------------------------------------------------------------
# CPP 11.3 — Plugin struct + PluginRegistry: all_plugins / active_plugins
# Compile PluginRegistry.cpp + MarketplaceJson.cpp + FrontmatterParser.cpp
# directly; link simdjson + spdlog.
# ---------------------------------------------------------------------------
batbox_add_unit_test(test_plugin_registry
    unit/test_plugin_registry.cpp  batbox_core)
target_sources(test_plugin_registry PRIVATE
    ${PROJECT_SOURCE_DIR}/src/plugins/PluginRegistry.cpp
    ${PROJECT_SOURCE_DIR}/src/plugins/MarketplaceJson.cpp
    ${PROJECT_SOURCE_DIR}/src/plugins/FrontmatterParser.cpp)
target_link_libraries(test_plugin_registry PRIVATE
    batbox_config
    simdjson::simdjson
    spdlog::spdlog)

# ---------------------------------------------------------------------------
# CPP 4.7 — Provider quirk handling: resolve_provider_hint + apply_provider_quirks
# Compile Client.cpp + ChatRequest.cpp directly; link batbox_config + simdjson.
# ---------------------------------------------------------------------------
batbox_add_unit_test(test_provider_hint
    unit/test_provider_hint.cpp  batbox_core)
target_sources(test_provider_hint PRIVATE
    ${PROJECT_SOURCE_DIR}/src/inference/Client.cpp
    ${PROJECT_SOURCE_DIR}/src/inference/ChatRequest.cpp
    ${PROJECT_SOURCE_DIR}/src/inference/SseParser.cpp)
target_link_libraries(test_provider_hint PRIVATE
    batbox_config
    cpr::cpr
    simdjson::simdjson
    spdlog::spdlog)

# ---------------------------------------------------------------------------
# CPP 7.1 — ScraplingProto: request/response struct round-trips
# Compile ScraplingProto.cpp + Json.cpp directly; link simdjson.
# ---------------------------------------------------------------------------
batbox_add_unit_test(test_scrapling_proto
    unit/test_scrapling_proto.cpp  batbox_core)
target_link_libraries(test_scrapling_proto PRIVATE
    batbox_sidecar
    simdjson::simdjson)

# ---------------------------------------------------------------------------
# CPP 9.x — IndexRebuilder / SessionRecovery: missing + corrupt index rebuild
# Compile SessionRecovery.cpp + SessionIndex.cpp directly; link simdjson +
# spdlog + batbox_session for SessionFile gzip support.
# ---------------------------------------------------------------------------
batbox_add_unit_test(test_session_recovery
    unit/test_session_recovery.cpp  batbox_core)
target_link_libraries(test_session_recovery PRIVATE
    batbox_session
    simdjson::simdjson
    spdlog::spdlog
    ZLIB::ZLIB)

# ---------------------------------------------------------------------------
# CPP 5.21 — SleepTool: sleep_ms duration + cancellation + >300s rejection
# Compile SleepTool.cpp directly; link batbox_permissions + simdjson.
# ---------------------------------------------------------------------------
batbox_add_unit_test(test_sleep_tool
    unit/test_sleep_tool.cpp  batbox_core)
target_sources(test_sleep_tool PRIVATE
    ${PROJECT_SOURCE_DIR}/src/tools/SleepTool.cpp)
target_link_libraries(test_sleep_tool PRIVATE
    batbox_permissions
    batbox_config
    simdjson::simdjson)

# ---------------------------------------------------------------------------
# CPP 1.8 — StreamingMessageView: SSE token streaming render + shared_mutex
# Compile StreamingMessageView.cpp + MarkdownRender.cpp + Events.cpp +
# ThemeApply.cpp + Theme.cpp + themes.cpp directly.
# Link ftxui for Event / Element / Screen; simdjson for Json.hpp.
# ---------------------------------------------------------------------------
batbox_add_unit_test(test_streaming_message_view
    unit/test_streaming_message_view.cpp  batbox_core)
target_sources(test_streaming_message_view PRIVATE
    ${PROJECT_SOURCE_DIR}/src/tui/StreamingMessageView.cpp
    ${PROJECT_SOURCE_DIR}/src/tui/MarkdownRender.cpp
    ${PROJECT_SOURCE_DIR}/src/tui/Events.cpp
    ${PROJECT_SOURCE_DIR}/src/tui/ThemeApply.cpp
    ${PROJECT_SOURCE_DIR}/src/theme/Theme.cpp
    ${PROJECT_SOURCE_DIR}/src/theme/themes.cpp)
target_link_libraries(test_streaming_message_view PRIVATE
    batbox_config
    ftxui::component  ftxui::dom  ftxui::screen
    simdjson::simdjson)

# ---------------------------------------------------------------------------
# CPP 3.8 — SystemPrompt: compose_system_prompt BATBOX.md merge + @import + plan-mode
# Compile SystemPrompt.cpp + Paths.cpp directly; no extra link deps.
# ---------------------------------------------------------------------------
batbox_add_unit_test(test_system_prompt
    unit/test_system_prompt.cpp  batbox_core)
target_sources(test_system_prompt PRIVATE
    ${PROJECT_SOURCE_DIR}/src/conversation/SystemPrompt.cpp)

# ---------------------------------------------------------------------------
# CPP 5.20 — AskUserQuestionTool: single-select, multi-select, headless mode
# Integration test: compile AskUserQuestionTool.cpp directly.
# Link batbox_permissions + batbox_config + simdjson for tool infrastructure.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_ask_user_question.cpp)
    add_executable(test_ask_user_question
        ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_ask_user_question.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/AskUserQuestionTool.cpp)
    set_target_properties(test_ask_user_question PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_include_directories(test_ask_user_question PRIVATE
        ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(test_ask_user_question PRIVATE
        doctest::doctest
        batbox_core
        batbox_permissions
        batbox_config
        simdjson::simdjson)
    doctest_discover_tests(test_ask_user_question)
endif()

# ---------------------------------------------------------------------------
# CPP 5.5 — EditTool: single match, zero matches, multiple matches, replace_all
# Integration test: compile EditTool.cpp directly.
# Link batbox_permissions + batbox_config + simdjson for tool infrastructure.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_edit_tool.cpp)
    add_executable(test_edit_tool
        ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_edit_tool.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/EditTool.cpp)
    set_target_properties(test_edit_tool PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_include_directories(test_edit_tool PRIVATE
        ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(test_edit_tool PRIVATE
        doctest::doctest
        batbox_core
        batbox_permissions
        batbox_config
        simdjson::simdjson)
    doctest_discover_tests(test_edit_tool)
endif()

# ---------------------------------------------------------------------------
# CPP 1.10 — PermissionCard: all 10 key-press decisions, await_user_decision
# Integration test: compile PermissionCard.cpp + Events.cpp + ThemeApply.cpp
# + Theme.cpp + themes.cpp + PermissionGate.cpp + PermissionRule.cpp +
# PatternMatcher.cpp + PermissionMode.cpp + PermissionStore.cpp +
# SettingsLoader.cpp + Json.cpp directly.
# Link ftxui::component/dom/screen + simdjson.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_permission_card.cpp)
    find_package(ftxui CONFIG QUIET)
    add_executable(test_permission_card
        ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_permission_card.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/PermissionCard.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/Events.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/ThemeApply.cpp
        ${PROJECT_SOURCE_DIR}/src/theme/Theme.cpp
        ${PROJECT_SOURCE_DIR}/src/theme/themes.cpp
        ${PROJECT_SOURCE_DIR}/src/permissions/PermissionGate.cpp
        ${PROJECT_SOURCE_DIR}/src/permissions/PermissionRule.cpp
        ${PROJECT_SOURCE_DIR}/src/permissions/PatternMatcher.cpp
        ${PROJECT_SOURCE_DIR}/src/permissions/PermissionMode.cpp
        ${PROJECT_SOURCE_DIR}/src/permissions/PermissionStore.cpp)
    set_target_properties(test_permission_card PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_include_directories(test_permission_card PRIVATE
        ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(test_permission_card PRIVATE
        doctest::doctest
        batbox_core
        batbox_config
        $<$<TARGET_EXISTS:ftxui::component>:ftxui::component>
        $<$<TARGET_EXISTS:ftxui::dom>:ftxui::dom>
        $<$<TARGET_EXISTS:ftxui::screen>:ftxui::screen>
        simdjson::simdjson
        spdlog::spdlog)
    doctest_discover_tests(test_permission_card)
endif()

# ---------------------------------------------------------------------------
# TUI-PLAN-T2 — PlanApprovalCard: all 3 action keys, await_user_decision,
#               pending() lifecycle, OnRender() smoke, plan_text() accessor.
# Integration test: compile PlanApprovalCard.cpp + Events.cpp + ThemeApply.cpp
# + Theme.cpp + themes.cpp + Json.cpp directly.
# Link ftxui::component/dom/screen + simdjson.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_plan_approval_card.cpp)
    find_package(ftxui CONFIG QUIET)
    add_executable(test_plan_approval_card
        ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_plan_approval_card.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/PlanApprovalCard.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/Events.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/ThemeApply.cpp
        ${PROJECT_SOURCE_DIR}/src/theme/Theme.cpp
        ${PROJECT_SOURCE_DIR}/src/theme/themes.cpp)
    set_target_properties(test_plan_approval_card PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_include_directories(test_plan_approval_card PRIVATE
        ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(test_plan_approval_card PRIVATE
        doctest::doctest
        batbox_core
        batbox_config
        $<$<TARGET_EXISTS:ftxui::component>:ftxui::component>
        $<$<TARGET_EXISTS:ftxui::dom>:ftxui::dom>
        $<$<TARGET_EXISTS:ftxui::screen>:ftxui::screen>
        simdjson::simdjson
        spdlog::spdlog)
    doctest_discover_tests(test_plan_approval_card)
endif()

# ---------------------------------------------------------------------------
# TUI-ASKQ-T2 — QuestionCard: renderer (header chip, option rows, footer hints).
# Unit test: compile QuestionCard.cpp + Events.cpp + ThemeApply.cpp + Theme sources.
# Link ftxui::component/dom/screen + simdjson.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/unit/test_question_card.cpp)
    find_package(ftxui CONFIG QUIET)
    batbox_add_unit_test(test_question_card
        unit/test_question_card.cpp  batbox_core)
    target_sources(test_question_card PRIVATE
        ${PROJECT_SOURCE_DIR}/src/tui/QuestionCard.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/Events.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/ThemeApply.cpp
        ${PROJECT_SOURCE_DIR}/src/theme/Theme.cpp
        ${PROJECT_SOURCE_DIR}/src/theme/themes.cpp)
    target_link_libraries(test_question_card PRIVATE
        batbox_config
        ftxui::component  ftxui::dom  ftxui::screen
        simdjson::simdjson)
endif()

# ---------------------------------------------------------------------------
# CPP 3.x — EnterPlanModeTool + ExitPlanModeTool + VerifyPlanExecutionTool
# Integration test: compile the three tool .cpp files + PlanMode.cpp directly.
# Link batbox_config + batbox_permissions + simdjson.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_plan_mode.cpp)
    add_executable(test_plan_mode
        ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_plan_mode.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/EnterPlanModeTool.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/ExitPlanModeTool.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/VerifyPlanExecutionTool.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/PlanMode.cpp)
    set_target_properties(test_plan_mode PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_include_directories(test_plan_mode PRIVATE
        ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(test_plan_mode PRIVATE
        doctest::doctest
        batbox_core
        batbox_permissions
        batbox_config
        simdjson::simdjson)
    doctest_discover_tests(test_plan_mode)
endif()

# ---------------------------------------------------------------------------
# CPP 9.4 — SessionStore: new_session, append_message, list_recent, load,
#             resume_for_cwd, thread-safety, current_session_id
# Integration test: compile SessionStore.cpp + SessionFile.cpp +
# SessionIndex.cpp + SessionRecovery.cpp directly.
# Link batbox_core + simdjson + spdlog + ZLIB.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_session_resume.cpp)
    find_package(ZLIB REQUIRED)
    add_executable(test_session_resume
        ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_session_resume.cpp)
    set_target_properties(test_session_resume PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_include_directories(test_session_resume PRIVATE
        ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(test_session_resume PRIVATE
        doctest::doctest
        batbox_core
        batbox_session
        simdjson::simdjson
        spdlog::spdlog
        ZLIB::ZLIB)
    doctest_discover_tests(test_session_resume)
endif()

# NOTE: test_tool_orchestration is NOT registered.
# The test source (tests/integration/test_tool_orchestration.cpp) contains
# CHECK() macros with || operators inside TEST_CASE bodies at lines 243, 419,
# 487, 648 which doctest cannot decompose ("Expression Too Complex — rewrite as
# binary comparison").  Fixing those expressions requires modifying test logic,
# which is out of scope for this task.  Surfaced for follow-up.

# ---------------------------------------------------------------------------
# Fix #16 — tools[] + tool_choice serialisation in /v1/chat/completions bodies
# Compile ToolRegistry.cpp + ChatRequest.cpp directly (same pattern as
# test_tool_registry + test_chat_wire_model). Link batbox_permissions for
# PermissionMode, batbox_config for Config, simdjson for Json.hpp.
# ---------------------------------------------------------------------------
batbox_add_unit_test(test_openai_client_tools
    unit/test_openai_client_tools.cpp  batbox_core)
target_sources(test_openai_client_tools PRIVATE
    ${PROJECT_SOURCE_DIR}/src/tools/ToolRegistry.cpp
    ${PROJECT_SOURCE_DIR}/src/inference/ChatRequest.cpp)
target_link_libraries(test_openai_client_tools PRIVATE
    batbox_permissions
    batbox_config
    simdjson::simdjson)

# ---------------------------------------------------------------------------
# Task 23 — Model alias resolution: resolve_model_alias unit tests
# Link batbox_config + batbox_core + simdjson (same pattern as test_config_load).
# ---------------------------------------------------------------------------
batbox_add_unit_test(test_model_alias
    unit/test_model_alias.cpp  batbox_config batbox_core)
target_link_libraries(test_model_alias PRIVATE simdjson::simdjson)

# ---------------------------------------------------------------------------
# Task 24 — Sidecar python_bin auto-resolution from venv at config load time
# Link batbox_config + batbox_core + simdjson (same pattern as test_config_load).
# ---------------------------------------------------------------------------
batbox_add_unit_test(test_sidecar_config
    unit/test_sidecar_config.cpp  batbox_config batbox_core)
target_link_libraries(test_sidecar_config PRIVATE simdjson::simdjson)

# ---------------------------------------------------------------------------
# TUI-T11 — UI-D9: token_received stays false during reasoning phase (#30)
# Tests that StreamDelta::from_json deserialises reasoning_content chunks and
# that the stream_started retry gate fires on reasoning-only deltas while
# token_received (telemetry) remains content-only.
# Compile ChatRequest.cpp directly (same pattern as test_openai_client_tools)
# to avoid duplicate symbols from batbox_inference OBJECT library.
# ---------------------------------------------------------------------------
batbox_add_unit_test(test_client_reasoning
    unit/test_client_reasoning.cpp  batbox_core)
target_sources(test_client_reasoning PRIVATE
    ${PROJECT_SOURCE_DIR}/src/inference/ChatRequest.cpp)
target_link_libraries(test_client_reasoning PRIVATE
    batbox_config
    simdjson::simdjson)


# ---------------------------------------------------------------------------
# TUI smoke tests (tmux-based, require real pty + BatBox binary + tmux)
#
# OFF by default so normal ctest / ctest -j8 runs do not include them.
# Enable explicitly and run with the label filter:
#
#   cmake -DBATBOX_TUI_SMOKE=ON <build-dir>
#   ctest -L tui-smoke
#   ctest -L tui-smoke -V
#
# Or invoke the harness directly (no cmake needed):
#   tests/tui-smoke/bin/harness smoke
#
# See tests/tui-smoke/README.md for full setup instructions.
# ---------------------------------------------------------------------------
option(BATBOX_TUI_SMOKE
    "Register tmux-based TUI smoke tests with CTest (requires tmux + BatBox binary)"
    OFF)

if(BATBOX_TUI_SMOKE)
    add_subdirectory(tui-smoke)
endif()

# TUI-FLOW-T3 — PerfSnapshot atomic store/load under contention
batbox_add_unit_test(test_perf_snapshot
    unit/test_perf_snapshot.cpp  batbox_perf)

# TUI-FLOW-T10 — Changelog parser + disk loader
# Compile Changelog.cpp directly (no full batbox_tui link needed for parser-only test).
batbox_add_unit_test(test_changelog
    unit/test_changelog.cpp  batbox_core)
target_sources(test_changelog PRIVATE
    ${PROJECT_SOURCE_DIR}/src/tui/Changelog.cpp)
target_include_directories(test_changelog PRIVATE
    ${PROJECT_SOURCE_DIR}/src/tui)

# TUI-FLOW-T10 — StateStore: state.json read/write
batbox_add_unit_test(test_state_store
    unit/test_state_store.cpp  batbox_session batbox_config batbox_core)

# TUI-FLOW-T11 — McpStatusPoller: polls MCP registry and fires on_change callback.
# McpStatusPoller.cpp is compiled directly (not a library target) so we add it
# as an extra source alongside the test file.
# batbox_mcp links batbox_config (for load_mcp_configs) and batbox_inference (for SseParser).
batbox_add_unit_test(test_mcp_status_poller
    unit/test_mcp_status_poller.cpp  batbox_mcp batbox_config batbox_inference batbox_core)
target_sources(test_mcp_status_poller PRIVATE
    ${PROJECT_SOURCE_DIR}/src/app/McpStatusPoller.cpp)
target_link_libraries(test_mcp_status_poller PRIVATE simdjson::simdjson)
