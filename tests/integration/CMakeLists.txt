# =============================================================================
# tests/integration/CMakeLists.txt — batbox integration tests
#
# Integration tests exercise multiple components together and may require
# filesystem access but should not need network or a running sidecar.
#
# Convention: add integration test executables here using a local variant of
# batbox_add_unit_test() with the test source linked against the specific
# source files under test (rather than a full library target), so that
# integration tests build even when upstream component targets are stubs.
# =============================================================================

if(NOT TARGET doctest::doctest)
    find_package(doctest CONFIG QUIET)
endif()

if(NOT TARGET doctest::doctest)
    message(STATUS "  doctest not found — integration test targets skipped")
    return()
endif()

# ---------------------------------------------------------------------------
# CPP A.2 — migrate subcommand integration test
#
# Links migrate.cpp + Logging (for BATBOX_LOG_* macros) directly into the
# test executable.  Does not require a full batbox link.
# ---------------------------------------------------------------------------
add_executable(test_migrate
    ${CMAKE_CURRENT_SOURCE_DIR}/test_migrate.cpp
    ${PROJECT_SOURCE_DIR}/src/migrate.cpp
)

set_target_properties(test_migrate PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

target_include_directories(test_migrate
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src          # for migrate.hpp
        ${PROJECT_SOURCE_DIR}/include      # for batbox/core/Logging.hpp etc.
)

target_link_libraries(test_migrate
    PRIVATE
        doctest::doctest
        batbox_core
)

include(doctest)
doctest_discover_tests(test_migrate)

# ---------------------------------------------------------------------------
# CPP 11.4 — SkillLoader integration test
#
# Links FrontmatterParser.cpp + SkillLoader.cpp directly so this test builds
# even when the full batbox_plugins library target is not yet assembled.
# ---------------------------------------------------------------------------
add_executable(test_skill_loader
    ${CMAKE_CURRENT_SOURCE_DIR}/test_skill_loader.cpp
    ${PROJECT_SOURCE_DIR}/src/plugins/FrontmatterParser.cpp
    ${PROJECT_SOURCE_DIR}/src/plugins/SkillLoader.cpp
)

set_target_properties(test_skill_loader PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

target_include_directories(test_skill_loader
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(test_skill_loader
    PRIVATE
        doctest::doctest
        batbox_core
        simdjson::simdjson
)

doctest_discover_tests(test_skill_loader)

# ---------------------------------------------------------------------------
# CPP 5.19 — SkillTool integration test
#
# Links SkillTool.cpp + FrontmatterParser.cpp + SkillLoader.cpp + CancelToken.cpp
# directly so this test builds without the full batbox_tools/batbox_plugins
# OBJECT library targets needing to be assembled.
# ---------------------------------------------------------------------------
add_executable(test_skill_tool
    ${CMAKE_CURRENT_SOURCE_DIR}/test_skill_tool.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/SkillTool.cpp
    ${PROJECT_SOURCE_DIR}/src/plugins/FrontmatterParser.cpp
    ${PROJECT_SOURCE_DIR}/src/plugins/SkillLoader.cpp
)

set_target_properties(test_skill_tool PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

target_include_directories(test_skill_tool
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(test_skill_tool
    PRIVATE
        doctest::doctest
        batbox_core
        simdjson::simdjson
)

doctest_discover_tests(test_skill_tool)

# ---------------------------------------------------------------------------
# CPP 5.8 / CPP 5.9 — BashRunner integration tests
#
# Links BashRunner.cpp, PtyBackend.cpp, PipesBackend.cpp, EnvScrub.cpp,
# and AnsiStrip.cpp directly.  CancelToken.cpp is provided via batbox_core.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_bash_tool.cpp)

    add_executable(test_bash_tool
        ${CMAKE_CURRENT_SOURCE_DIR}/test_bash_tool.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/bash/BashRunner.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/bash/PtyBackend.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/bash/PipesBackend.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/bash/EnvScrub.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/bash/AnsiStrip.cpp
    )

    set_target_properties(test_bash_tool PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_bash_tool
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${PROJECT_SOURCE_DIR}/src/tools/bash  # for *Internal.hpp
    )

    target_link_libraries(test_bash_tool
        PRIVATE
            doctest::doctest
            batbox_core
    )

    # macOS ships forkpty in libc; Linux needs -lutil
    if(UNIX AND NOT APPLE)
        target_link_libraries(test_bash_tool PRIVATE util)
    endif()

    include(doctest)
    doctest_discover_tests(test_bash_tool)

endif()

# ---------------------------------------------------------------------------
# CPP 11.6 — CommandLoader integration test
#
# Links FrontmatterParser.cpp, CommandLoader.cpp, and SlashCommandRegistry.cpp
# directly so this test builds even when the full batbox_plugins library target
# is not yet fully assembled.  Also links Paths.cpp (needed by load_user_commands).
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_plugin_command_loader.cpp)

    add_executable(test_plugin_command_loader
        ${CMAKE_CURRENT_SOURCE_DIR}/test_plugin_command_loader.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/FrontmatterParser.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/CommandLoader.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/SlashCommandRegistry.cpp
    )

    set_target_properties(test_plugin_command_loader PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_plugin_command_loader
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    # find simdjson for FrontmatterParser (Json.cpp parse_fast path)
    find_package(simdjson CONFIG QUIET)
    find_package(nlohmann_json CONFIG QUIET)
    find_package(fmt CONFIG QUIET)

    target_link_libraries(test_plugin_command_loader
        PRIVATE
            doctest::doctest
            batbox_core
            $<$<TARGET_EXISTS:simdjson::simdjson>:simdjson::simdjson>
            $<$<TARGET_EXISTS:nlohmann_json::nlohmann_json>:nlohmann_json::nlohmann_json>
            $<$<TARGET_EXISTS:fmt::fmt>:fmt::fmt>
    )

    doctest_discover_tests(test_plugin_command_loader)

endif()

# ---------------------------------------------------------------------------
# CPP 11.7 — PluginLoader integration test
#
# Links all plugin components directly so this test builds even when the full
# batbox_plugins library target is not yet fully assembled.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_plugin_loader.cpp)

    add_executable(test_plugin_loader
        ${CMAKE_CURRENT_SOURCE_DIR}/test_plugin_loader.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/FrontmatterParser.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/MarketplaceJson.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/SkillLoader.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/AgentLoader.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/CommandLoader.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/PluginRegistry.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/PluginLoader.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/SlashCommandRegistry.cpp
    )

    set_target_properties(test_plugin_loader PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_plugin_loader
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    # Expose fixture directory so the test can find sample_plugins/.
    target_compile_definitions(test_plugin_loader
        PRIVATE
            BATBOX_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures"
    )

    find_package(simdjson CONFIG QUIET)
    find_package(nlohmann_json CONFIG QUIET)
    find_package(fmt CONFIG QUIET)

    target_link_libraries(test_plugin_loader
        PRIVATE
            doctest::doctest
            batbox_core
            batbox_config
            $<$<TARGET_EXISTS:simdjson::simdjson>:simdjson::simdjson>
            $<$<TARGET_EXISTS:nlohmann_json::nlohmann_json>:nlohmann_json::nlohmann_json>
            $<$<TARGET_EXISTS:fmt::fmt>:fmt::fmt>
    )

    doctest_discover_tests(test_plugin_loader)

endif()

# ---------------------------------------------------------------------------
# CPP 11.8 — PluginCmd integration test
#
# Links PluginCmd.cpp + all plugin components + SlashCommandRegistry.cpp +
# SettingsLoader.cpp directly so this test builds independently.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_plugin_cmd.cpp)

    add_executable(test_plugin_cmd
        ${CMAKE_CURRENT_SOURCE_DIR}/test_plugin_cmd.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/PluginCmd.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/SlashCommandRegistry.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/FrontmatterParser.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/MarketplaceJson.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/SkillLoader.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/AgentLoader.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/CommandLoader.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/PluginRegistry.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/PluginLoader.cpp
        ${PROJECT_SOURCE_DIR}/src/config/SettingsLoader.cpp
    )

    set_target_properties(test_plugin_cmd PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_plugin_cmd
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    find_package(simdjson CONFIG QUIET)
    find_package(nlohmann_json CONFIG QUIET)
    find_package(fmt CONFIG QUIET)

    target_link_libraries(test_plugin_cmd
        PRIVATE
            doctest::doctest
            batbox_core
            $<$<TARGET_EXISTS:simdjson::simdjson>:simdjson::simdjson>
            $<$<TARGET_EXISTS:nlohmann_json::nlohmann_json>:nlohmann_json::nlohmann_json>
            $<$<TARGET_EXISTS:fmt::fmt>:fmt::fmt>
    )

    doctest_discover_tests(test_plugin_cmd)

endif()

# ---------------------------------------------------------------------------
# CPP T.6 — PluginLoader end-to-end integration test against fixture plugins
#
# Exercises the full loading pipeline with realistic multi-plugin fixture trees:
#   Scenario A  — shared skill name across two plugins (namespace prefix)
#   Scenario A2 — shared agent name across two plugins (namespace prefix)
#   Scenario B  — dual-layout .claude-plugin/.batbox-plugin probe order
#   Scenario C  — disabled-list round-trip via reload
#   Scenario D  — add_local / remove cycles
#   Scenario E  — mixed marketplace (5 plugins, varied assets)
#   Scenario F  — in-flight snapshot before/after reload
#   Scenario G  — load from tests/fixtures/sample_plugins_e2e/
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_plugin_loader_e2e.cpp)

    add_executable(test_plugin_loader_e2e
        ${CMAKE_CURRENT_SOURCE_DIR}/test_plugin_loader_e2e.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/FrontmatterParser.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/MarketplaceJson.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/SkillLoader.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/AgentLoader.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/CommandLoader.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/PluginRegistry.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/PluginLoader.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/SlashCommandRegistry.cpp
    )

    set_target_properties(test_plugin_loader_e2e PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_plugin_loader_e2e
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    # Expose fixture directory so Scenario G can locate sample_plugins_e2e/.
    target_compile_definitions(test_plugin_loader_e2e
        PRIVATE
            BATBOX_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures"
    )

    find_package(simdjson CONFIG QUIET)
    find_package(nlohmann_json CONFIG QUIET)
    find_package(fmt CONFIG QUIET)

    target_link_libraries(test_plugin_loader_e2e
        PRIVATE
            doctest::doctest
            batbox_core
            batbox_config
            $<$<TARGET_EXISTS:simdjson::simdjson>:simdjson::simdjson>
            $<$<TARGET_EXISTS:nlohmann_json::nlohmann_json>:nlohmann_json::nlohmann_json>
            $<$<TARGET_EXISTS:fmt::fmt>:fmt::fmt>
    )

    doctest_discover_tests(test_plugin_loader_e2e)

endif()

# ---------------------------------------------------------------------------
# CPP 4.5 — Client::chat() non-streaming integration smoke test
#
# Spawns tests/fixtures/fake_openai_server.py as a child process and
# exercises Client::chat() against it.  Requires Python 3 on PATH.
# Links Client.cpp + ChatRequest.cpp directly so we don't depend on the
# full batbox_inference OBJECT library being wired into the build yet.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_inference_smoke_openai_compat.cpp)

    add_executable(test_inference_smoke_openai_compat
        ${CMAKE_CURRENT_SOURCE_DIR}/test_inference_smoke_openai_compat.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/Client.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/ChatRequest.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/SseParser.cpp
    )

    set_target_properties(test_inference_smoke_openai_compat PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_inference_smoke_openai_compat
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    # Expose the fixtures directory at compile time so find_fixture_script() works.
    target_compile_definitions(test_inference_smoke_openai_compat
        PRIVATE
            BATBOX_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures"
    )

    target_link_libraries(test_inference_smoke_openai_compat
        PRIVATE
            doctest::doctest
            batbox_config
            batbox_core
            cpr::cpr
            simdjson::simdjson
    )

    doctest_discover_tests(test_inference_smoke_openai_compat)

endif()

# ---------------------------------------------------------------------------
# CPP 5.22 — RemoteTriggerTool integration test
#
# Links RemoteTriggerTool.cpp + PatternMatcher.cpp directly against an
# in-process ix::HttpServer fixture so no external network is required.
# The test validates: POST with auth headers, non-2xx error excerpt,
# URL allowlist glob check, payload serialisation, cancellation.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_remote_trigger.cpp)

    add_executable(test_remote_trigger
        ${CMAKE_CURRENT_SOURCE_DIR}/test_remote_trigger.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/RemoteTriggerTool.cpp
    )

    set_target_properties(test_remote_trigger PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_remote_trigger
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    target_link_libraries(test_remote_trigger
        PRIVATE
            doctest::doctest
            batbox_core
            batbox_config
            batbox_permissions
            cpr::cpr
            ixwebsocket::ixwebsocket
            simdjson::simdjson
    )

    doctest_discover_tests(test_remote_trigger)

endif()

# ---------------------------------------------------------------------------
# CPP 5.7 — GrepTool integration test
#
# Exercises GrepTool::run() against a temp fixture tree and the repo's own
# source directory.  Links GrepTool.cpp directly so the test builds even
# when the full batbox_tools OBJECT library is not yet wired.
#
# BATBOX_PROJECT_SOURCE_DIR is injected so test_grep_glob.cpp can locate
# the repo's include/ and src/ directories for the live fixture tests (AC6).
# ---------------------------------------------------------------------------
add_executable(test_grep_glob
    ${CMAKE_CURRENT_SOURCE_DIR}/test_grep_glob.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/GrepTool.cpp
)

set_target_properties(test_grep_glob PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

target_include_directories(test_grep_glob
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_compile_definitions(test_grep_glob
    PRIVATE
        BATBOX_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

target_link_libraries(test_grep_glob
    PRIVATE
        doctest::doctest
        batbox_core
        batbox_config
        batbox_permissions
        simdjson::simdjson
)

doctest_discover_tests(test_grep_glob)

# ---------------------------------------------------------------------------
# CPP 3.5 — Compactor integration test
#
# Spawns tests/fixtures/fake_openai_server.py and exercises Compactor::compact()
# through Client::chat() against it.  Links Compactor.cpp, Client.cpp,
# ChatRequest.cpp, Message.cpp, SseParser.cpp, CancelToken.cpp directly so the test builds even when
# the full batbox_conversation OBJECT library is not yet fully assembled.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_compactor.cpp)

    add_executable(test_compactor
        ${CMAKE_CURRENT_SOURCE_DIR}/test_compactor.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Compactor.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Message.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/Client.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/ChatRequest.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/SseParser.cpp
    )

    set_target_properties(test_compactor PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_compactor
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    target_compile_definitions(test_compactor
        PRIVATE
            BATBOX_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures"
    )

    target_link_libraries(test_compactor
        PRIVATE
            doctest::doctest
            batbox_config
            batbox_core
            cpr::cpr
            simdjson::simdjson
    )

    doctest_discover_tests(test_compactor)

endif()

# ---------------------------------------------------------------------------
# CPP 10.7 — ConfigReload integration test
#
# Links Config.cpp, ConfigReload.cpp, EnvLoader.cpp, SettingsLoader.cpp
# directly so the test builds even without a full batbox_config OBJECT
# library being wired into the executable.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_config_reload.cpp)

    add_executable(test_config_reload
        ${CMAKE_CURRENT_SOURCE_DIR}/test_config_reload.cpp
        ${PROJECT_SOURCE_DIR}/src/config/Config.cpp
        ${PROJECT_SOURCE_DIR}/src/config/ConfigReload.cpp
        ${PROJECT_SOURCE_DIR}/src/config/EnvLoader.cpp
        ${PROJECT_SOURCE_DIR}/src/config/SettingsLoader.cpp
    )

    set_target_properties(test_config_reload PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_config_reload
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    target_link_libraries(test_config_reload
        PRIVATE
            doctest::doctest
            batbox_core
            simdjson::simdjson
    )

    include(doctest)
    doctest_discover_tests(test_config_reload)

endif()
# ---------------------------------------------------------------------------
# CPP S.13 — CopyCmd integration test
#
# Links CopyCmd.cpp + SlashCommandRegistry.cpp directly so the test builds
# even when the full batbox_commands OBJECT library is not yet fully wired.
# Clipboard dispatch is exercised via the BATBOX_CLIPBOARD_CMD env override.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_copy_command.cpp)

    add_executable(test_copy_command
        ${CMAKE_CURRENT_SOURCE_DIR}/test_copy_command.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/CopyCmd.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/SlashCommandRegistry.cpp
    )

    set_target_properties(test_copy_command PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_copy_command
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    target_link_libraries(test_copy_command
        PRIVATE
            doctest::doctest
            batbox_core
            simdjson::simdjson
    )

    include(doctest)
    doctest_discover_tests(test_copy_command)

endif()

# ---------------------------------------------------------------------------
# CPP 8.7 — WsTransport integration test
#
# Spawns tests/fixtures/fake_mcp_ws.py (a minimal JSON-RPC 2.0 WebSocket
# server) and exercises WsTransport against it.  Requires Python 3 + the
# websockets package (pip install websockets) on PATH.
#
# Links WsTransport.cpp and JsonRpc.cpp directly so this test builds even
# when the full batbox_mcp OBJECT library is not yet fully wired.
# CancelToken.cpp is compiled in (same pattern as test_bash_tool).
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_mcp_ws.cpp)

    add_executable(test_mcp_ws
        ${CMAKE_CURRENT_SOURCE_DIR}/test_mcp_ws.cpp
        ${PROJECT_SOURCE_DIR}/src/mcp/WsTransport.cpp
        ${PROJECT_SOURCE_DIR}/src/mcp/JsonRpc.cpp
    )

    set_target_properties(test_mcp_ws PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_mcp_ws
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    target_compile_definitions(test_mcp_ws
        PRIVATE
            BATBOX_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures"
    )

    target_link_libraries(test_mcp_ws
        PRIVATE
            doctest::doctest
            batbox_core
            batbox_config
            ixwebsocket::ixwebsocket
            simdjson::simdjson
    )

    include(doctest)
    doctest_discover_tests(test_mcp_ws)

endif()

# ---------------------------------------------------------------------------
# CPP 8.4 — StdioTransport integration test
#
# Spawns tests/fixtures/fake_mcp_stdio.py as a child MCP server and
# exercises StdioTransport through the full initialize → tools/list →
# tools/call → stop() lifecycle.
#
# Links StdioTransport.cpp + McpFraming.cpp + JsonRpc.cpp + CancelToken.cpp
# directly so the test builds even when batbox_mcp is not fully wired in.
# Requires Python 3 on PATH.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_mcp_stdio.cpp)

    add_executable(test_mcp_stdio
        ${CMAKE_CURRENT_SOURCE_DIR}/test_mcp_stdio.cpp
        ${PROJECT_SOURCE_DIR}/src/mcp/StdioTransport.cpp
        ${PROJECT_SOURCE_DIR}/src/mcp/JsonRpc.cpp
        ${PROJECT_SOURCE_DIR}/src/mcp/McpFraming.cpp
    )

    set_target_properties(test_mcp_stdio PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_mcp_stdio
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    # Expose the fixtures directory so find_fixture_script() resolves correctly.
    target_compile_definitions(test_mcp_stdio
        PRIVATE
            BATBOX_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures"
    )

    target_link_libraries(test_mcp_stdio
        PRIVATE
            doctest::doctest
            batbox_core
            simdjson::simdjson
    )

    include(doctest)
    doctest_discover_tests(test_mcp_stdio)

endif()

# ---------------------------------------------------------------------------
# CPP 8.5 — HttpTransport integration test
#
# Spawns tests/fixtures/fake_mcp_http.py as a child process and exercises
# HttpTransport against it.  Requires Python 3 on PATH.
# Links HttpTransport.cpp + JsonRpc.cpp + SseParser.cpp + CancelToken.cpp
# directly so we don't depend on the full batbox_mcp OBJECT library being
# wired into the build yet.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_mcp_http.cpp)

    add_executable(test_mcp_http
        ${CMAKE_CURRENT_SOURCE_DIR}/test_mcp_http.cpp
        ${PROJECT_SOURCE_DIR}/src/mcp/HttpTransport.cpp
        ${PROJECT_SOURCE_DIR}/src/mcp/JsonRpc.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/SseParser.cpp
    )

    set_target_properties(test_mcp_http PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_mcp_http
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    # Expose the fixtures directory so find_fixture_script() works.
    target_compile_definitions(test_mcp_http
        PRIVATE
            BATBOX_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures"
    )

    target_link_libraries(test_mcp_http
        PRIVATE
            doctest::doctest
            batbox_core
            batbox_config
            cpr::cpr
            simdjson::simdjson
            spdlog::spdlog
            fmt::fmt
    )

    doctest_discover_tests(test_mcp_http)

endif()

# ---------------------------------------------------------------------------
# CPP 8.6 — SseTransport integration test
#
# Spawns tests/fixtures/fake_mcp_sse.py and exercises SseTransport against
# it.  Links SseTransport.cpp, JsonRpc.cpp, SseParser.cpp, and CancelToken.cpp
# directly so the test builds without the full batbox_mcp OBJECT library.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_mcp_sse.cpp)

    add_executable(test_mcp_sse
        ${CMAKE_CURRENT_SOURCE_DIR}/test_mcp_sse.cpp
        ${PROJECT_SOURCE_DIR}/src/mcp/SseTransport.cpp
        ${PROJECT_SOURCE_DIR}/src/mcp/JsonRpc.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/SseParser.cpp
    )

    set_target_properties(test_mcp_sse PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_mcp_sse
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    # Expose the fixtures directory so find_fixture_script() works.
    target_compile_definitions(test_mcp_sse
        PRIVATE
            BATBOX_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures"
    )

    target_link_libraries(test_mcp_sse
        PRIVATE
            doctest::doctest
            batbox_core
            cpr::cpr
            simdjson::simdjson
    )

    doctest_discover_tests(test_mcp_sse)

endif()

# ---------------------------------------------------------------------------
# CPP 5.12 — WebFetchTool integration test
#
# Links WebFetchTool.cpp, ScraplingClient.cpp, ScraplingProto.cpp, and
# CancelToken.cpp directly against an in-process mock HTTP server so no
# real Python sidecar process is needed.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_webfetch_via_sidecar.cpp)

    add_executable(test_webfetch_via_sidecar
        ${CMAKE_CURRENT_SOURCE_DIR}/test_webfetch_via_sidecar.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/WebFetchTool.cpp
        ${PROJECT_SOURCE_DIR}/src/sidecar/ScraplingClient.cpp
        ${PROJECT_SOURCE_DIR}/src/sidecar/ScraplingProto.cpp
        ${PROJECT_SOURCE_DIR}/src/sidecar/SidecarManager.cpp
        ${PROJECT_SOURCE_DIR}/src/sidecar/SidecarState.cpp
    )

    set_target_properties(test_webfetch_via_sidecar PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_webfetch_via_sidecar
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    target_link_libraries(test_webfetch_via_sidecar
        PRIVATE
            doctest::doctest
            batbox_core
            batbox_config
            cpr::cpr
            simdjson::simdjson
    )

    include(doctest)
    doctest_discover_tests(test_webfetch_via_sidecar)

endif()

# ---------------------------------------------------------------------------
# CPP 5.23 — ConfigTool integration test
#
# Links ConfigTool.cpp directly against the pre-built batbox_config and
# batbox_tools OBJECT library targets (which already include Config.cpp,
# ConfigReload.cpp, EnvLoader.cpp, SettingsLoader.cpp).  CancelToken.cpp
# is provided via batbox_core.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_config_tool.cpp)

    add_executable(test_config_tool
        ${CMAKE_CURRENT_SOURCE_DIR}/test_config_tool.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/ConfigTool.cpp
    )

    set_target_properties(test_config_tool PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_config_tool
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    target_link_libraries(test_config_tool
        PRIVATE
            doctest::doctest
            batbox_core
            batbox_config
            batbox_permissions
            simdjson::simdjson
    )

    include(doctest)
    doctest_discover_tests(test_config_tool)

endif()

# ---------------------------------------------------------------------------
# CPP 3.6 — Conversation basic integration test
#
# Exercises Conversation::user_message + run_turn (no-tools path) against
# the fake_openai_server.py fixture.  Links all required .cpp files directly
# so this test builds independently of the full batbox_conversation OBJECT
# library target being wired in.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_conversation_basic.cpp)

    add_executable(test_conversation_basic
        ${CMAKE_CURRENT_SOURCE_DIR}/test_conversation_basic.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Conversation.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/ToolCallOrchestrator.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Message.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/ToolRegistry.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/ContextWindow.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Compactor.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/Client.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/ChatRequest.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/SseParser.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionStore.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionFile.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionIndex.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionRecovery.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/SystemPrompt.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/ToolCallAccumulator.cpp
        ${PROJECT_SOURCE_DIR}/src/perf/PerfSnapshot.cpp
    )

    set_target_properties(test_conversation_basic PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_conversation_basic
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    target_compile_definitions(test_conversation_basic
        PRIVATE
            BATBOX_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures"
    )

    target_link_libraries(test_conversation_basic
        PRIVATE
            doctest::doctest
            batbox_config
            batbox_core
            batbox_permissions
            batbox_perf
            cpr::cpr
            simdjson::simdjson
            spdlog::spdlog
            fmt::fmt
            ZLIB::ZLIB
    )

    include(doctest)
    doctest_discover_tests(test_conversation_basic)

endif()

# ---------------------------------------------------------------------------
# PEXT2 3.4 (D-8) -- Config snapshot race regression test
#
# Verifies that Conversation::run_turn() snapshots cfg.api.default_model and
# cfg.api.api_key under cfg_mutex at turn start, eliminating the data race
# that occurs when /model switches on the UI thread while the worker thread
# reads those fields during inference.
#
# Under BATBOX_SANITIZERS=thread (Linux only): the test is designed to produce
# zero TSan reports after the snapshot patch.  Enable with:
#   cmake -B build -DBATBOX_SANITIZERS=thread
#   cmake --build build --target test_cfg_snapshot_race
#   TSAN_OPTIONS=halt_on_error=1 ctest -R test_cfg_snapshot_race -V
#
# On macOS/arm64 (Apple Clang TSan not reliably available): the test still
# exercises the concurrent path and asserts correctness without instrumentation.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_cfg_snapshot_race.cpp)

    add_executable(test_cfg_snapshot_race
        ${CMAKE_CURRENT_SOURCE_DIR}/test_cfg_snapshot_race.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Conversation.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/ToolCallOrchestrator.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Message.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/ToolRegistry.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/ContextWindow.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Compactor.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/Client.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/ChatRequest.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/SseParser.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionStore.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionFile.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionIndex.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionRecovery.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/SystemPrompt.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/ToolCallAccumulator.cpp
        ${PROJECT_SOURCE_DIR}/src/perf/PerfSnapshot.cpp
    )

    set_target_properties(test_cfg_snapshot_race PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_cfg_snapshot_race
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    target_compile_definitions(test_cfg_snapshot_race
        PRIVATE
            BATBOX_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures"
    )

    target_link_libraries(test_cfg_snapshot_race
        PRIVATE
            doctest::doctest
            batbox_config
            batbox_core
            batbox_permissions
            batbox_perf
            cpr::cpr
            simdjson::simdjson
            spdlog::spdlog
            fmt::fmt
            ZLIB::ZLIB
    )

    # Apply TSan instrumentation when BATBOX_SANITIZERS=thread.
    # On macOS this call is a no-op (TSan skipped with a CMake warning).
    if(COMMAND batbox_enable_sanitizers)
        batbox_enable_sanitizers(test_cfg_snapshot_race)
    endif()

    include(doctest)
    doctest_discover_tests(test_cfg_snapshot_race)

endif()


# ---------------------------------------------------------------------------
# TUI-FIX-T5 — Conversation tool-call loop + follow-up turn test
#
# Verifies that run_turn() drives the full tool-call loop (User →
# Assistant{tool_calls} → Tool → Assistant{stop}) and that a subsequent
# user message is handled correctly (no dropped-turn bug).
# Root cause of the live bug: App.cpp tui_on_submit had no turn-in-flight
# guard, allowing concurrent user_message()/run_turn() on the non-thread-safe
# Conversation.  Fix: tui_turn_in_flight atomic bool (TUI-FIX-T5).
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_conversation_tool_loop.cpp)

    add_executable(test_conversation_tool_loop
        ${CMAKE_CURRENT_SOURCE_DIR}/test_conversation_tool_loop.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Conversation.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/ToolCallOrchestrator.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Message.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/ToolRegistry.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/ContextWindow.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Compactor.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/Client.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/ChatRequest.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/SseParser.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionStore.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionFile.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionIndex.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionRecovery.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/SystemPrompt.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/ToolCallAccumulator.cpp
        ${PROJECT_SOURCE_DIR}/src/perf/PerfSnapshot.cpp
    )

    set_target_properties(test_conversation_tool_loop PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_conversation_tool_loop
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    target_compile_definitions(test_conversation_tool_loop
        PRIVATE
            BATBOX_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures"
    )

    target_link_libraries(test_conversation_tool_loop
        PRIVATE
            doctest::doctest
            batbox_config
            batbox_core
            batbox_permissions
            batbox_perf
            cpr::cpr
            simdjson::simdjson
            spdlog::spdlog
            fmt::fmt
            ZLIB::ZLIB
    )

    include(doctest)
    doctest_discover_tests(test_conversation_tool_loop)

endif()

# ---------------------------------------------------------------------------
# CPP 5.13 — WebSearchTool integration test
#
# Exercises WebSearchTool via an in-process mock HTTP server so no real Python
# sidecar process is needed.  Adds all required .cpp files directly to the
# executable to avoid ODR duplicate symbol issues from OBJECT library
# transitive dependency ordering.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_websearch_via_sidecar.cpp)

    add_executable(test_websearch_via_sidecar
        ${CMAKE_CURRENT_SOURCE_DIR}/test_websearch_via_sidecar.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/WebSearchTool.cpp
        ${PROJECT_SOURCE_DIR}/src/sidecar/ScraplingClient.cpp
        ${PROJECT_SOURCE_DIR}/src/sidecar/ScraplingProto.cpp
        ${PROJECT_SOURCE_DIR}/src/sidecar/SidecarManager.cpp
        ${PROJECT_SOURCE_DIR}/src/sidecar/SidecarState.cpp
        ${PROJECT_SOURCE_DIR}/src/config/Config.cpp
        ${PROJECT_SOURCE_DIR}/src/config/EnvLoader.cpp
        ${PROJECT_SOURCE_DIR}/src/config/SettingsLoader.cpp
        ${PROJECT_SOURCE_DIR}/src/core/CancelToken.cpp
        ${PROJECT_SOURCE_DIR}/src/core/Json.cpp
        ${PROJECT_SOURCE_DIR}/src/core/Logging.cpp
        ${PROJECT_SOURCE_DIR}/src/core/Paths.cpp
    )

    set_target_properties(test_websearch_via_sidecar PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_websearch_via_sidecar
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    target_link_libraries(test_websearch_via_sidecar
        PRIVATE
            doctest::doctest
            cpr::cpr
            simdjson::simdjson
            spdlog::spdlog
            fmt::fmt
    )

    include(doctest)
    doctest_discover_tests(test_websearch_via_sidecar)

endif()

# ---------------------------------------------------------------------------
# CPP 12.4 — PermissionGate integration test
#
# Links PermissionGate.cpp + PermissionMode.cpp + PermissionRule.cpp +
# PatternMatcher.cpp + PermissionStore.cpp + SettingsLoader.cpp directly so
# this test builds without depending on the full batbox_permissions or
# batbox_config OBJECT library transitive link chains.  Json.cpp and
# CancelToken.cpp are provided via batbox_core.  SettingsLoader.cpp is
# compiled directly (not via batbox_config) to avoid duplicate symbols when
# also linking batbox_config's OBJECT library.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_permission_gate.cpp)

    add_executable(test_permission_gate
        ${CMAKE_CURRENT_SOURCE_DIR}/test_permission_gate.cpp
        ${PROJECT_SOURCE_DIR}/src/permissions/PermissionGate.cpp
        ${PROJECT_SOURCE_DIR}/src/permissions/PermissionMode.cpp
        ${PROJECT_SOURCE_DIR}/src/permissions/PermissionRule.cpp
        ${PROJECT_SOURCE_DIR}/src/permissions/PatternMatcher.cpp
        ${PROJECT_SOURCE_DIR}/src/permissions/PermissionStore.cpp
        ${PROJECT_SOURCE_DIR}/src/config/SettingsLoader.cpp
    )

    set_target_properties(test_permission_gate PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_permission_gate
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    target_link_libraries(test_permission_gate
        PRIVATE
            doctest::doctest
            batbox_core
            simdjson::simdjson
    )

    include(doctest)
    doctest_discover_tests(test_permission_gate)

endif()

# ---------------------------------------------------------------------------
# CPP 8.8 — McpServerRegistry integration test
#
# Tests McpServerRegistry with in-process MockTransport instances.  Covers
# the transport factory (all 4 transport types instantiable from config),
# start_all() parallelism, restart(), and health-monitor plumbing.
#
# Links via batbox_mcp (which includes McpServerRegistry + all transports)
# and batbox_config (which includes McpConfig).  CancelToken.cpp is provided
# via batbox_core.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_mcp_registry.cpp)

    add_executable(test_mcp_registry
        ${CMAKE_CURRENT_SOURCE_DIR}/test_mcp_registry.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/SseParser.cpp
    )

    set_target_properties(test_mcp_registry PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_mcp_registry
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    target_link_libraries(test_mcp_registry
        PRIVATE
            doctest::doctest
            batbox_mcp
            batbox_core
            batbox_config
            cpr::cpr
            ixwebsocket::ixwebsocket
            simdjson::simdjson
    )

    include(doctest)
    doctest_discover_tests(test_mcp_registry)

endif()

# ---------------------------------------------------------------------------
# CPP 7.9 — SidecarManager lifecycle integration test
#
# Spawns tests/fixtures/fake_scrapling_server.py as a real Python child
# process (via SidecarManager's posix_spawn mechanism) and exercises the
# full Cold→Starting→Running→shutdown→crash-recovery→restart-cap lifecycle.
#
# Requires Python 3 on PATH.  Links SidecarManager.cpp, ScraplingClient.cpp,
# ScraplingProto.cpp, SidecarState.cpp, and CancelToken.cpp directly.
#
# BATBOX_FIXTURE_DIR is injected so find_fixture_file() resolves
# tests/fixtures/fake_scrapling_server.py at run-time.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_sidecar_lifecycle.cpp)

    add_executable(test_sidecar_lifecycle
        ${CMAKE_CURRENT_SOURCE_DIR}/test_sidecar_lifecycle.cpp
        ${PROJECT_SOURCE_DIR}/src/sidecar/SidecarManager.cpp
        ${PROJECT_SOURCE_DIR}/src/sidecar/ScraplingClient.cpp
        ${PROJECT_SOURCE_DIR}/src/sidecar/ScraplingProto.cpp
        ${PROJECT_SOURCE_DIR}/src/sidecar/SidecarState.cpp
    )

    set_target_properties(test_sidecar_lifecycle PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_sidecar_lifecycle
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    target_compile_definitions(test_sidecar_lifecycle
        PRIVATE
            BATBOX_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures"
    )

    target_link_libraries(test_sidecar_lifecycle
        PRIVATE
            doctest::doctest
            batbox_core
            batbox_config
            cpr::cpr
            simdjson::simdjson
            spdlog::spdlog
    )

    include(doctest)
    doctest_discover_tests(test_sidecar_lifecycle)

endif()

# ---------------------------------------------------------------------------
# CPP T.2 — Sidecar lifecycle integration test (expanded)
#
# Extends CPP 7.9 with: post-crash auto-restart (AC6), restart-cap boundary
# (AC7), graceful /shutdown endpoint (AC8), SIGKILL escalation (AC9),
# prewarm_async + abort_startup race (AC10), multi-fetch concurrency (AC11).
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_sidecar_lifecycle_full.cpp)

    add_executable(test_sidecar_lifecycle_full
        ${CMAKE_CURRENT_SOURCE_DIR}/test_sidecar_lifecycle_full.cpp
        ${PROJECT_SOURCE_DIR}/src/sidecar/SidecarManager.cpp
        ${PROJECT_SOURCE_DIR}/src/sidecar/ScraplingClient.cpp
        ${PROJECT_SOURCE_DIR}/src/sidecar/ScraplingProto.cpp
        ${PROJECT_SOURCE_DIR}/src/sidecar/SidecarState.cpp
    )

    set_target_properties(test_sidecar_lifecycle_full PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_sidecar_lifecycle_full
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    target_compile_definitions(test_sidecar_lifecycle_full
        PRIVATE
            BATBOX_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures"
    )

    target_link_libraries(test_sidecar_lifecycle_full
        PRIVATE
            doctest::doctest
            batbox_core
            batbox_config
            cpr::cpr
            simdjson::simdjson
            spdlog::spdlog
    )

    include(doctest)
    doctest_discover_tests(test_sidecar_lifecycle_full)

endif()

# ---------------------------------------------------------------------------
# CPP S.12 — IDE/editor commands integration test
#
# Tests /ide (Phase-2 stub), /vim, /keybindings, /terminal-setup.
# Links IdeCmd.cpp, VimCmd.cpp, KeybindingsCmd.cpp, TerminalSetupCmd.cpp,
# SlashCommandRegistry.cpp, VimMode.cpp, Keybindings.cpp, KeybindingsConfig.cpp
# directly so the test builds without the full batbox_commands OBJECT library.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_ide_editor_commands.cpp)

    add_executable(test_ide_editor_commands
        ${CMAKE_CURRENT_SOURCE_DIR}/test_ide_editor_commands.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/IdeCmd.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/VimCmd.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/KeybindingsCmd.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/TerminalSetupCmd.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/SlashCommandRegistry.cpp
        ${PROJECT_SOURCE_DIR}/src/repl/VimMode.cpp
        ${PROJECT_SOURCE_DIR}/src/repl/Keybindings.cpp
        ${PROJECT_SOURCE_DIR}/src/config/KeybindingsConfig.cpp
    )

    set_target_properties(test_ide_editor_commands PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_ide_editor_commands
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    find_package(simdjson CONFIG QUIET)
    find_package(nlohmann_json CONFIG QUIET)
    find_package(fmt CONFIG QUIET)
    find_package(ftxui CONFIG QUIET)

    target_link_libraries(test_ide_editor_commands
        PRIVATE
            doctest::doctest
            batbox_core
            $<$<TARGET_EXISTS:simdjson::simdjson>:simdjson::simdjson>
            $<$<TARGET_EXISTS:nlohmann_json::nlohmann_json>:nlohmann_json::nlohmann_json>
            $<$<TARGET_EXISTS:fmt::fmt>:fmt::fmt>
            $<$<TARGET_EXISTS:ftxui::component>:ftxui::component>
    )

    include(doctest)
    doctest_discover_tests(test_ide_editor_commands)

endif()

# ---------------------------------------------------------------------------
# CPP S.5 — /resume, /session, /compact integration test
#
# Links ResumeCmd.cpp, SessionCmd.cpp, CompactCmd.cpp, SlashCommandRegistry.cpp,
# and the session store chain directly so the test builds even without the
# full batbox_commands / batbox_session OBJECT libraries being assembled.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_session_commands.cpp)

    add_executable(test_session_commands
        ${CMAKE_CURRENT_SOURCE_DIR}/test_session_commands.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/ResumeCmd.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/SessionCmd.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/CompactCmd.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/SlashCommandRegistry.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionStore.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionFile.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionIndex.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionRecovery.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Compactor.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Message.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/Client.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/ChatRequest.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/SseParser.cpp
    )

    set_target_properties(test_session_commands PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_session_commands
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    find_package(simdjson CONFIG QUIET)
    find_package(nlohmann_json CONFIG QUIET)
    find_package(fmt CONFIG QUIET)
    find_package(ZLIB QUIET)

    target_link_libraries(test_session_commands
        PRIVATE
            doctest::doctest
            batbox_core
            batbox_config
            cpr::cpr
            $<$<TARGET_EXISTS:simdjson::simdjson>:simdjson::simdjson>
            $<$<TARGET_EXISTS:nlohmann_json::nlohmann_json>:nlohmann_json::nlohmann_json>
            $<$<TARGET_EXISTS:fmt::fmt>:fmt::fmt>
            $<$<TARGET_EXISTS:ZLIB::ZLIB>:ZLIB::ZLIB>
    )

    include(doctest)
    doctest_discover_tests(test_session_commands)

endif()

# ---------------------------------------------------------------------------
# CPP S.6 — MemoryCmd / ContextCmd integration test
#
# Links MemoryCmd.cpp, ContextCmd.cpp, SlashCommandRegistry.cpp,
# ContextWindow.cpp, and Message.cpp directly so the test builds even when
# the full batbox_commands / batbox_conversation OBJECT libraries are not
# yet fully wired.
# BATBOX_MEMORY_EDITOR env var is used to inject a no-op editor.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_memory_context_commands.cpp)

    add_executable(test_memory_context_commands
        ${CMAKE_CURRENT_SOURCE_DIR}/test_memory_context_commands.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/MemoryCmd.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/ContextCmd.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/SlashCommandRegistry.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/ContextWindow.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Message.cpp
    )

    set_target_properties(test_memory_context_commands PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_memory_context_commands
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    find_package(simdjson CONFIG QUIET)
    find_package(nlohmann_json CONFIG QUIET)
    find_package(fmt CONFIG QUIET)

    target_link_libraries(test_memory_context_commands
        PRIVATE
            doctest::doctest
            batbox_core
            batbox_config
            $<$<TARGET_EXISTS:simdjson::simdjson>:simdjson::simdjson>
            $<$<TARGET_EXISTS:nlohmann_json::nlohmann_json>:nlohmann_json::nlohmann_json>
            $<$<TARGET_EXISTS:fmt::fmt>:fmt::fmt>
    )

    include(doctest)
    doctest_discover_tests(test_memory_context_commands)

endif()


# ---------------------------------------------------------------------------
# CPP S.10 — PermissionsCmd / HooksCmd / AdvisorCmd integration tests
#
# Links the three command implementations + SlashCommandRegistry.cpp +
# PermissionStore.cpp + PermissionRule.cpp + SettingsLoader.cpp directly so
# the test builds even when the full batbox_commands OBJECT library is not
# yet wired.  simdjson is required by batbox::Json (used in HooksCmd.cpp and
# SettingsLoader.cpp) and nlohmann_json by SettingsLoader.cpp.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_permissions_advisor.cpp)

    add_executable(test_permissions_advisor
        ${CMAKE_CURRENT_SOURCE_DIR}/test_permissions_advisor.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/PermissionsCmd.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/HooksCmd.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/AdvisorCmd.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/SlashCommandRegistry.cpp
        ${PROJECT_SOURCE_DIR}/src/permissions/PermissionStore.cpp
        ${PROJECT_SOURCE_DIR}/src/permissions/PermissionRule.cpp
        ${PROJECT_SOURCE_DIR}/src/permissions/PermissionMode.cpp
        ${PROJECT_SOURCE_DIR}/src/config/SettingsLoader.cpp
    )

    set_target_properties(test_permissions_advisor PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_permissions_advisor
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    find_package(nlohmann_json CONFIG QUIET)
    find_package(simdjson CONFIG QUIET)

    target_link_libraries(test_permissions_advisor
        PRIVATE
            doctest::doctest
            batbox_core
            $<$<TARGET_EXISTS:nlohmann_json::nlohmann_json>:nlohmann_json::nlohmann_json>
            $<$<TARGET_EXISTS:simdjson::simdjson>:simdjson::simdjson>
    )

    include(doctest)
    doctest_discover_tests(test_permissions_advisor)

endif()

# ---------------------------------------------------------------------------
# CPP 1.13 — SubAgentPanel integration test
#
# Tests TuiAgentTickerThread 10Hz constraint and SubAgentPanel rendering
# logic using the AgentEventQueue dirty_seq mechanism.  Links SubAgentPanel.cpp,
# Events.cpp, ThemeApply.cpp, AgentEvent.cpp, Theme.cpp, themes.cpp directly
# so this test builds without the full batbox_tui or batbox_agents OBJECT
# libraries needing to be fully assembled.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_subagent_panel.cpp)

    add_executable(test_subagent_panel
        ${CMAKE_CURRENT_SOURCE_DIR}/test_subagent_panel.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/SubAgentPanel.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/Events.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/ThemeApply.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/AgentEvent.cpp
        ${PROJECT_SOURCE_DIR}/src/theme/Theme.cpp
        ${PROJECT_SOURCE_DIR}/src/theme/themes.cpp
    )

    set_target_properties(test_subagent_panel PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_subagent_panel
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${PROJECT_SOURCE_DIR}/tests
    )

    find_package(ftxui CONFIG QUIET)

    target_link_libraries(test_subagent_panel
        PRIVATE
            doctest::doctest
            batbox_core
            batbox_agents
            $<$<TARGET_EXISTS:ftxui::component>:ftxui::component>
            $<$<TARGET_EXISTS:ftxui::dom>:ftxui::dom>
            $<$<TARGET_EXISTS:ftxui::screen>:ftxui::screen>
    )

    include(doctest)
    doctest_discover_tests(test_subagent_panel)

endif()

# ---------------------------------------------------------------------------
# CPP 5.26 — MCP Resources tools integration test
#
# Exercises ListMcpResourcesTool and ReadMcpResourceTool using in-process
# MockTransport injected via McpServerRegistry::add_transport().  No real
# MCP server, network, or child process is required.
#
# Links ListMcpResourcesTool.cpp, ReadMcpResourceTool.cpp directly;
# McpServerRegistry and all 4 transports come in via batbox_mcp (same
# pattern as test_mcp_registry).
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_mcp_resources_tools.cpp)

    add_executable(test_mcp_resources_tools
        ${CMAKE_CURRENT_SOURCE_DIR}/test_mcp_resources_tools.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/ListMcpResourcesTool.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/ReadMcpResourceTool.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/SseParser.cpp
    )

    set_target_properties(test_mcp_resources_tools PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_mcp_resources_tools
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    target_link_libraries(test_mcp_resources_tools
        PRIVATE
            doctest::doctest
            batbox_mcp
            batbox_core
            batbox_config
            batbox_permissions
    )

    include(doctest)
    doctest_discover_tests(test_mcp_resources_tools)

endif()

# ---------------------------------------------------------------------------
# CPP 5.17 — CronScheduler integration test
#
# Links CronScheduler.cpp + CronCreateTool.cpp + CronDeleteTool.cpp +
# CronListTool.cpp + Uuid.cpp (for generate_uuid_v4) directly so this test
# builds without the full batbox_tools OBJECT library.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_cron_scheduler.cpp)

    add_executable(test_cron_scheduler
        ${CMAKE_CURRENT_SOURCE_DIR}/test_cron_scheduler.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/CronScheduler.cpp
    )

    set_target_properties(test_cron_scheduler PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_cron_scheduler
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    find_package(simdjson CONFIG QUIET)
    find_package(spdlog CONFIG QUIET)
    find_package(fmt CONFIG QUIET)

    target_link_libraries(test_cron_scheduler
        PRIVATE
            doctest::doctest
            batbox_core
            $<$<TARGET_EXISTS:simdjson::simdjson>:simdjson::simdjson>
            $<$<TARGET_EXISTS:spdlog::spdlog>:spdlog::spdlog>
            $<$<TARGET_EXISTS:fmt::fmt>:fmt::fmt>
    )

    include(doctest)
    doctest_discover_tests(test_cron_scheduler)

endif()

# ---------------------------------------------------------------------------
# CPP 5.28 — Sub-agent tool family integration test
#
# Tests TaskTool, TaskOutputTool, TaskStopTool, SendMessageTool against a
# real AgentSupervisor (stub implementation from AgentSupervisor.cpp).
# Links the four tool .cpp files + AgentSupervisor.cpp stub + AgentSpec.cpp
# stub + CancelToken.cpp directly so the test builds without the full
# batbox_tools or batbox_agents OBJECT library being assembled.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_task_family.cpp)

    add_executable(test_task_family
        ${CMAKE_CURRENT_SOURCE_DIR}/test_task_family.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/TaskTool.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/TaskOutputTool.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/TaskStopTool.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/SendMessageTool.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/AgentSupervisor.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/AgentSpec.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/AgentEvent.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/SubAgent.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/FrontmatterParser.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Conversation.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/ToolCallOrchestrator.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Message.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/ContextWindow.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Compactor.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/SystemPrompt.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/ToolRegistry.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/Client.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/ChatRequest.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/SseParser.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/ToolCallAccumulator.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionStore.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionFile.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionIndex.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionRecovery.cpp
        ${PROJECT_SOURCE_DIR}/src/perf/PerfSnapshot.cpp
    )

    set_target_properties(test_task_family PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_task_family
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    target_link_libraries(test_task_family
        PRIVATE
            doctest::doctest
            batbox_core
            batbox_config
            batbox_permissions
            batbox_perf
            cpr::cpr
            simdjson::simdjson
    )

    include(doctest)
    doctest_discover_tests(test_task_family)

endif()

# ---------------------------------------------------------------------------
# CPP 5.27 — McpTool integration test
#
# Exercises McpTool::run() against in-process MockTransport instances injected
# into McpServerRegistry.  No network or real MCP server required.
# Links all required MCP transport sources directly.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_mcp_dynamic_tool.cpp)

    add_executable(test_mcp_dynamic_tool
        ${CMAKE_CURRENT_SOURCE_DIR}/test_mcp_dynamic_tool.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/McpTool.cpp
        ${PROJECT_SOURCE_DIR}/src/mcp/McpServerRegistry.cpp
        ${PROJECT_SOURCE_DIR}/src/mcp/StdioTransport.cpp
        ${PROJECT_SOURCE_DIR}/src/mcp/HttpTransport.cpp
        ${PROJECT_SOURCE_DIR}/src/mcp/SseTransport.cpp
        ${PROJECT_SOURCE_DIR}/src/mcp/WsTransport.cpp
        ${PROJECT_SOURCE_DIR}/src/mcp/JsonRpc.cpp
        ${PROJECT_SOURCE_DIR}/src/mcp/McpFraming.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/SseParser.cpp
    )

    set_target_properties(test_mcp_dynamic_tool PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_mcp_dynamic_tool
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    target_link_libraries(test_mcp_dynamic_tool
        PRIVATE
            doctest::doctest
            batbox_core
            batbox_config
            cpr::cpr
            ixwebsocket::ixwebsocket
            simdjson::simdjson
    )

    include(doctest)
    doctest_discover_tests(test_mcp_dynamic_tool)

endif()

# ---------------------------------------------------------------------------
# CPP 6.6 — Team blackboard integration test
#
# Links Team.cpp directly so this test builds without the full batbox_agents
# OBJECT library being assembled.  Json.cpp is provided via batbox_core.
# Tests: team create/delete,
# membership lifecycle, blackboard set/get/cas/erase, thread-safe contention
# under 4 concurrent agents, broadcast enqueue+drain, registry singleton.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_team_blackboard.cpp)

    add_executable(test_team_blackboard
        ${CMAKE_CURRENT_SOURCE_DIR}/test_team_blackboard.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/Team.cpp
    )

    set_target_properties(test_team_blackboard PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_team_blackboard
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    find_package(simdjson CONFIG QUIET)
    find_package(nlohmann_json CONFIG QUIET)

    target_link_libraries(test_team_blackboard
        PRIVATE
            doctest::doctest
            batbox_core
            $<$<TARGET_EXISTS:simdjson::simdjson>:simdjson::simdjson>
            $<$<TARGET_EXISTS:nlohmann_json::nlohmann_json>:nlohmann_json::nlohmann_json>
    )

    include(doctest)
    doctest_discover_tests(test_team_blackboard)

endif()

# ---------------------------------------------------------------------------
# CPP 6.3 — AgentLoader integration test
#
# Links FrontmatterParser.cpp + AgentLoader.cpp + AgentSpec.cpp directly so
# this test builds even when the full batbox_agents OBJECT library is not
# yet fully assembled.  Paths.cpp is needed by AgentLoader::load() for
# home_dir() resolution.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_agent_loader.cpp)

    add_executable(test_agent_loader
        ${CMAKE_CURRENT_SOURCE_DIR}/test_agent_loader.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/FrontmatterParser.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/AgentLoader.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/AgentSpec.cpp
    )

    set_target_properties(test_agent_loader PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_agent_loader
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    target_link_libraries(test_agent_loader
        PRIVATE
            doctest::doctest
            batbox_core
            simdjson::simdjson
    )

    include(doctest)
    doctest_discover_tests(test_agent_loader)

endif()

# ---------------------------------------------------------------------------
# CPP S.3 — ThemeCmd / OutputStyleCmd integration test
#
# Links ThemeCmd.cpp, OutputStyleCmd.cpp, SlashCommandRegistry.cpp, and
# SettingsLoader.cpp directly so the test builds without the full
# batbox_commands or batbox_config OBJECT libraries being assembled.
# nlohmann_json is required by SettingsLoader.cpp; simdjson by batbox::Json.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_theme_output_commands.cpp)

    add_executable(test_theme_output_commands
        ${CMAKE_CURRENT_SOURCE_DIR}/test_theme_output_commands.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/ThemeCmd.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/OutputStyleCmd.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/SlashCommandRegistry.cpp
        ${PROJECT_SOURCE_DIR}/src/config/SettingsLoader.cpp
    )

    set_target_properties(test_theme_output_commands PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_theme_output_commands
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    find_package(nlohmann_json CONFIG QUIET)
    find_package(simdjson CONFIG QUIET)

    target_link_libraries(test_theme_output_commands
        PRIVATE
            doctest::doctest
            batbox_core
            $<$<TARGET_EXISTS:nlohmann_json::nlohmann_json>:nlohmann_json::nlohmann_json>
            $<$<TARGET_EXISTS:simdjson::simdjson>:simdjson::simdjson>
    )

    include(doctest)
    doctest_discover_tests(test_theme_output_commands)

endif()

# ---------------------------------------------------------------------------
# CPP 11.5 — AgentLoader (plugin-side) integration test
#
# Links FrontmatterParser.cpp + AgentLoader.cpp directly so this test builds
# even when the full batbox_plugins library target is not yet fully assembled.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_plugin_agent_loader.cpp)

    add_executable(test_plugin_agent_loader
        ${CMAKE_CURRENT_SOURCE_DIR}/test_plugin_agent_loader.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/FrontmatterParser.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/AgentLoader.cpp
    )

    set_target_properties(test_plugin_agent_loader PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_plugin_agent_loader
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    find_package(simdjson CONFIG QUIET)
    find_package(nlohmann_json CONFIG QUIET)
    find_package(fmt CONFIG QUIET)

    target_link_libraries(test_plugin_agent_loader
        PRIVATE
            doctest::doctest
            batbox_core
            $<$<TARGET_EXISTS:simdjson::simdjson>:simdjson::simdjson>
            $<$<TARGET_EXISTS:nlohmann_json::nlohmann_json>:nlohmann_json::nlohmann_json>
            $<$<TARGET_EXISTS:fmt::fmt>:fmt::fmt>
    )

    doctest_discover_tests(test_plugin_agent_loader)

endif()

# ---------------------------------------------------------------------------
# CPP S.14 — DemonCmd integration test
#
# Links DemonCmd.cpp + SlashCommandRegistry.cpp + AgentSupervisor.cpp
# + AgentSpec.cpp + DemonPanel.cpp + Events.cpp + FrontmatterParser.cpp
# directly so the test builds even when the full batbox_commands /
# batbox_agents / batbox_tui OBJECT libraries are not wired.
# CancelToken.cpp is provided via batbox_core.
#
# Tests run in null-panel + null-supervisor mode
# (register_demon_cmd(registry) headless overload) to avoid needing a live
# FTXUI ScreenInteractive.  DemonPanel.cpp must be linked because DemonCmd.cpp
# references DemonPanel methods that must resolve at link time.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_demon_command.cpp)

    add_executable(test_demon_command
        ${CMAKE_CURRENT_SOURCE_DIR}/test_demon_command.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/DemonCmd.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/SlashCommandRegistry.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/DemonPanel.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/Events.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/AgentSupervisor.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/SubAgent.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/AgentEvent.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/AgentSpec.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/FrontmatterParser.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Conversation.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/ToolCallOrchestrator.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Message.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/ContextWindow.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Compactor.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/SystemPrompt.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/ToolRegistry.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/Client.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/ChatRequest.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/SseParser.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/ToolCallAccumulator.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionStore.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionFile.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionIndex.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionRecovery.cpp
        ${PROJECT_SOURCE_DIR}/src/perf/PerfSnapshot.cpp
    )

    set_target_properties(test_demon_command PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_demon_command
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    find_package(ftxui CONFIG QUIET)
    find_package(simdjson CONFIG QUIET)
    find_package(nlohmann_json CONFIG QUIET)

    target_link_libraries(test_demon_command
        PRIVATE
            doctest::doctest
            batbox_core
            batbox_config
            batbox_permissions
            batbox_perf
            cpr::cpr
            $<$<TARGET_EXISTS:ftxui::component>:ftxui::component>
            $<$<TARGET_EXISTS:ftxui::dom>:ftxui::dom>
            $<$<TARGET_EXISTS:ftxui::screen>:ftxui::screen>
            $<$<TARGET_EXISTS:simdjson::simdjson>:simdjson::simdjson>
            $<$<TARGET_EXISTS:nlohmann_json::nlohmann_json>:nlohmann_json::nlohmann_json>
    )

    include(doctest)
    doctest_discover_tests(test_demon_command)

endif()

# ---------------------------------------------------------------------------
# CPP 6.8 — Demon agent spec + rate limiter integration test
#
# Tests demon_make_spec() and DemonRateLimiter purely in memory.
# No FTXUI, no AgentSupervisor threads — pure logic tests.
# Links Demon.cpp + AgentSpec.cpp + FrontmatterParser.cpp directly.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_demon.cpp)

    add_executable(test_demon
        ${CMAKE_CURRENT_SOURCE_DIR}/test_demon.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/Demon.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/AgentSpec.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/FrontmatterParser.cpp
    )

    set_target_properties(test_demon PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_demon
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${PROJECT_SOURCE_DIR}/src/agents   # for demon_taglines.hpp
    )

    find_package(simdjson CONFIG QUIET)
    find_package(nlohmann_json CONFIG QUIET)

    target_link_libraries(test_demon
        PRIVATE
            doctest::doctest
            batbox_core
            $<$<TARGET_EXISTS:simdjson::simdjson>:simdjson::simdjson>
            $<$<TARGET_EXISTS:nlohmann_json::nlohmann_json>:nlohmann_json::nlohmann_json>
    )

    include(doctest)
    doctest_discover_tests(test_demon)

endif()

# ---------------------------------------------------------------------------
# CPP S.7 — AddDirCmd / FilesCmd / DiffCmd integration test
#
# Tests /add-dir (path validation + persistence), /files (tree view + filter),
# and /diff (git diff rendering + non-git error).  Links the three Cmd sources
# plus SlashCommandRegistry.cpp directly so this test builds without the full
# batbox_commands OBJECT library being wired.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_filesystem_commands.cpp)

    add_executable(test_filesystem_commands
        ${CMAKE_CURRENT_SOURCE_DIR}/test_filesystem_commands.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/AddDirCmd.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/FilesCmd.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/DiffCmd.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/SlashCommandRegistry.cpp
    )

    set_target_properties(test_filesystem_commands PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_filesystem_commands
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    find_package(simdjson CONFIG QUIET)
    find_package(nlohmann_json CONFIG QUIET)

    target_link_libraries(test_filesystem_commands
        PRIVATE
            doctest::doctest
            batbox_core
            $<$<TARGET_EXISTS:simdjson::simdjson>:simdjson::simdjson>
            $<$<TARGET_EXISTS:nlohmann_json::nlohmann_json>:nlohmann_json::nlohmann_json>
    )

    include(doctest)
    doctest_discover_tests(test_filesystem_commands)

endif()

# ---------------------------------------------------------------------------
# CPP 6.4 — SubAgent integration test
#
# Tests SubAgent lifecycle: Started → TokenAppended* → Completed event order,
# cancellation via cancel(), enqueue_message() peer injection, status transitions,
# and last_5_lines snapshot rolling buffer.
#
# Links SubAgent.cpp, AgentEvent.cpp, AgentSupervisor.cpp (stub), AgentSpec.cpp,
# FrontmatterParser.cpp, Conversation.cpp, and all inference + session sources
# directly.  Routes inference calls through fake_openai_server.py fixture.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_subagent.cpp)

    add_executable(test_subagent
        ${CMAKE_CURRENT_SOURCE_DIR}/test_subagent.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/SubAgent.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/AgentEvent.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/AgentSupervisor.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/AgentSpec.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/FrontmatterParser.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Conversation.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/ToolCallOrchestrator.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Message.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/ContextWindow.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Compactor.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/SystemPrompt.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/ToolRegistry.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/Client.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/ChatRequest.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/SseParser.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/ToolCallAccumulator.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionStore.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionFile.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionIndex.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionRecovery.cpp
        ${PROJECT_SOURCE_DIR}/src/perf/PerfSnapshot.cpp
    )

    set_target_properties(test_subagent PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_subagent
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    target_compile_definitions(test_subagent
        PRIVATE
            BATBOX_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures"
    )

    target_link_libraries(test_subagent
        PRIVATE
            doctest::doctest
            batbox_core
            batbox_config
            batbox_permissions
            batbox_perf
            cpr::cpr
            simdjson::simdjson
    )

    include(doctest)
    doctest_discover_tests(test_subagent)

endif()

# ---------------------------------------------------------------------------
# CPP S.2 — /model, /config, /effort command integration tests
#
# Links ModelCmd.cpp, ConfigCmd.cpp, EffortCmd.cpp, SlashCommandRegistry.cpp,
# and EnvLoader.cpp directly so the test builds without the full
# batbox_commands OBJECT library being assembled.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_model_config_commands.cpp)

    add_executable(test_model_config_commands
        ${CMAKE_CURRENT_SOURCE_DIR}/test_model_config_commands.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/ModelCmd.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/ConfigCmd.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/EffortCmd.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/SlashCommandRegistry.cpp
        ${PROJECT_SOURCE_DIR}/src/config/EnvLoader.cpp
    )

    set_target_properties(test_model_config_commands PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_model_config_commands
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    find_package(simdjson CONFIG QUIET)
    find_package(nlohmann_json CONFIG QUIET)
    find_package(fmt CONFIG QUIET)

    target_link_libraries(test_model_config_commands
        PRIVATE
            doctest::doctest
            batbox_core
            $<$<TARGET_EXISTS:simdjson::simdjson>:simdjson::simdjson>
            $<$<TARGET_EXISTS:nlohmann_json::nlohmann_json>:nlohmann_json::nlohmann_json>
            $<$<TARGET_EXISTS:fmt::fmt>:fmt::fmt>
    )

    include(doctest)
    doctest_discover_tests(test_model_config_commands)

endif()

# ---------------------------------------------------------------------------
# CPP S.4 — /status, /stats, /usage, /cost integration test
#
# Links the four command implementations plus their runtime dependencies:
#   - SidecarState.cpp      : SidecarStateMachine::current(), to_string()
#   - McpServerRegistry.cpp : server_names(), get() — needed by StatusCmd
#   - All four MCP transports (StdioTransport, SseTransport, HttpTransport,
#     WsTransport) — pulled in transitively by McpServerRegistry.cpp
#   - AgentSupervisor.cpp   : snapshot() — needed by StatsCmd
#   - UsageTracker.cpp, ModelPricing.cpp — token/cost accumulators
#
# In the test the sidecar_manager, mcp_registry, and agent_supervisor
# pointers are always nullptr, so no live subprocess or network is required.
# The source files are linked to satisfy the linker symbol requirements.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_status_stats_commands.cpp)

    add_executable(test_status_stats_commands
        ${CMAKE_CURRENT_SOURCE_DIR}/test_status_stats_commands.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/StatusCmd.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/StatsCmd.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/UsageCmd.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/CostCmd.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/SlashCommandRegistry.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/UsageTracker.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/ModelPricing.cpp
        ${PROJECT_SOURCE_DIR}/src/sidecar/SidecarState.cpp
        ${PROJECT_SOURCE_DIR}/src/mcp/McpServerRegistry.cpp
        ${PROJECT_SOURCE_DIR}/src/mcp/JsonRpc.cpp
        ${PROJECT_SOURCE_DIR}/src/mcp/McpFraming.cpp
        ${PROJECT_SOURCE_DIR}/src/mcp/StdioTransport.cpp
        ${PROJECT_SOURCE_DIR}/src/mcp/SseTransport.cpp
        ${PROJECT_SOURCE_DIR}/src/mcp/HttpTransport.cpp
        ${PROJECT_SOURCE_DIR}/src/mcp/WsTransport.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/AgentSupervisor.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/SubAgent.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/AgentEvent.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/AgentSpec.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/FrontmatterParser.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/SseParser.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Conversation.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/ToolCallOrchestrator.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Message.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/ContextWindow.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Compactor.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/SystemPrompt.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/ToolRegistry.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/Client.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/ChatRequest.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/ToolCallAccumulator.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionStore.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionFile.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionIndex.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionRecovery.cpp
        ${PROJECT_SOURCE_DIR}/src/perf/PerfSnapshot.cpp
    )

    set_target_properties(test_status_stats_commands PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_status_stats_commands
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    find_package(simdjson CONFIG QUIET)
    find_package(nlohmann_json CONFIG QUIET)
    find_package(fmt CONFIG QUIET)
    find_package(ZLIB QUIET)

    target_link_libraries(test_status_stats_commands
        PRIVATE
            doctest::doctest
            batbox_core
            batbox_config
            batbox_permissions
            batbox_perf
            cpr::cpr
            $<$<TARGET_EXISTS:ixwebsocket::ixwebsocket>:ixwebsocket::ixwebsocket>
            $<$<TARGET_EXISTS:simdjson::simdjson>:simdjson::simdjson>
            $<$<TARGET_EXISTS:nlohmann_json::nlohmann_json>:nlohmann_json::nlohmann_json>
            $<$<TARGET_EXISTS:fmt::fmt>:fmt::fmt>
            $<$<TARGET_EXISTS:ZLIB::ZLIB>:ZLIB::ZLIB>
    )

    include(doctest)
    doctest_discover_tests(test_status_stats_commands)

endif()

# ---------------------------------------------------------------------------
# CPP 8.9 — McpClient unit tests
#
# Tests the McpClient orchestrator using in-process MockTransport injected
# via McpServerRegistry::add_transport().  Exercises initialize handshake,
# tools/list caching and cache-invalidation on list_changed, tools/call,
# resources/*, prompts/*, reconnect-on-failure, and unknown-server error
# paths.
#
# Links via batbox_mcp (which includes McpServerRegistry + all transports +
# McpClient), batbox_core (provides CancelToken), and batbox_config.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_mcp_client.cpp)

    add_executable(test_mcp_client
        ${CMAKE_CURRENT_SOURCE_DIR}/test_mcp_client.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/SseParser.cpp
    )

    set_target_properties(test_mcp_client PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_mcp_client
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    target_link_libraries(test_mcp_client
        PRIVATE
            doctest::doctest
            batbox_mcp
            batbox_core
            batbox_config
            cpr::cpr
            ixwebsocket::ixwebsocket
            simdjson::simdjson
    )

    include(doctest)
    doctest_discover_tests(test_mcp_client)

endif()

# ---------------------------------------------------------------------------
# CPP S.9 — /agents, /plan, /tasks, /skills slash command integration tests
#
# Links all four command .cpp files + their required dependencies directly
# so this test builds without requiring the full batbox_commands OBJECT
# library to be assembled.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_agent_planning_commands.cpp)

    add_executable(test_agent_planning_commands
        ${CMAKE_CURRENT_SOURCE_DIR}/test_agent_planning_commands.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/AgentsCmd.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/PlanCmd.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/TasksCmd.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/SkillsCmd.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/SlashCommandRegistry.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/PlanMode.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/SkillLoader.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/FrontmatterParser.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/TaskStore.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/AgentSupervisor.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/SubAgent.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/AgentEvent.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/AgentSpec.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Conversation.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/ToolCallOrchestrator.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Message.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/ContextWindow.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Compactor.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/SystemPrompt.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/ToolRegistry.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/Client.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/ChatRequest.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/SseParser.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/ToolCallAccumulator.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionStore.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionFile.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionIndex.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionRecovery.cpp
        ${PROJECT_SOURCE_DIR}/src/perf/PerfSnapshot.cpp
    )

    set_target_properties(test_agent_planning_commands PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_agent_planning_commands
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    find_package(fmt CONFIG QUIET)
    find_package(spdlog CONFIG QUIET)
    find_package(simdjson CONFIG QUIET)

    target_link_libraries(test_agent_planning_commands
        PRIVATE
            doctest::doctest
            batbox_core
            batbox_config
            batbox_permissions
            batbox_perf
            cpr::cpr
            $<$<TARGET_EXISTS:fmt::fmt>:fmt::fmt>
            $<$<TARGET_EXISTS:spdlog::spdlog>:spdlog::spdlog>
            $<$<TARGET_EXISTS:simdjson::simdjson>:simdjson::simdjson>
    )

    include(doctest)
    doctest_discover_tests(test_agent_planning_commands)

endif()

# ---------------------------------------------------------------------------
# CPP 6.7 — Workflow DAG integration test
#
# Tests Workflow::execute() with the stub AgentSupervisor:
#   - structural validation (duplicate names, unknown deps, self-dep)
#   - cycle detection (2-node, 3-node, partial cycles)
#   - empty workflow
#   - diamond DAG structural layout
#   - failure policy enum round-trip
#   - steps() / failure_policy() accessors
#
# Links Workflow.cpp, AgentSpec.cpp, AgentEvent.cpp, AgentSupervisor.cpp,
# and FrontmatterParser.cpp directly so this test builds independently of
# the batbox_agents OBJECT library status.  CancelToken.cpp is provided
# via batbox_core.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_workflow_dag.cpp)

    add_executable(test_workflow_dag
        ${CMAKE_CURRENT_SOURCE_DIR}/test_workflow_dag.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/Workflow.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/AgentSpec.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/AgentEvent.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/AgentSupervisor.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/SubAgent.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/FrontmatterParser.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Conversation.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/ToolCallOrchestrator.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Message.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/ContextWindow.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Compactor.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/SystemPrompt.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/ToolRegistry.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/Client.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/ChatRequest.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/SseParser.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/ToolCallAccumulator.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionStore.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionFile.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionIndex.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionRecovery.cpp
        ${PROJECT_SOURCE_DIR}/src/perf/PerfSnapshot.cpp
    )

    set_target_properties(test_workflow_dag PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_workflow_dag
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    target_link_libraries(test_workflow_dag
        PRIVATE
            doctest::doctest
            batbox_core
            batbox_config
            batbox_permissions
            batbox_perf
            cpr::cpr
            simdjson::simdjson
    )

    include(doctest)
    doctest_discover_tests(test_workflow_dag)

endif()

# ---------------------------------------------------------------------------
# CPP 6.5 — AgentSupervisor bounded-supervision integration test
#
# Tests semaphore-bounded spawn + snapshot + cancellation + wait_all()
# without requiring a live inference server.  All agents are pre-cancelled
# so they exit cooperatively without performing network I/O.
#
# Links AgentSupervisor.cpp, SubAgent.cpp, AgentEvent.cpp, AgentSpec.cpp,
# FrontmatterParser.cpp, and the inference + session + conversation sources
# needed to satisfy the SubAgent constructor.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_agent_supervision_bounded.cpp)

    add_executable(test_agent_supervision_bounded
        ${CMAKE_CURRENT_SOURCE_DIR}/test_agent_supervision_bounded.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/AgentSupervisor.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/SubAgent.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/AgentEvent.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/AgentSpec.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/FrontmatterParser.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Conversation.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/ToolCallOrchestrator.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Message.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/ContextWindow.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Compactor.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/SystemPrompt.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/ToolRegistry.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/Client.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/ChatRequest.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/SseParser.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/ToolCallAccumulator.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionStore.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionFile.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionIndex.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionRecovery.cpp
        ${PROJECT_SOURCE_DIR}/src/perf/PerfSnapshot.cpp
    )

    set_target_properties(test_agent_supervision_bounded PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_agent_supervision_bounded
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    target_link_libraries(test_agent_supervision_bounded
        PRIVATE
            doctest::doctest
            batbox_core
            batbox_config
            batbox_permissions
            batbox_perf
            cpr::cpr
            simdjson::simdjson
    )

    include(doctest)
    doctest_discover_tests(test_agent_supervision_bounded)

endif()
# ---------------------------------------------------------------------------
# CPP 6.9 — AgentSupervisor deep integration test
#
# Extends CPP 6.5 with new scenarios that exercise:
#   - Cascading cancellation (cancel all → all terminal)
#   - Pre-cancelled parent token → fast exit
#   - Cancel-while-queue-non-empty → pending queue drains cleanly
#   - Burst load: 8 spawns / limit=4 → at most 4 concurrent running
#   - FIFO queue ordering: limit=1 + 3 spawns → all exit in spawn order
#   - Queue promotion: cancel one of 4 running → queued agent promoted
#   - Team broadcast + partial member cancellation (in-memory, no server)
#   - Workflow DAG: cycle detection, diamond layout, cancellation, empty wf
#   - Concurrent snapshot() thread safety
#
# Routes inference calls through fake_openai_server.py for suites A-C and F.
# Suites D and E are purely in-memory (no inference server needed).
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_agent_supervision_integration.cpp)

    add_executable(test_agent_supervision_integration
        ${CMAKE_CURRENT_SOURCE_DIR}/test_agent_supervision_integration.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/AgentSupervisor.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/SubAgent.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/AgentEvent.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/AgentSpec.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/Team.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/Workflow.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/FrontmatterParser.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Conversation.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/ToolCallOrchestrator.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Message.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/ContextWindow.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Compactor.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/SystemPrompt.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/ToolRegistry.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/Client.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/ChatRequest.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/SseParser.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/ToolCallAccumulator.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionStore.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionFile.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionIndex.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionRecovery.cpp
        ${PROJECT_SOURCE_DIR}/src/perf/PerfSnapshot.cpp
    )

    set_target_properties(test_agent_supervision_integration PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_agent_supervision_integration
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    target_compile_definitions(test_agent_supervision_integration
        PRIVATE
            BATBOX_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures"
    )

    target_link_libraries(test_agent_supervision_integration
        PRIVATE
            doctest::doctest
            batbox_core
            batbox_config
            batbox_permissions
            batbox_perf
            cpr::cpr
            simdjson::simdjson
    )

    include(doctest)
    doctest_discover_tests(test_agent_supervision_integration)

endif()


# ---------------------------------------------------------------------------
# CPP S.8 — /review and /security-review integration test
#
# Tests ReviewCmd + SecurityReviewCmd through the ISlashCommand interface:
# no-diff gating, user confirmation, inject_user_message call, prompt content
# (OWASP keywords, diff delimiters), --pr and --branch flags, and joint
# registration in the same SlashCommandRegistry.
#
# Links ReviewCmd.cpp, SecurityReviewCmd.cpp, SlashCommandRegistry.cpp
# directly so this test builds even when batbox_commands is not assembled.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_review_commands.cpp)

    add_executable(test_review_commands
        ${CMAKE_CURRENT_SOURCE_DIR}/test_review_commands.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/ReviewCmd.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/SecurityReviewCmd.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/SlashCommandRegistry.cpp
    )

    set_target_properties(test_review_commands PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_review_commands
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    find_package(simdjson CONFIG QUIET)
    find_package(nlohmann_json CONFIG QUIET)

    target_link_libraries(test_review_commands
        PRIVATE
            doctest::doctest
            batbox_core
            $<$<TARGET_EXISTS:simdjson::simdjson>:simdjson::simdjson>
            $<$<TARGET_EXISTS:nlohmann_json::nlohmann_json>:nlohmann_json::nlohmann_json>
    )

    include(doctest)
    doctest_discover_tests(test_review_commands)

endif()

# ---------------------------------------------------------------------------
# CPP A.1 — Headless --print mode integration test
#
# Spawns fake_openai_server.py and forks the batbox binary itself to test
# --print, --print-format, stdin prompt path, argv prompt path, JSON output,
# exit codes, and absence of TUI artefacts on stdout.
#
# Requires:
#   - Python 3 on PATH (for fake_openai_server.py)
#   - BATBOX_FIXTURE_DIR (CMAKE compile-time define → path to tests/fixtures/)
#   - BATBOX_BINARY_DIR  (CMAKE compile-time define → build tree root)
#   - nlohmann_json (for JSON assertion in test)
#
# This test links against no batbox source files directly — it exercises the
# fully-linked batbox binary end-to-end (the black-box approach).
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_headless_print.cpp)

    add_executable(test_headless_print
        ${CMAKE_CURRENT_SOURCE_DIR}/test_headless_print.cpp
    )

    set_target_properties(test_headless_print PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_headless_print
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    target_compile_definitions(test_headless_print
        PRIVATE
            BATBOX_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures"
            BATBOX_BINARY_DIR="${CMAKE_BINARY_DIR}"
    )

    find_package(nlohmann_json CONFIG QUIET)

    target_link_libraries(test_headless_print
        PRIVATE
            doctest::doctest
            $<$<TARGET_EXISTS:nlohmann_json::nlohmann_json>:nlohmann_json::nlohmann_json>
    )

    include(doctest)
    doctest_discover_tests(test_headless_print)

endif()

# ---------------------------------------------------------------------------
# CPP 5.29 — TeamCreate / TeamDelete / ListPeers / Workflow tool family
#
# Links the four tool .cpp files plus Team, Workflow, AgentSupervisor, and
# their transitive agents/ dependencies so the test builds without the full
# batbox_tools or batbox_agents OBJECT libraries being assembled.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_team_family.cpp)

    add_executable(test_team_family
        ${CMAKE_CURRENT_SOURCE_DIR}/test_team_family.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/TeamCreateTool.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/TeamDeleteTool.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/ListPeersTool.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/WorkflowTool.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/Team.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/Workflow.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/AgentSpec.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/AgentEvent.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/AgentSupervisor.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/FrontmatterParser.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/SubAgentStub.cpp
    )

    set_target_properties(test_team_family PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_team_family
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    target_link_libraries(test_team_family
        PRIVATE
            doctest::doctest
            batbox_core
            batbox_config
            batbox_permissions
            simdjson::simdjson
    )

    include(doctest)
    doctest_discover_tests(test_team_family)

endif()

# ---------------------------------------------------------------------------
# CPP 2.6 — Repl main loop + prefix dispatch integration test
#
# Links Repl.cpp + all component sources needed to exercise the prefix routes:
# slash (SlashCommandRegistry), bash (!), note (#), history push, exit flag.
# Does NOT invoke real inference (no API server needed).
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_repl.cpp)

    add_executable(test_repl
        ${CMAKE_CURRENT_SOURCE_DIR}/test_repl.cpp
        ${PROJECT_SOURCE_DIR}/src/repl/Repl.cpp
        ${PROJECT_SOURCE_DIR}/src/repl/History.cpp
        ${PROJECT_SOURCE_DIR}/src/repl/HistorySearch.cpp
        ${PROJECT_SOURCE_DIR}/src/repl/Autocomplete.cpp
        ${PROJECT_SOURCE_DIR}/src/repl/VimMode.cpp
        ${PROJECT_SOURCE_DIR}/src/repl/Keybindings.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/SlashCommandRegistry.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/BashTool.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/bash/BashRunner.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/bash/PtyBackend.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/bash/PipesBackend.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/bash/EnvScrub.cpp
        ${PROJECT_SOURCE_DIR}/src/tools/bash/AnsiStrip.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Conversation.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Message.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/ContextWindow.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/Compactor.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/SystemPrompt.cpp
        ${PROJECT_SOURCE_DIR}/src/conversation/ToolCallOrchestrator.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionStore.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionFile.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionIndex.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionRecovery.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/Client.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/ChatRequest.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/SseParser.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/ToolCallAccumulator.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/UsageTracker.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/ModelPricing.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/PluginRegistry.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/FrontmatterParser.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/MarketplaceJson.cpp
        ${PROJECT_SOURCE_DIR}/src/perf/PerfSnapshot.cpp
    )

    set_target_properties(test_repl PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_repl
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${PROJECT_SOURCE_DIR}/src/tools/bash
    )

    target_link_libraries(test_repl
        PRIVATE
            doctest::doctest
            batbox_core
            batbox_config
            batbox_permissions
            batbox_perf
            batbox_tools
            cpr::cpr
            simdjson::simdjson
            ZLIB::ZLIB
    )

    # Keybindings.cpp uses ftxui::Event symbols — link the component + screen libs.
    find_package(ftxui CONFIG QUIET)
    if(TARGET ftxui::component)
        target_link_libraries(test_repl PRIVATE
            ftxui::component
            ftxui::dom
            ftxui::screen
        )
    endif()

    # macOS ships forkpty in libc; Linux needs -lutil
    if(UNIX AND NOT APPLE)
        target_link_libraries(test_repl PRIVATE util)
    endif()

    include(doctest)
    doctest_discover_tests(test_repl)

endif()

# ---------------------------------------------------------------------------
# CPP T.4 — MCP-transports integration test: all 4 transports together
#
# Spins up all 4 fixture servers (stdio, http, sse, ws) simultaneously,
# registers them in McpServerRegistry, and exercises McpClient across all 4
# in parallel:
#   AC1. All 4 fixture servers boot to healthy state.
#   AC2. McpClient.initialize_all() handshakes all 4 in parallel.
#   AC3. Each transport's tools/list returns its declared tool.
#   AC4. tools/call routes to the correct transport by server name.
#   AC5. Crashing one server: others remain healthy; crashed one unhealthy.
#   AC6. Test runs on Linux + macOS.
#
# Links via batbox_mcp (McpClient + McpServerRegistry + all 4 transports),
# batbox_core, batbox_config, and all required transport dependencies.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_mcp_transports.cpp)

    add_executable(test_mcp_transports
        ${CMAKE_CURRENT_SOURCE_DIR}/test_mcp_transports.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/SseParser.cpp
    )

    set_target_properties(test_mcp_transports PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_mcp_transports
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    target_compile_definitions(test_mcp_transports
        PRIVATE
            BATBOX_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures"
    )

    target_link_libraries(test_mcp_transports
        PRIVATE
            doctest::doctest
            batbox_mcp
            batbox_core
            batbox_config
            cpr::cpr
            ixwebsocket::ixwebsocket
            simdjson::simdjson
    )

    include(doctest)
    doctest_discover_tests(test_mcp_transports)

endif()

# ---------------------------------------------------------------------------
# CPP S.11 — /mcp command integration test
#
# Tests McpCmd slash command: registration, list, restart, resources, prompts.
# Uses MinimalMockTransport injected via McpServerRegistry::add_transport().
# Links McpCmd.cpp + SlashCommandRegistry.cpp directly plus batbox_mcp and
# batbox_commands dependencies.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_mcp_command.cpp)

    add_executable(test_mcp_command
        ${CMAKE_CURRENT_SOURCE_DIR}/test_mcp_command.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/McpCmd.cpp
        ${PROJECT_SOURCE_DIR}/src/commands/SlashCommandRegistry.cpp
        ${PROJECT_SOURCE_DIR}/src/inference/SseParser.cpp
    )

    set_target_properties(test_mcp_command PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_mcp_command
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    target_link_libraries(test_mcp_command
        PRIVATE
            doctest::doctest
            batbox_mcp
            batbox_core
            batbox_config
            cpr::cpr
            ixwebsocket::ixwebsocket
            simdjson::simdjson
    )

    include(doctest)
    doctest_discover_tests(test_mcp_command)

endif()

# ---------------------------------------------------------------------------
# CPP 1.15 — wire_tui() integration test
#
# Tests the full TUI layout composition: ChatView + SubAgentPanel + DemonPanel
# + InputBar assembled via build_main_layout, plus the Splash → main transition.
#
# Sources compiled directly (same pattern as test_subagent_panel) so the test
# builds without requiring the full batbox_tui OBJECT library to be assembled.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_tui_layout.cpp)

    add_executable(test_tui_layout
        ${CMAKE_CURRENT_SOURCE_DIR}/test_tui_layout.cpp
        ${PROJECT_SOURCE_DIR}/src/app/WireTui.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/PermissionBanner.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/Screen.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/ChatView.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/SubAgentPanel.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/DemonPanel.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/InputBar.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/Splash.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/Events.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/ThemeApply.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/MarkdownRender.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/StreamingMessageView.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/SyntaxHighlight.cpp
        ${PROJECT_SOURCE_DIR}/src/agents/AgentEvent.cpp
        # TUI-T4: PermissionCard and its permission dependencies
        ${PROJECT_SOURCE_DIR}/src/tui/PermissionCard.cpp
        ${PROJECT_SOURCE_DIR}/src/permissions/PermissionRule.cpp
        ${PROJECT_SOURCE_DIR}/src/permissions/PermissionStore.cpp
        ${PROJECT_SOURCE_DIR}/src/permissions/PatternMatcher.cpp
        # TUI-FIX: missing sources that WireTui.cpp pulls in
        ${PROJECT_SOURCE_DIR}/src/app/McpStatusPoller.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/QuestionCard.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/Changelog.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/AccountLabel.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/PlanApprovalCard.cpp
        ${PROJECT_SOURCE_DIR}/src/util/EditorLaunch.cpp
        # Theme.cpp and themes.cpp are omitted here: batbox_theme (OBJECT) in
        # target_link_libraries already provides them without duplication.
    )

    set_target_properties(test_tui_layout PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_tui_layout
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${PROJECT_SOURCE_DIR}/tests
    )

    find_package(ftxui CONFIG QUIET)

    target_link_libraries(test_tui_layout
        PRIVATE
            doctest::doctest
            batbox_core
            batbox_repl
            batbox_config
            batbox_theme
            batbox_perf
            $<$<TARGET_EXISTS:ftxui::component>:ftxui::component>
            $<$<TARGET_EXISTS:ftxui::dom>:ftxui::dom>
            $<$<TARGET_EXISTS:ftxui::screen>:ftxui::screen>
    )

    # Manual lexers compiled directly into test
    target_sources(test_tui_layout PRIVATE
        ${PROJECT_SOURCE_DIR}/src/tui/manual_lexers/cpp_lexer.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/manual_lexers/python_lexer.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/manual_lexers/js_lexer.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/manual_lexers/ts_lexer.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/manual_lexers/rust_lexer.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/manual_lexers/go_lexer.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/manual_lexers/html_lexer.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/manual_lexers/css_lexer.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/manual_lexers/json_lexer.cpp
    )

    # batbox_repl (OBJECT) depends on batbox_commands and batbox_plugins (also OBJECT);
    # OBJECT libraries do not propagate transitively, so compile needed sources directly.
    target_sources(test_tui_layout PRIVATE
        ${PROJECT_SOURCE_DIR}/src/commands/SlashCommandRegistry.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/PluginRegistry.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/MarketplaceJson.cpp
        ${PROJECT_SOURCE_DIR}/src/plugins/FrontmatterParser.cpp
        ${PROJECT_SOURCE_DIR}/src/perf/PerfSnapshot.cpp
    )
    target_link_libraries(test_tui_layout PRIVATE simdjson::simdjson spdlog::spdlog)

    include(doctest)
    doctest_discover_tests(test_tui_layout)

endif()

# ---------------------------------------------------------------------------
# CPP 1.16 — PermissionBanner + Nuclear mode integration test
#
# Tests the top-of-screen permission mode banner: mode cycling via Shift+Tab,
# Nuclear confirmation modal (y/N), CLI set_mode_direct() bypass, and rendering
# in all four modes.
#
# Sources compiled directly so the test builds without requiring the full
# batbox_tui OBJECT library.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_nuclear_mode.cpp)

    add_executable(test_nuclear_mode
        ${CMAKE_CURRENT_SOURCE_DIR}/test_nuclear_mode.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/PermissionBanner.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/ThemeApply.cpp
        ${PROJECT_SOURCE_DIR}/src/permissions/PermissionMode.cpp
        ${PROJECT_SOURCE_DIR}/src/theme/Theme.cpp
        ${PROJECT_SOURCE_DIR}/src/theme/themes.cpp
    )

    set_target_properties(test_nuclear_mode PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_nuclear_mode
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${PROJECT_SOURCE_DIR}/tests
    )

    find_package(ftxui CONFIG QUIET)

    target_link_libraries(test_nuclear_mode
        PRIVATE
            doctest::doctest
            batbox_core
            $<$<TARGET_EXISTS:ftxui::component>:ftxui::component>
            $<$<TARGET_EXISTS:ftxui::dom>:ftxui::dom>
            $<$<TARGET_EXISTS:ftxui::screen>:ftxui::screen>
    )

    include(doctest)
    doctest_discover_tests(test_nuclear_mode)

endif()

# ---------------------------------------------------------------------------
# CPP A.3 — App::init wiring integration test
#
# Tests the init sequence: BundledSkillsRegistry, SkillLoader, Config loading,
# PluginLoader, and headless batbox invocations via a fake OpenAI server.
#
# Links against batbox library targets (not raw .cpp) because the init
# sequence exercises multiple components that are already assembled.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_app_init.cpp)

    add_executable(test_app_init
        ${CMAKE_CURRENT_SOURCE_DIR}/test_app_init.cpp
    )

    set_target_properties(test_app_init PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_app_init
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${PROJECT_SOURCE_DIR}/src       # for App.hpp
    )

    # Expose fixture + binary dirs for process-spawning tests.
    target_compile_definitions(test_app_init
        PRIVATE
            BATBOX_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures"
            BATBOX_BINARY_DIR="${CMAKE_BINARY_DIR}"
    )

    find_package(simdjson CONFIG QUIET)
    find_package(fmt CONFIG QUIET)

    target_link_libraries(test_app_init
        PRIVATE
            doctest::doctest
            batbox::deps
            batbox_core
            batbox_config
            batbox_permissions
            batbox_plugins
            batbox_skills
            batbox_sidecar
            batbox_mcp
            batbox_agents
            batbox_tools
            batbox_tools_bash
            batbox_commands
            batbox_conversation
            batbox_session
            batbox_inference
            batbox_theme
            batbox_repl
            batbox_tui
    )

    include(doctest)
    doctest_discover_tests(test_app_init)

endif()

# ---------------------------------------------------------------------------
# CPP A.4 — App::shutdown integration test
#
# Tests the clean shutdown sequence: null-safety, idempotency, live-subsystem
# teardown (idle AgentSupervisor, cold SidecarManager, empty SessionStore),
# and process-level smoke tests (--print exits cleanly, SIGTERM handled).
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_app_shutdown.cpp)

    add_executable(test_app_shutdown
        ${CMAKE_CURRENT_SOURCE_DIR}/test_app_shutdown.cpp
        ${PROJECT_SOURCE_DIR}/src/AppShutdown.cpp
    )

    set_target_properties(test_app_shutdown PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_app_shutdown
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${PROJECT_SOURCE_DIR}/src       # for App.hpp
    )

    # Expose fixture + binary dirs for process-spawning tests.
    target_compile_definitions(test_app_shutdown
        PRIVATE
            BATBOX_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures"
            BATBOX_BINARY_DIR="${CMAKE_BINARY_DIR}"
    )

    find_package(simdjson CONFIG QUIET)
    find_package(fmt CONFIG QUIET)

    target_link_libraries(test_app_shutdown
        PRIVATE
            doctest::doctest
            batbox::deps
            batbox_core
            batbox_config
            batbox_permissions
            batbox_plugins
            batbox_skills
            batbox_sidecar
            batbox_mcp
            batbox_agents
            batbox_tools
            batbox_tools_bash
            batbox_commands
            batbox_conversation
            batbox_session
            batbox_inference
            batbox_theme
            batbox_repl
            batbox_tui
    )

    include(doctest)
    doctest_discover_tests(test_app_shutdown PROPERTIES LABELS "unit")

endif()

# ---------------------------------------------------------------------------
# CPP T.5 — End-to-end CLI smoke test
#
# Black-box test: invokes the compiled batbox binary against
# tests/fixtures/fake_openai_server.py and fake_mcp_stdio.py.
# Exercises the full conversation pipeline end-to-end.
#
# Requires:
#   - Python 3 on PATH (for fake_openai_server.py / fake_mcp_stdio.py)
#   - BATBOX_FIXTURE_DIR (compile-time define → path to tests/fixtures/)
#   - BATBOX_BINARY_DIR  (compile-time define → build tree root)
#   - nlohmann_json (for JSON assertion in TC6 mcp.json construction)
#
# This test links against no batbox source files directly — it exercises the
# fully-linked batbox binary end-to-end (the black-box approach).
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_e2e_cli_smoke.cpp)

    add_executable(test_e2e_cli_smoke
        ${CMAKE_CURRENT_SOURCE_DIR}/test_e2e_cli_smoke.cpp
    )

    set_target_properties(test_e2e_cli_smoke PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_e2e_cli_smoke
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}   # for cli_smoke_helpers.hpp
            ${PROJECT_SOURCE_DIR}/include
    )

    target_compile_definitions(test_e2e_cli_smoke
        PRIVATE
            BATBOX_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures"
            BATBOX_BINARY_DIR="${CMAKE_BINARY_DIR}"
    )

    find_package(nlohmann_json CONFIG QUIET)

    target_link_libraries(test_e2e_cli_smoke
        PRIVATE
            doctest::doctest
            $<$<TARGET_EXISTS:nlohmann_json::nlohmann_json>:nlohmann_json::nlohmann_json>
    )

    include(doctest)
    doctest_discover_tests(test_e2e_cli_smoke)

endif()

# ---------------------------------------------------------------------------
# CPP T.7 — Performance budget regression test
#
# Verifies the batbox rewrite meets pmdraft.md F3 targets:
#   - Cold start (first stdout byte) < 400 ms
#   - Idle CPU < 5% per core
#   - Streaming CPU < 15% per core
#   - Idle RSS < 100 MB
#   - SubAgentPanel ticker rate: 10 Hz ± 20%
#   - DemonPanel tagline rotation: 5-second period
#   - SubAgentPanel ticker cap: ≤ 10 Hz under high load
#
# Sources compiled directly (same pattern as test_subagent_panel) so the test
# builds without requiring the full batbox_tui OBJECT library to be assembled.
#
# Process-spawn tests use BATBOX_BINARY_DIR to locate the batbox binary and
# BATBOX_FIXTURE_DIR to locate fake_openai_server.py.
#
# macOS only: links libproc (via -lproc) for proc_pidinfo CPU/RSS probes.
# Linux: uses /proc/<pid>/stat and /proc/<pid>/status (no extra link needed).
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_perf_budget.cpp)

    # Sources compiled directly (same pattern as test_subagent_panel).
    # SubAgentPanel.cpp references AgentSupervisor::snapshot() which requires
    # linking batbox_agents (the OBJECT library that contains AgentSupervisor.cpp
    # and its transitive dependencies).  DemonPanel needs Theme + ThemeApply.
    # RenderStats.cpp is compiled here because it is not yet part of any
    # assembled OBJECT library target.
    add_executable(test_perf_budget
        ${CMAKE_CURRENT_SOURCE_DIR}/test_perf_budget.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/SubAgentPanel.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/DemonPanel.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/Events.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/ThemeApply.cpp
        ${PROJECT_SOURCE_DIR}/src/tui/RenderStats.cpp
        ${PROJECT_SOURCE_DIR}/src/theme/Theme.cpp
        ${PROJECT_SOURCE_DIR}/src/theme/themes.cpp
    )

    set_target_properties(test_perf_budget PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_perf_budget
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}   # for perf_probes.hpp
    )

    target_compile_definitions(test_perf_budget
        PRIVATE
            BATBOX_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures"
            BATBOX_BINARY_DIR="${CMAKE_BINARY_DIR}"
    )

    find_package(ftxui CONFIG QUIET)

    # batbox_agents supplies AgentSupervisor, SubAgent, AgentEvent, and all
    # their transitive dependencies (Config, CancelToken, Uuid, Conversation…).
    target_link_libraries(test_perf_budget
        PRIVATE
            doctest::doctest
            batbox_core
            batbox_agents
            $<$<TARGET_EXISTS:ftxui::component>:ftxui::component>
            $<$<TARGET_EXISTS:ftxui::dom>:ftxui::dom>
            $<$<TARGET_EXISTS:ftxui::screen>:ftxui::screen>
    )

    # macOS: proc_pidinfo is in libproc.
    if(APPLE)
        target_link_libraries(test_perf_budget PRIVATE "-lproc")
    endif()

    include(doctest)
    doctest_discover_tests(test_perf_budget)

endif()

# ---------------------------------------------------------------------------
# #15b — headless --print ToolRegistry wiring integration test
#
# Spawns batbox --print against the fake_openai_server.py fixture and verifies:
#   - The process exits 0 (no "no registry/gate configured" error).
#   - Stderr does not contain the old "no registry/gate configured" message.
#   - JSON format works with tools wired.
#   - Nuclear mode completes cleanly.
#   - The App.cpp source no longer contains the old "tools not wired" comments.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_print_mode_tools.cpp)

    add_executable(test_print_mode_tools
        ${CMAKE_CURRENT_SOURCE_DIR}/test_print_mode_tools.cpp
    )

    set_target_properties(test_print_mode_tools PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_print_mode_tools
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    target_compile_definitions(test_print_mode_tools
        PRIVATE
            BATBOX_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures"
            BATBOX_BINARY_DIR="${CMAKE_BINARY_DIR}"
    )

    find_package(nlohmann_json CONFIG QUIET)

    target_link_libraries(test_print_mode_tools
        PRIVATE
            doctest::doctest
            $<$<TARGET_EXISTS:nlohmann_json::nlohmann_json>:nlohmann_json::nlohmann_json>
    )

    include(doctest)
    doctest_discover_tests(test_print_mode_tools)

endif()

# ---------------------------------------------------------------------------
# #17 — --resume <UUID> history seeding integration test
#
# Mixed test: unit-level SessionStore::touch() tests (links session sources)
# plus e2e subprocess tests for --resume flag:
#   - Pre-seeds a session, spawns batbox --resume <uuid> --print.
#   - Verifies "Resumed session" in stderr + 4-message count.
#   - Verifies nonexistent UUID → exit code 2 + "not found" in stderr.
#   - Verifies exit 0 when session exists and inference completes.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_resume_mode.cpp)

    add_executable(test_resume_mode
        ${CMAKE_CURRENT_SOURCE_DIR}/test_resume_mode.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionStore.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionFile.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionIndex.cpp
        ${PROJECT_SOURCE_DIR}/src/session/SessionRecovery.cpp
    )

    set_target_properties(test_resume_mode PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_resume_mode
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    target_compile_definitions(test_resume_mode
        PRIVATE
            BATBOX_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures"
            BATBOX_BINARY_DIR="${CMAKE_BINARY_DIR}"
    )

    find_package(nlohmann_json CONFIG QUIET)

    target_link_libraries(test_resume_mode
        PRIVATE
            doctest::doctest
            batbox_core
            $<$<TARGET_EXISTS:nlohmann_json::nlohmann_json>:nlohmann_json::nlohmann_json>
            ZLIB::ZLIB
            simdjson::simdjson
    )

    include(doctest)
    doctest_discover_tests(test_resume_mode)

# ---------------------------------------------------------------------------
# PEXT3 1.6 — make_askq_prompt_fn nuclear branch integration test
#
# Tests that make_askq_prompt_fn(nuclear=true) returns a closure that
# immediately returns {} without posting modal events.
# Calls the REAL batbox::tui::make_askq_prompt_fn factory directly.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_nuclear_no_modals.cpp)

    add_executable(test_nuclear_no_modals
        ${CMAKE_CURRENT_SOURCE_DIR}/test_nuclear_no_modals.cpp
    )

    set_target_properties(test_nuclear_no_modals PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_include_directories(test_nuclear_no_modals
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    find_package(simdjson CONFIG QUIET)

    target_link_libraries(test_nuclear_no_modals
        PRIVATE
            doctest::doctest
            batbox::deps
            batbox_core
            batbox_config
            batbox_permissions
            batbox_plugins
            batbox_skills
            batbox_sidecar
            batbox_mcp
            batbox_agents
            batbox_tools
            batbox_tools_bash
            batbox_commands
            batbox_conversation
            batbox_session
            batbox_inference
            batbox_theme
            batbox_repl
            batbox_tui
            $<$<TARGET_EXISTS:simdjson::simdjson>:simdjson::simdjson>
    )

    include(doctest)
    doctest_discover_tests(test_nuclear_no_modals)

endif()

endif()