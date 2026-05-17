# =============================================================================
# Component C5 — Tool Engine (batbox_tools)
# 39 tool implementations, dispatch, permission gate hook, MCP bridge, skill bridge.
# Includes the bash/ sub-library for BashTool internals.
# Depends on: batbox_core, batbox_config, batbox_permissions, batbox_mcp
# =============================================================================

add_subdirectory(bash)

# ---------------------------------------------------------------------------
# macOS large-archive fix: Apple's ar+ranlib uses a 32-bit symbol-table index
# that fails ("archive member ... size too large") once libbatbox_tools.a
# grows past ~128 MB.  Apple's /usr/bin/libtool -static writes a 64-bit index
# and is the Apple-recommended tool for exactly this situation.  We override
# the two CMake variables that control static-library creation for this
# directory only, scoped before the add_library() call so CMake picks them up
# when it generates the link rules for batbox_tools.  CMAKE_RANLIB is set to
# a no-op because libtool already embeds the symbol index.
# ---------------------------------------------------------------------------
if(APPLE)
    # <CMAKE_AR> is expanded from CMakeCXXCompiler.cmake which hard-codes
    # /usr/bin/ar before our FORCE takes effect.  Use the absolute libtool
    # path directly in the rule string so it cannot be overridden.
    set(CMAKE_C_CREATE_STATIC_LIBRARY
        "/usr/bin/libtool -static -no_warning_for_no_symbols -o <TARGET> <LINK_FLAGS> <OBJECTS>")
    set(CMAKE_CXX_CREATE_STATIC_LIBRARY
        "/usr/bin/libtool -static -no_warning_for_no_symbols -o <TARGET> <LINK_FLAGS> <OBJECTS>")
    set(CMAKE_RANLIB ":" CACHE FILEPATH "Ranlib (disabled — libtool already indexes)" FORCE)
endif()

add_library(batbox_tools STATIC)

set_target_properties(batbox_tools PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

target_include_directories(batbox_tools
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(batbox_tools
    PUBLIC
        batbox_core
        batbox_config
        batbox_permissions
        batbox_mcp
        batbox_tools_bash
        batbox_agents
)

# ---------------------------------------------------------------------------
# Sources — uncomment as each CPP 5.x task lands:
#   ToolRegistry.cpp          (CPP 5.1)  — ITool dispatch, schema export
#   ReadTool.cpp              (CPP 5.2)  — Read tool
#   WriteTool.cpp             (CPP 5.3)  — Write tool
#   EditTool.cpp              (CPP 5.4)  — Edit tool
#   GlobTool.cpp              (CPP 5.5)  — Glob tool
#   GrepTool.cpp              (CPP 5.6)  — Grep tool
#   ToolSearchTool.cpp        (CPP 5.7)  — ToolSearch tool
#   BashTool.cpp              (CPP 5.8)  — Bash tool (delegates to bash/)
#   PowerShellTool.cpp        (CPP 5.9)  — PowerShell tool (no-op on non-Windows)
#   WebFetchTool.cpp          (CPP 5.10) — WebFetch via sidecar
#   WebSearchTool.cpp         (CPP 5.11) — WebSearch via sidecar
#   TaskTool.cpp              (CPP 5.12) — Task tool (delegates to agents/)
#   TaskOutputTool.cpp        (CPP 5.13) — TaskOutput tool
#   TaskStopTool.cpp          (CPP 5.14) — TaskStop tool
#   SendMessageTool.cpp       (CPP 5.15) — SendMessage tool
#   TodoWriteTool.cpp         (CPP 5.16) — TodoWrite tool
#   TaskCreateTool.cpp        (CPP 5.17) — TaskCreate tool
#   TaskListTool.cpp          (CPP 5.18) — TaskList tool
#   TaskGetTool.cpp           (CPP 5.19) — TaskGet tool
#   TaskUpdateTool.cpp        (CPP 5.20) — TaskUpdate tool
#   CronCreateTool.cpp        (CPP 5.21) — CronCreate tool
#   CronDeleteTool.cpp        (CPP 5.22) — CronDelete tool
#   CronListTool.cpp          (CPP 5.23) — CronList tool
#   EnterPlanModeTool.cpp     (CPP 5.24) — EnterPlanMode tool
#   ExitPlanModeTool.cpp      (CPP 5.25) — ExitPlanMode tool
#   VerifyPlanExecutionTool.cpp (CPP 5.26) — VerifyPlanExecution tool
#   SkillTool.cpp             (CPP 5.27) — Skill tool (delegates to skills/)
#   AskUserQuestionTool.cpp   (CPP 5.28) — AskUserQuestion tool
#   ListMcpResourcesTool.cpp  (CPP 5.29) — ListMcpResources tool
#   ReadMcpResourceTool.cpp   (CPP 5.30) — ReadMcpResource tool
#   McpTool.cpp               (CPP 5.31) — MCP generic tool proxy
#   SleepTool.cpp             (CPP 5.32) — Sleep tool
#   RemoteTriggerTool.cpp     (CPP 5.33) — RemoteTrigger tool
#   ConfigTool.cpp            (CPP 5.34) — Config tool
#   CtxInspectTool.cpp        (CPP 5.35) — CtxInspect tool
#   TeamCreateTool.cpp        (CPP 5.36) — TeamCreate tool
#   TeamDeleteTool.cpp        (CPP 5.37) — TeamDelete tool
#   ListPeersTool.cpp         (CPP 5.38) — ListPeers tool
#   WorkflowTool.cpp          (CPP 5.39) — Workflow tool
#   SnipTool.cpp              (CPP 5.40) — Snip tool
# ---------------------------------------------------------------------------
target_sources(batbox_tools
    PRIVATE
        .placeholder.cpp
        ToolRegistry.cpp
        ReadTool.cpp
        WriteTool.cpp
        EditTool.cpp
        GlobTool.cpp
        GrepTool.cpp
        ToolSearchTool.cpp
        BashTool.cpp
        PowerShellTool.cpp
        WebFetchTool.cpp
        WebSearchTool.cpp
        TaskTool.cpp
        TaskOutputTool.cpp
        TaskStopTool.cpp
        SendMessageTool.cpp
        TodoWriteTool.cpp
        TaskStore.cpp
        TaskCreateTool.cpp
        TaskListTool.cpp
        TaskGetTool.cpp
        TaskUpdateTool.cpp
        CronCreateTool.cpp
        CronDeleteTool.cpp
        CronListTool.cpp
        CronScheduler.cpp
        EnterPlanModeTool.cpp
        ExitPlanModeTool.cpp
        VerifyPlanExecutionTool.cpp
        SkillTool.cpp
        AskUserQuestionTool.cpp
        ListMcpResourcesTool.cpp
        ReadMcpResourceTool.cpp
        McpTool.cpp
        SleepTool.cpp
        RemoteTriggerTool.cpp
        ConfigTool.cpp
        CtxInspectTool.cpp
        TeamCreateTool.cpp
        TeamDeleteTool.cpp
        ListPeersTool.cpp
        WorkflowTool.cpp
        SnipTool.cpp
)
