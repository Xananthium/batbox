# =============================================================================
# tests/unit/CMakeLists.txt — batbox unit test registration
#
# This file is included via add_subdirectory(unit) from tests/CMakeLists.txt,
# which defines the batbox_add_unit_test() macro and ensures doctest::doctest
# is available before this file is processed.
#
# Naming convention:
#   test_<component>.cpp  →  executable target named  test_<component>
#
# Test sources are listed explicitly (not via file(GLOB)) so that:
#   1. CMake re-runs automatically when a new file is added to the list.
#   2. CI never silently picks up incomplete test stubs.
#
# Tests whose source files are not yet present (in-flight tasks) are guarded
# with if(EXISTS ...) so the build configures cleanly and ctest reports zero
# tests for those targets rather than a configure error.
# =============================================================================

# ---------------------------------------------------------------------------
# Landing tests — source files exist, register unconditionally.
# Each test links batbox_core; specialised tests add their component target.
# ---------------------------------------------------------------------------

# CPP 0.1 — Result<T,E> type
batbox_add_unit_test(test_result
    ${CMAKE_CURRENT_SOURCE_DIR}/test_result.cpp
    batbox_core)

# CPP 0.2 — filesystem path helpers (Paths.hpp / Paths.cpp)
batbox_add_unit_test(test_paths
    ${CMAKE_CURRENT_SOURCE_DIR}/test_paths.cpp
    batbox_core)

# CPP 0.4 — CancelToken / CancelSource / combine_tokens
batbox_add_unit_test(test_cancel_token
    ${CMAKE_CURRENT_SOURCE_DIR}/test_cancel_token.cpp
    batbox_core)

# CPP 0.6 — Uuid v4 generation
batbox_add_unit_test(test_uuid
    ${CMAKE_CURRENT_SOURCE_DIR}/test_uuid.cpp
    batbox_core)

# CPP 2.4 — VimMode state machine (Normal/Insert/Visual, hjkl, dd/yy/p, w/b/e, text objects)
batbox_add_unit_test(test_vim_mode
    ${CMAKE_CURRENT_SOURCE_DIR}/test_vim_mode.cpp
    batbox_core)

# ---------------------------------------------------------------------------
# In-flight tests — guarded by if(EXISTS); configure succeeds even when the
# source file has not yet been committed by its owning CPP task.
# Remove the guard once the file lands.
# ---------------------------------------------------------------------------

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_json_helpers.cpp)
    batbox_add_unit_test(test_json_helpers
        ${CMAKE_CURRENT_SOURCE_DIR}/test_json_helpers.cpp
        batbox_core)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_logging.cpp)
    batbox_add_unit_test(test_logging
        ${CMAKE_CURRENT_SOURCE_DIR}/test_logging.cpp
        batbox_core)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_tui_events.cpp)
    # batbox_tui may not exist yet — guard against missing target as well
    if(TARGET batbox_tui)
        batbox_add_unit_test(test_tui_events
            ${CMAKE_CURRENT_SOURCE_DIR}/test_tui_events.cpp
            batbox_tui batbox_core)
    else()
        batbox_add_unit_test(test_tui_events
            ${CMAKE_CURRENT_SOURCE_DIR}/test_tui_events.cpp
            batbox_core)
    endif()
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_env_loader.cpp)
    if(TARGET batbox_config)
        batbox_add_unit_test(test_env_loader
            ${CMAKE_CURRENT_SOURCE_DIR}/test_env_loader.cpp
            batbox_config batbox_core)
    else()
        batbox_add_unit_test(test_env_loader
            ${CMAKE_CURRENT_SOURCE_DIR}/test_env_loader.cpp
            batbox_core)
    endif()
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_permission_mode.cpp)
    if(TARGET batbox_permissions)
        batbox_add_unit_test(test_permission_mode
            ${CMAKE_CURRENT_SOURCE_DIR}/test_permission_mode.cpp
            batbox_permissions batbox_core)
    else()
        batbox_add_unit_test(test_permission_mode
            ${CMAKE_CURRENT_SOURCE_DIR}/test_permission_mode.cpp
            batbox_core)
    endif()
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_history.cpp)
    if(TARGET batbox_repl)
        batbox_add_unit_test(test_history
            ${CMAKE_CURRENT_SOURCE_DIR}/test_history.cpp
            batbox_repl batbox_core)
    else()
        batbox_add_unit_test(test_history
            ${CMAKE_CURRENT_SOURCE_DIR}/test_history.cpp
            batbox_core)
    endif()
endif()
# CPP 2.3 — Autocomplete: file paths, slash commands, @-mentions, agent [[ prefix
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_autocomplete.cpp)
    if(TARGET batbox_repl)
        batbox_add_unit_test(test_autocomplete
            ${CMAKE_CURRENT_SOURCE_DIR}/test_autocomplete.cpp
            batbox_repl batbox_commands batbox_plugins batbox_core)
    else()
        batbox_add_unit_test(test_autocomplete
            ${CMAKE_CURRENT_SOURCE_DIR}/test_autocomplete.cpp
            batbox_commands batbox_plugins batbox_core)
    endif()
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_vimmode.cpp)
    if(TARGET batbox_repl)
        batbox_add_unit_test(test_vimmode
            ${CMAKE_CURRENT_SOURCE_DIR}/test_vimmode.cpp
            batbox_repl batbox_core)
    else()
        batbox_add_unit_test(test_vimmode
            ${CMAKE_CURRENT_SOURCE_DIR}/test_vimmode.cpp
            batbox_core)
    endif()
endif()
# CPP 2.4 upgrade: when batbox_repl is built, re-link test_vim_mode against it
if(TARGET batbox_repl)
    target_link_libraries(test_vim_mode PRIVATE batbox_repl)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_plan_mode.cpp)
    if(TARGET batbox_conversation)
        batbox_add_unit_test(test_plan_mode
            ${CMAKE_CURRENT_SOURCE_DIR}/test_plan_mode.cpp
            batbox_conversation batbox_core)
    else()
        batbox_add_unit_test(test_plan_mode
            ${CMAKE_CURRENT_SOURCE_DIR}/test_plan_mode.cpp
            batbox_core)
    endif()
endif()
# CPP 3.8 — SystemPrompt: compose_system_prompt BATBOX.md merge + @import + plan-mode prefix
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_system_prompt.cpp)
    if(TARGET batbox_conversation)
        batbox_add_unit_test(test_system_prompt
            ${CMAKE_CURRENT_SOURCE_DIR}/test_system_prompt.cpp
            batbox_conversation batbox_core)
    else()
        batbox_add_unit_test(test_system_prompt
            ${CMAKE_CURRENT_SOURCE_DIR}/test_system_prompt.cpp
            batbox_core)
    endif()
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_agent_event.cpp)
    if(TARGET batbox_agents)
        batbox_add_unit_test(test_agent_event
            ${CMAKE_CURRENT_SOURCE_DIR}/test_agent_event.cpp
            batbox_agents batbox_core)
    else()
        batbox_add_unit_test(test_agent_event
            ${CMAKE_CURRENT_SOURCE_DIR}/test_agent_event.cpp
            batbox_core)
    endif()
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_sidecar_state.cpp)
    if(TARGET batbox_sidecar)
        batbox_add_unit_test(test_sidecar_state
            ${CMAKE_CURRENT_SOURCE_DIR}/test_sidecar_state.cpp
            batbox_sidecar batbox_core)
    else()
        batbox_add_unit_test(test_sidecar_state
            ${CMAKE_CURRENT_SOURCE_DIR}/test_sidecar_state.cpp
            batbox_core)
    endif()
endif()

# CPP 7.4 — SidecarManager: spawn / healthcheck / restart / shutdown
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_sidecar_manager.cpp)
    if(TARGET batbox_sidecar)
        batbox_add_unit_test(test_sidecar_manager
            ${CMAKE_CURRENT_SOURCE_DIR}/test_sidecar_manager.cpp
            batbox_sidecar batbox_core)
    else()
        batbox_add_unit_test(test_sidecar_manager
            ${CMAKE_CURRENT_SOURCE_DIR}/test_sidecar_manager.cpp
            batbox_core)
    endif()
endif()


if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_mcp_framing.cpp)
    if(TARGET batbox_mcp)
        batbox_add_unit_test(test_mcp_framing
            ${CMAKE_CURRENT_SOURCE_DIR}/test_mcp_framing.cpp
            batbox_mcp batbox_core)
    else()
        batbox_add_unit_test(test_mcp_framing
            ${CMAKE_CURRENT_SOURCE_DIR}/test_mcp_framing.cpp
            batbox_core)
    endif()
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_message_model.cpp)
    if(TARGET batbox_conversation)
        batbox_add_unit_test(test_message_model
            ${CMAKE_CURRENT_SOURCE_DIR}/test_message_model.cpp
            batbox_conversation batbox_core)
    else()
        batbox_add_unit_test(test_message_model
            ${CMAKE_CURRENT_SOURCE_DIR}/test_message_model.cpp
            batbox_core)
    endif()
endif()

# CPP 5.1 — ITool.hpp + ToolResult.hpp + ToolContext.hpp interfaces
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_tool_interfaces.cpp)
    if(TARGET batbox_tools)
        batbox_add_unit_test(test_tool_interfaces
            ${CMAKE_CURRENT_SOURCE_DIR}/test_tool_interfaces.cpp
            batbox_tools batbox_core)
    else()
        batbox_add_unit_test(test_tool_interfaces
            ${CMAKE_CURRENT_SOURCE_DIR}/test_tool_interfaces.cpp
            batbox_core)
    endif()
endif()


# CPP 5.10 — BashTool ITool adapter unit tests
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_bash_tool_itool.cpp)
    if(TARGET batbox_tools)
        batbox_add_unit_test(test_bash_tool_itool
            ${CMAKE_CURRENT_SOURCE_DIR}/test_bash_tool_itool.cpp
            batbox_tools batbox_tools_bash batbox_permissions batbox_core)
    else()
        batbox_add_unit_test(test_bash_tool_itool
            ${CMAKE_CURRENT_SOURCE_DIR}/test_bash_tool_itool.cpp
            batbox_core)
    endif()
endif()

# CPP 7.1 — ScraplingProto.hpp / .cpp — request/response structs
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_scrapling_proto.cpp)
    if(TARGET batbox_sidecar)
        batbox_add_unit_test(test_scrapling_proto
            ${CMAKE_CURRENT_SOURCE_DIR}/test_scrapling_proto.cpp
            batbox_sidecar batbox_core)
    else()
        batbox_add_unit_test(test_scrapling_proto
            ${CMAKE_CURRENT_SOURCE_DIR}/test_scrapling_proto.cpp
            batbox_core)
    endif()
endif()

# CPP 4.1 — ChatRequest.hpp + ChatResponse.hpp wire models
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_chat_wire_model.cpp)
    if(TARGET batbox_inference)
        batbox_add_unit_test(test_chat_wire_model
            ${CMAKE_CURRENT_SOURCE_DIR}/test_chat_wire_model.cpp
            batbox_inference batbox_core)
    else()
        batbox_add_unit_test(test_chat_wire_model
            ${CMAKE_CURRENT_SOURCE_DIR}/test_chat_wire_model.cpp
            batbox_core)
    endif()
endif()

# CPP 4.3 — ToolCallAccumulator: per-index delta accumulation + finalize()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_tool_call_accumulator.cpp)
    if(TARGET batbox_inference)
        batbox_add_unit_test(test_tool_call_accumulator
            ${CMAKE_CURRENT_SOURCE_DIR}/test_tool_call_accumulator.cpp
            batbox_inference batbox_core)
    else()
        batbox_add_unit_test(test_tool_call_accumulator
            ${CMAKE_CURRENT_SOURCE_DIR}/test_tool_call_accumulator.cpp
            batbox_core)
    endif()
endif()

# CPP 9.1 — SessionFile.hpp/.cpp — per-session JSON shape, gzip, crash recovery
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_session_file.cpp)
    if(TARGET batbox_session)
        batbox_add_unit_test(test_session_file
            ${CMAKE_CURRENT_SOURCE_DIR}/test_session_file.cpp
            batbox_session batbox_core)
    else()
        # Fallback: link zlib directly if batbox_session target isn't built yet
        find_package(ZLIB REQUIRED)
        add_executable(test_session_file
            ${CMAKE_CURRENT_SOURCE_DIR}/test_session_file.cpp
            ${PROJECT_SOURCE_DIR}/src/session/SessionFile.cpp)
        set_target_properties(test_session_file PROPERTIES
            CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
        target_include_directories(test_session_file
            PRIVATE ${PROJECT_SOURCE_DIR}/include)
        target_link_libraries(test_session_file
            PRIVATE doctest::doctest batbox_core ZLIB::ZLIB)
        doctest_discover_tests(test_session_file)
    endif()
endif()

# CPP 11.2 — MarketplaceJson parser
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_marketplace_json.cpp)
    if(TARGET batbox_plugins)
        batbox_add_unit_test(test_marketplace_json
            ${CMAKE_CURRENT_SOURCE_DIR}/test_marketplace_json.cpp
            batbox_plugins batbox_core)
    else()
        batbox_add_unit_test(test_marketplace_json
            ${CMAKE_CURRENT_SOURCE_DIR}/test_marketplace_json.cpp
            batbox_core)
    endif()
endif()

# CPP 11.3 — Plugin struct + PluginRegistry
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_plugin_registry.cpp)
    if(TARGET batbox_plugins)
        batbox_add_unit_test(test_plugin_registry
            ${CMAKE_CURRENT_SOURCE_DIR}/test_plugin_registry.cpp
            batbox_plugins batbox_core)
    else()
        batbox_add_unit_test(test_plugin_registry
            ${CMAKE_CURRENT_SOURCE_DIR}/test_plugin_registry.cpp
            batbox_core)
    endif()
endif()

# CPP 12.2 — PatternMatcher.hpp/.cpp — fnmatch-style + tool-arg matching
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_pattern_matcher.cpp)
    if(TARGET batbox_permissions)
        batbox_add_unit_test(test_pattern_matcher
            ${CMAKE_CURRENT_SOURCE_DIR}/test_pattern_matcher.cpp
            batbox_permissions batbox_core)
    else()
        batbox_add_unit_test(test_pattern_matcher
            ${CMAKE_CURRENT_SOURCE_DIR}/test_pattern_matcher.cpp
            batbox_core)
    endif()
endif()

# CPP 10.2 — Config struct: load_default, load_from_env, load (env>settings_json>defaults),
#             validate, to_json, redacted_for_display
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_config_load.cpp)
    if(TARGET batbox_config)
        batbox_add_unit_test(test_config_load
            ${CMAKE_CURRENT_SOURCE_DIR}/test_config_load.cpp
            batbox_config batbox_core)
    else()
        batbox_add_unit_test(test_config_load
            ${CMAKE_CURRENT_SOURCE_DIR}/test_config_load.cpp
            batbox_core)
    endif()
endif()

# CPP 5.2 — ToolRegistry: register_tool, find_by_name, available_tool_schemas, dispatch
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_tool_registry.cpp)
    if(TARGET batbox_tools)
        batbox_add_unit_test(test_tool_registry
            ${CMAKE_CURRENT_SOURCE_DIR}/test_tool_registry.cpp
            batbox_tools batbox_core)
    else()
        batbox_add_unit_test(test_tool_registry
            ${CMAKE_CURRENT_SOURCE_DIR}/test_tool_registry.cpp
            batbox_core)
    endif()
endif()

# S7 (DIS-979) — ToolSubagentEnvelope: the universal subagent-dispatch seam
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_tool_subagent_envelope.cpp)
    if(TARGET batbox_tools)
        batbox_add_unit_test(test_tool_subagent_envelope
            ${CMAKE_CURRENT_SOURCE_DIR}/test_tool_subagent_envelope.cpp
            batbox_tools batbox_core)
    else()
        batbox_add_unit_test(test_tool_subagent_envelope
            ${CMAKE_CURRENT_SOURCE_DIR}/test_tool_subagent_envelope.cpp
            batbox_core)
    endif()
endif()

# CPP 5.11 — PowerShellTool: no-op on macOS/Linux, returns platform error
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_powershell_stub.cpp)
    if(TARGET batbox_tools)
        batbox_add_unit_test(test_powershell_stub
            ${CMAKE_CURRENT_SOURCE_DIR}/test_powershell_stub.cpp
            batbox_tools batbox_core)
    else()
        batbox_add_unit_test(test_powershell_stub
            ${CMAKE_CURRENT_SOURCE_DIR}/test_powershell_stub.cpp
            batbox_core)
    endif()
endif()

# CPP 5.4 — WriteTool: atomic file write, parent dir creation, unified diff
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_write_tool.cpp)
    if(TARGET batbox_tools)
        batbox_add_unit_test(test_write_tool
            ${CMAKE_CURRENT_SOURCE_DIR}/test_write_tool.cpp
            batbox_tools batbox_core)
    else()
        batbox_add_unit_test(test_write_tool
            ${CMAKE_CURRENT_SOURCE_DIR}/test_write_tool.cpp
            batbox_core)
    endif()
endif()

# CPP 4.4 — UsageTracker + ModelPricing: token accumulation + cost computation
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_usage_tracker.cpp)
    if(TARGET batbox_inference)
        batbox_add_unit_test(test_usage_tracker
            ${CMAKE_CURRENT_SOURCE_DIR}/test_usage_tracker.cpp
            batbox_inference batbox_core)
    else()
        batbox_add_unit_test(test_usage_tracker
            ${CMAKE_CURRENT_SOURCE_DIR}/test_usage_tracker.cpp
            batbox_core)
    endif()
endif()

# CPP 4.7 — Provider quirk handling: resolve_provider_hint + apply_provider_quirks unit tests
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_provider_hint.cpp)
    if(TARGET batbox_inference)
        batbox_add_unit_test(test_provider_hint
            ${CMAKE_CURRENT_SOURCE_DIR}/test_provider_hint.cpp
            batbox_inference batbox_config batbox_core)
    else()
        batbox_add_unit_test(test_provider_hint
            ${CMAKE_CURRENT_SOURCE_DIR}/test_provider_hint.cpp
            batbox_config batbox_core)
    endif()
endif()

# CPP 5.6 — GlobTool: filesystem glob with recursive_directory_iterator
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_glob_tool.cpp)
    if(TARGET batbox_tools)
        batbox_add_unit_test(test_glob_tool
            ${CMAKE_CURRENT_SOURCE_DIR}/test_glob_tool.cpp
            batbox_tools batbox_permissions batbox_core)
    else()
        batbox_add_unit_test(test_glob_tool
            ${CMAKE_CURRENT_SOURCE_DIR}/test_glob_tool.cpp
            batbox_permissions batbox_core)
    endif()
endif()

# CPP 8.3 — IMcpTransport interface + MockTransport unit tests
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_mcp_mock_transport.cpp)
    if(TARGET batbox_mcp)
        batbox_add_unit_test(test_mcp_mock_transport
            ${CMAKE_CURRENT_SOURCE_DIR}/test_mcp_mock_transport.cpp
            batbox_mcp batbox_core)
    else()
        batbox_add_unit_test(test_mcp_mock_transport
            ${CMAKE_CURRENT_SOURCE_DIR}/test_mcp_mock_transport.cpp
            batbox_core)
    endif()
endif()

# CPP 5.25 — SnipTool: named snippet save/load/list/delete to ~/.batbox/snippets/
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_snip_tool.cpp)
    if(TARGET batbox_tools)
        batbox_add_unit_test(test_snip_tool
            ${CMAKE_CURRENT_SOURCE_DIR}/test_snip_tool.cpp
            batbox_tools batbox_core)
    else()
        batbox_add_unit_test(test_snip_tool
            ${CMAKE_CURRENT_SOURCE_DIR}/test_snip_tool.cpp
            batbox_core)
    endif()
endif()

# CPP 10.3 — SettingsLoader: load/write ~/.batbox/settings.json, atomic write, defaults
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_settings_loader.cpp)
    if(TARGET batbox_config)
        batbox_add_unit_test(test_settings_loader
            ${CMAKE_CURRENT_SOURCE_DIR}/test_settings_loader.cpp
            batbox_config batbox_core)
    else()
        batbox_add_unit_test(test_settings_loader
            ${CMAKE_CURRENT_SOURCE_DIR}/test_settings_loader.cpp
            batbox_core)
    endif()
endif()

# CPP 10.5 — McpConfig: load_mcp_config (stdio/sse/http/ws, ${env:NAME} expansion, per-entry errors)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_mcp_config.cpp)
    if(TARGET batbox_config)
        batbox_add_unit_test(test_mcp_config
            ${CMAKE_CURRENT_SOURCE_DIR}/test_mcp_config.cpp
            batbox_config batbox_core)
    else()
        batbox_add_unit_test(test_mcp_config
            ${CMAKE_CURRENT_SOURCE_DIR}/test_mcp_config.cpp
            batbox_core)
    endif()
endif()

# CPP 10.6 — KeybindingsConfig: load_keybindings, default_keybindings, overlay, warnings
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_keybindings_config.cpp)
    if(TARGET batbox_config)
        batbox_add_unit_test(test_keybindings_config
            ${CMAKE_CURRENT_SOURCE_DIR}/test_keybindings_config.cpp
            batbox_config batbox_core)
    else()
        batbox_add_unit_test(test_keybindings_config
            ${CMAKE_CURRENT_SOURCE_DIR}/test_keybindings_config.cpp
            batbox_core)
    endif()
endif()

# CPP 5.14 — ToolSearchTool: fuzzy/substring search over ToolRegistry
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_tool_search.cpp)
    if(TARGET batbox_tools)
        batbox_add_unit_test(test_tool_search
            ${CMAKE_CURRENT_SOURCE_DIR}/test_tool_search.cpp
            batbox_tools batbox_permissions batbox_core)
    else()
        batbox_add_unit_test(test_tool_search
            ${CMAKE_CURRENT_SOURCE_DIR}/test_tool_search.cpp
            batbox_permissions batbox_core)
    endif()
endif()

# CPP 1.1 — Theme struct + ThemeRole + color_for() + theme_from_name() + load_theme()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_theme.cpp)
    if(TARGET batbox_theme)
        batbox_add_unit_test(test_theme
            ${CMAKE_CURRENT_SOURCE_DIR}/test_theme.cpp
            batbox_theme batbox_core)
    else()
        batbox_add_unit_test(test_theme
            ${CMAKE_CURRENT_SOURCE_DIR}/test_theme.cpp
            batbox_core)
    endif()
endif()

# CPP 1.2 + TUI-FLOW-T4 — Splash (legacy ASCII bat) + SplashBanner (Claude-Code-style box)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_splash.cpp)
    if(TARGET batbox_tui)
        batbox_add_unit_test(test_splash
            ${CMAKE_CURRENT_SOURCE_DIR}/test_splash.cpp
            batbox_tui batbox_theme batbox_core)
    else()
        batbox_add_unit_test(test_splash
            ${CMAKE_CURRENT_SOURCE_DIR}/test_splash.cpp
            batbox_theme batbox_core)
    endif()
endif()

# TUI-FIX-T8 — SplashBanner account fallback: resolve_account_label + set_email
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_splash_account_fallback.cpp)
    if(TARGET batbox_tui)
        batbox_add_unit_test(test_splash_account_fallback
            ${CMAKE_CURRENT_SOURCE_DIR}/test_splash_account_fallback.cpp
            batbox_tui batbox_theme batbox_core)
    else()
        batbox_add_unit_test(test_splash_account_fallback
            ${CMAKE_CURRENT_SOURCE_DIR}/test_splash_account_fallback.cpp
            batbox_theme batbox_core)
    endif()
endif()

# CPP 2.5 — Keybindings: defaults + override loader + event_to_action dispatch
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_keybindings.cpp)
    if(TARGET batbox_repl)
        batbox_add_unit_test(test_keybindings
            ${CMAKE_CURRENT_SOURCE_DIR}/test_keybindings.cpp
            batbox_repl batbox_config batbox_core)
    else()
        batbox_add_unit_test(test_keybindings
            ${CMAKE_CURRENT_SOURCE_DIR}/test_keybindings.cpp
            batbox_config batbox_core)
    endif()
endif()

# CPP 12.3 — PermissionRule struct + PermissionStore: load/persist allow/deny/ask rules
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_permission_store.cpp)
    if(TARGET batbox_permissions)
        batbox_add_unit_test(test_permission_store
            ${CMAKE_CURRENT_SOURCE_DIR}/test_permission_store.cpp
            batbox_permissions batbox_config batbox_core)
    else()
        batbox_add_unit_test(test_permission_store
            ${CMAKE_CURRENT_SOURCE_DIR}/test_permission_store.cpp
            batbox_core)
    endif()
endif()

# CPP 1.3 — MarkdownRenderer: incremental markdown -> FTXUI Element
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_markdown_render.cpp)
    if(TARGET batbox_tui)
        batbox_add_unit_test(test_markdown_render
            ${CMAKE_CURRENT_SOURCE_DIR}/test_markdown_render.cpp
            batbox_tui batbox_theme batbox_core)
    else()
        batbox_add_unit_test(test_markdown_render
            ${CMAKE_CURRENT_SOURCE_DIR}/test_markdown_render.cpp
            batbox_theme batbox_core)
    endif()
endif()

# CPP 1.9 — InputBar: prompt + status line + slash palette overlay + autocomplete
# TUI-FLOW-T6: batbox_perf is also linked because InputBar.cpp includes PerfSnapshot.hpp
#              (added by TUI-FLOW-T3 perf HUD).
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_input_bar.cpp)
    if(TARGET batbox_tui)
        batbox_add_unit_test(test_input_bar
            ${CMAKE_CURRENT_SOURCE_DIR}/test_input_bar.cpp
            batbox_tui batbox_repl batbox_theme batbox_perf batbox_core)
    else()
        batbox_add_unit_test(test_input_bar
            ${CMAKE_CURRENT_SOURCE_DIR}/test_input_bar.cpp
            batbox_repl batbox_theme batbox_perf batbox_core)
    endif()
endif()

# CPP 5.16 — TaskStore + TaskCreate/TaskList/TaskGet/TaskUpdate tools
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_task_crud.cpp)
    if(TARGET batbox_tools)
        batbox_add_unit_test(test_task_crud
            ${CMAKE_CURRENT_SOURCE_DIR}/test_task_crud.cpp
            batbox_tools batbox_core)
    else()
        batbox_add_unit_test(test_task_crud
            ${CMAKE_CURRENT_SOURCE_DIR}/test_task_crud.cpp
            batbox_core)
    endif()
endif()

# CPP 1.7 — ChatView: append-only scrollable message-history Component
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_chat_view.cpp)
    if(TARGET batbox_tui)
        batbox_add_unit_test(test_chat_view
            ${CMAKE_CURRENT_SOURCE_DIR}/test_chat_view.cpp
            batbox_tui batbox_conversation batbox_theme batbox_core)
    else()
        batbox_add_unit_test(test_chat_view
            ${CMAKE_CURRENT_SOURCE_DIR}/test_chat_view.cpp
            batbox_theme batbox_core)
    endif()
endif()

# CPP 1.8 — StreamingMessageView: SSE token streaming render with shared_mutex
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_streaming_message_view.cpp)
    if(TARGET batbox_tui)
        batbox_add_unit_test(test_streaming_message_view
            ${CMAKE_CURRENT_SOURCE_DIR}/test_streaming_message_view.cpp
            batbox_tui batbox_theme batbox_core)
    else()
        batbox_add_unit_test(test_streaming_message_view
            ${CMAKE_CURRENT_SOURCE_DIR}/test_streaming_message_view.cpp
            batbox_theme batbox_core)
    endif()
endif()

# CPP 12.5 — PermissionGate: unit tests for all 4 modes + mode switching
# Links batbox_permissions (PermissionGate + PermissionMode + PermissionRule +
# PatternMatcher + PermissionStore) and batbox_config (SettingsLoader) so that
# all permission-layer symbols resolve without manually listing .cpp files.
# simdjson is linked explicitly because batbox_config is an OBJECT library and
# its simdjson dependency does not propagate transitively to test executables.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_permission_gate.cpp)
    if(TARGET batbox_permissions AND TARGET batbox_config)
        batbox_add_unit_test(test_permission_gate
            ${CMAKE_CURRENT_SOURCE_DIR}/test_permission_gate.cpp
            batbox_permissions batbox_config batbox_core)
        target_link_libraries(test_permission_gate PRIVATE simdjson::simdjson)
    elseif(TARGET batbox_permissions)
        batbox_add_unit_test(test_permission_gate
            ${CMAKE_CURRENT_SOURCE_DIR}/test_permission_gate.cpp
            batbox_permissions batbox_core)
        target_link_libraries(test_permission_gate PRIVATE simdjson::simdjson)
    else()
        batbox_add_unit_test(test_permission_gate
            ${CMAKE_CURRENT_SOURCE_DIR}/test_permission_gate.cpp
            batbox_core)
        target_link_libraries(test_permission_gate PRIVATE simdjson::simdjson)
    endif()
endif()

# CPP 5.17 — CronScheduler + CronCreateTool / CronDeleteTool / CronListTool
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_cron_expr.cpp)
    if(TARGET batbox_tools)
        batbox_add_unit_test(test_cron_expr
            ${CMAKE_CURRENT_SOURCE_DIR}/test_cron_expr.cpp
            batbox_tools batbox_core)
    else()
        batbox_add_unit_test(test_cron_expr
            ${CMAKE_CURRENT_SOURCE_DIR}/test_cron_expr.cpp
            batbox_core)
    endif()
endif()

# CPP 6.2 — AgentSpec: frontmatter-driven agent config, from_file, from_type
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_agent_spec.cpp)
    if(TARGET batbox_agents AND TARGET batbox_plugins)
        batbox_add_unit_test(test_agent_spec
            ${CMAKE_CURRENT_SOURCE_DIR}/test_agent_spec.cpp
            batbox_agents batbox_plugins batbox_core)
    elseif(TARGET batbox_plugins)
        batbox_add_unit_test(test_agent_spec
            ${CMAKE_CURRENT_SOURCE_DIR}/test_agent_spec.cpp
            batbox_plugins batbox_core)
    else()
        batbox_add_unit_test(test_agent_spec
            ${CMAKE_CURRENT_SOURCE_DIR}/test_agent_spec.cpp
            batbox_core)
    endif()
endif()

# Fix #16 — tools[] serialisation in /v1/chat/completions request bodies.
# Tests that a populated ToolRegistry produces a "tools" array and
# "tool_choice":"auto" in the serialised ChatRequest, and that an empty
# or null registry omits both fields.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_openai_client_tools.cpp)
    if(TARGET batbox_tools AND TARGET batbox_inference)
        batbox_add_unit_test(test_openai_client_tools
            ${CMAKE_CURRENT_SOURCE_DIR}/test_openai_client_tools.cpp
            batbox_tools batbox_inference batbox_permissions batbox_core)
        target_link_libraries(test_openai_client_tools PRIVATE simdjson::simdjson)
    elseif(TARGET batbox_tools)
        batbox_add_unit_test(test_openai_client_tools
            ${CMAKE_CURRENT_SOURCE_DIR}/test_openai_client_tools.cpp
            batbox_tools batbox_core)
        target_link_libraries(test_openai_client_tools PRIVATE simdjson::simdjson)
    else()
        batbox_add_unit_test(test_openai_client_tools
            ${CMAKE_CURRENT_SOURCE_DIR}/test_openai_client_tools.cpp
            batbox_core)
        target_link_libraries(test_openai_client_tools PRIVATE simdjson::simdjson)
    endif()
endif()
# Task 23 — Model alias resolution: resolve_model_alias unit tests
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_model_alias.cpp)
    if(TARGET batbox_config)
        batbox_add_unit_test(test_model_alias
            ${CMAKE_CURRENT_SOURCE_DIR}/test_model_alias.cpp
            batbox_config batbox_core)
    else()
        batbox_add_unit_test(test_model_alias
            ${CMAKE_CURRENT_SOURCE_DIR}/test_model_alias.cpp
            batbox_core)
    endif()
endif()

# Task 24 — sidecar python_bin auto-resolution from venv at config load time
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_sidecar_config.cpp)
    if(TARGET batbox_config)
        batbox_add_unit_test(test_sidecar_config
            ${CMAKE_CURRENT_SOURCE_DIR}/test_sidecar_config.cpp
            batbox_config batbox_core)
    else()
        batbox_add_unit_test(test_sidecar_config
            ${CMAKE_CURRENT_SOURCE_DIR}/test_sidecar_config.cpp
            batbox_core)
    endif()
endif()

# TUI-T11 — UI-D9: token_received stays false during reasoning phase.
# Tests that StreamDelta::from_json deserialises reasoning_content chunks
# correctly and that the stream_started retry gate fires on reasoning-only
# deltas while token_received (telemetry) remains content-only.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_client_reasoning.cpp)
    if(TARGET batbox_inference)
        batbox_add_unit_test(test_client_reasoning
            ${CMAKE_CURRENT_SOURCE_DIR}/test_client_reasoning.cpp
            batbox_inference batbox_core)
    else()
        batbox_add_unit_test(test_client_reasoning
            ${CMAKE_CURRENT_SOURCE_DIR}/test_client_reasoning.cpp
            batbox_core)
    endif()
endif()

# TUI-ASKQ-T2 — QuestionCard: renderer (header chip, option rows, footer hints)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_question_card.cpp)
    if(TARGET batbox_tui)
        batbox_add_unit_test(test_question_card
            ${CMAKE_CURRENT_SOURCE_DIR}/test_question_card.cpp
            batbox_tui batbox_theme batbox_core)
    else()
        batbox_add_unit_test(test_question_card
            ${CMAKE_CURRENT_SOURCE_DIR}/test_question_card.cpp
            batbox_theme batbox_core)
    endif()
endif()

# TUI-FLOW-T10 — Changelog parser + disk loader
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_changelog.cpp)
    if(TARGET batbox_tui)
        batbox_add_unit_test(test_changelog
            ${CMAKE_CURRENT_SOURCE_DIR}/test_changelog.cpp
            batbox_tui batbox_core)
    else()
        batbox_add_unit_test(test_changelog
            ${CMAKE_CURRENT_SOURCE_DIR}/test_changelog.cpp
            batbox_core)
    endif()
endif()

# TUI-FLOW-T10 — StateStore: state.json read/write
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_state_store.cpp)
    if(TARGET batbox_session AND TARGET batbox_config)
        batbox_add_unit_test(test_state_store
            ${CMAKE_CURRENT_SOURCE_DIR}/test_state_store.cpp
            batbox_session batbox_config batbox_core)
    elseif(TARGET batbox_config)
        batbox_add_unit_test(test_state_store
            ${CMAKE_CURRENT_SOURCE_DIR}/test_state_store.cpp
            batbox_config batbox_core)
    else()
        batbox_add_unit_test(test_state_store
            ${CMAKE_CURRENT_SOURCE_DIR}/test_state_store.cpp
            batbox_core)
    endif()
endif()

# TUI-FLOW-T11 — McpStatusPoller: polls MCP registry and fires on_change callback
# McpStatusPoller.cpp is compiled into the batbox executable, not a library target.
# Include it directly in the test executable so it links without a separate library.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_mcp_status_poller.cpp)
    add_executable(test_mcp_status_poller
        ${CMAKE_CURRENT_SOURCE_DIR}/test_mcp_status_poller.cpp
        ${PROJECT_SOURCE_DIR}/src/app/McpStatusPoller.cpp)
    set_target_properties(test_mcp_status_poller PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_include_directories(test_mcp_status_poller
        PRIVATE ${PROJECT_SOURCE_DIR}/include)
    if(TARGET batbox_mcp)
        target_link_libraries(test_mcp_status_poller
            PRIVATE doctest::doctest batbox_mcp batbox_core)
    else()
        target_link_libraries(test_mcp_status_poller
            PRIVATE doctest::doctest batbox_core)
    endif()
    doctest_discover_tests(test_mcp_status_poller)
endif()

# TUI-FIX-T4 — InputBar segment-model: paste chip, bracketed paste, Shift+Enter newline
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_input_bar_segments.cpp)
    if(TARGET batbox_tui)
        batbox_add_unit_test(test_input_bar_segments
            ${CMAKE_CURRENT_SOURCE_DIR}/test_input_bar_segments.cpp
            batbox_tui batbox_repl batbox_theme batbox_perf batbox_core)
    else()
        batbox_add_unit_test(test_input_bar_segments
            ${CMAKE_CURRENT_SOURCE_DIR}/test_input_bar_segments.cpp
            batbox_repl batbox_theme batbox_perf batbox_core)
    endif()
endif()

# TUI-FIX-T10 — InputBar input-queue (Codex hybrid): all 10 edge cases + G1/G10
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_input_queue.cpp)
    if(TARGET batbox_tui)
        batbox_add_unit_test(test_input_queue
            ${CMAKE_CURRENT_SOURCE_DIR}/test_input_queue.cpp
            batbox_tui batbox_repl batbox_theme batbox_perf batbox_core)
    else()
        batbox_add_unit_test(test_input_queue
            ${CMAKE_CURRENT_SOURCE_DIR}/test_input_queue.cpp
            batbox_repl batbox_theme batbox_perf batbox_core)
    endif()
endif()

# PEXT2 1.4c — HTML manual lexer: tags, attrs, comments, DOCTYPE, entities
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_html_lexer.cpp)
    if(TARGET batbox_tui)
        batbox_add_unit_test(test_html_lexer
            ${CMAKE_CURRENT_SOURCE_DIR}/test_html_lexer.cpp
            batbox_tui batbox_core)
    else()
        batbox_add_unit_test(test_html_lexer
            ${CMAKE_CURRENT_SOURCE_DIR}/test_html_lexer.cpp
            batbox_core)
    endif()
endif()

# PEXT2 1.4e — JSON manual lexer: strings, numbers, keywords, structural punctuation, JSON5
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_json_lexer.cpp)
    if(TARGET batbox_tui)
        batbox_add_unit_test(test_json_lexer
            ${CMAKE_CURRENT_SOURCE_DIR}/test_json_lexer.cpp
            batbox_tui batbox_core)
    else()
        batbox_add_unit_test(test_json_lexer
            ${CMAKE_CURRENT_SOURCE_DIR}/test_json_lexer.cpp
            batbox_core)
    endif()
endif()

# PEXT2 1.4b — TypeScript manual lexer: keywords, type annotations, generics, TSX tags
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_ts_lexer.cpp)
    if(TARGET batbox_tui)
        batbox_add_unit_test(test_ts_lexer
            ${CMAKE_CURRENT_SOURCE_DIR}/test_ts_lexer.cpp
            batbox_tui batbox_core)
    else()
        # batbox_tui not yet built — compile ts_lexer.cpp directly into the test
        add_executable(test_ts_lexer
            ${CMAKE_CURRENT_SOURCE_DIR}/test_ts_lexer.cpp
            ${PROJECT_SOURCE_DIR}/src/tui/manual_lexers/ts_lexer.cpp)
        set_target_properties(test_ts_lexer PROPERTIES
            CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
        target_include_directories(test_ts_lexer
            PRIVATE ${PROJECT_SOURCE_DIR}/include
                    ${PROJECT_SOURCE_DIR}/src/tui/manual_lexers)
        target_link_libraries(test_ts_lexer
            PRIVATE doctest::doctest)
        doctest_discover_tests(test_ts_lexer)
    endif()
endif()

# PEXT2 3.1 — D-1: per-turn JSON dump collapse (one dump per turn)
# Tests that the preflight dump body string is byte-identical to the wire body
# that Client::stream_chat would build, validating the PEXT2 3.1 caching strategy.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_run_turn_single_dump.cpp)
    if(TARGET batbox_inference AND TARGET batbox_conversation)
        batbox_add_unit_test(test_run_turn_single_dump
            ${CMAKE_CURRENT_SOURCE_DIR}/test_run_turn_single_dump.cpp
            batbox_inference batbox_conversation batbox_core)
    elseif(TARGET batbox_inference)
        batbox_add_unit_test(test_run_turn_single_dump
            ${CMAKE_CURRENT_SOURCE_DIR}/test_run_turn_single_dump.cpp
            batbox_inference batbox_core)
    else()
        batbox_add_unit_test(test_run_turn_single_dump
            ${CMAKE_CURRENT_SOURCE_DIR}/test_run_turn_single_dump.cpp
            batbox_core)
    endif()
endif()
