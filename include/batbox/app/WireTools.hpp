// include/batbox/app/WireTools.hpp
// =============================================================================
// wire_tools() — registers all 39 curated tools into ToolRegistry at App init.
//
// Blueprint contract (task CPP 5.30):
//   function  batbox::app::wire_tools
//   file      include/batbox/app/WireTools.hpp
//   file      src/app/WireTools.cpp
//
// Registration order:
//   1. Read-only / zero-dep tools first (Read, Write, Edit, Glob, Grep,
//      TodoWrite, Sleep, Snip, PowerShell, CtxInspect).
//   2. Config-dep tools (Bash needs config.tools.* limits, WebFetch/Search need
//      sidecar, RemoteTrigger, Config need cfg+mutex).
//   3. Shared-state tools: TaskStore (tasks.json) owns TaskCreate/List/Get/Update;
//      CronScheduler owns CronCreate/Delete/List; PlanMode owns Enter/Exit/Verify.
//   4. MCP tools: McpServerRegistry is constructed first, then
//      ListMcpResources, ReadMcpResource, MCP.
//   5. ToolSearch last among read-only tools (needs a populated registry ref).
//   6. AgentSupervisor / TeamRegistry tools: Task, TaskOutput, TaskStop,
//      SendMessage, TeamCreate, TeamDelete, ListPeers, Workflow.
//   7. Skill: SkillLoader (from plugins layer) must be ready first.
//   8. AskUserQuestion: default-constructed (no deps) or with PromptFn (TUI mode).
//
// Fail-fast contract:
//   After registration, an assert() verifies exactly 39 tools are present.
//   This fires at startup if a tool was accidentally omitted or duplicated.
// =============================================================================

#pragma once

#include <batbox/tools/ToolRegistry.hpp>
#include <batbox/tools/ExitPlanModeTool.hpp>
#include <batbox/tools/AskUserQuestionTool.hpp>
#include <batbox/config/Config.hpp>
#include <batbox/sidecar/SidecarManager.hpp>
#include <batbox/mcp/McpServerRegistry.hpp>
#include <batbox/agents/AgentSupervisor.hpp>
#include <batbox/agents/Team.hpp>
#include <batbox/plugins/SkillLoader.hpp>
#include <batbox/conversation/PlanMode.hpp>

#include <mutex>

namespace batbox::app {

/// Register all 39 curated tools into \p registry.
///
/// All dependency objects (\p cfg, \p sidecar, \p mcp_registry,
/// \p supervisor, \p team_registry, \p skill_loader, \p plan_mode,
/// \p cfg_mutex) MUST outlive the ToolRegistry and all registered tools.
///
/// Aborts (via assert) if the final tool count is not exactly 39.
///
/// @param registry       Target ToolRegistry — must be empty on entry.
/// @param cfg            Runtime config (read for timeout/limits; written by ConfigTool).
/// @param cfg_mutex      Mutex protecting Config mutations (ConfigTool holds a ref).
/// @param sidecar        SidecarManager for WebFetch and WebSearch routing.
/// @param mcp_registry   McpServerRegistry for ListMcpResources, ReadMcpResource, MCP.
/// @param supervisor     AgentSupervisor for Task, TaskOutput, TaskStop, SendMessage, Workflow.
/// @param team_registry  TeamRegistry for TeamCreate, TeamDelete, ListPeers.
/// @param skill_loader   SkillLoader for the Skill tool.
/// @param plan_mode      PlanMode for EnterPlanMode, ExitPlanMode, VerifyPlanExecution.
/// @param confirm_fn     Optional TUI callback for ExitPlanMode approval.
///                       When non-null, ExitPlanMode shows a PlanApprovalCard
///                       instead of the broken stdin fallback.
///                       When null (default), ExitPlanMode uses stdin (TTY required).
/// @param prompt_fn      Optional TUI callback for AskUserQuestion.
///                       When non-null, AskUserQuestion posts a QuestionShow event
///                       and blocks via QuestionCard::await_user_answer() instead of
///                       falling back to the broken stdin path in raw-terminal mode.
///                       When null (default), AskUserQuestion uses stdin (TTY required).
void wire_tools(
    batbox::tools::ToolRegistry&                  registry,
    batbox::config::Config&                       cfg,
    std::mutex&                                   cfg_mutex,
    batbox::sidecar::SidecarManager&              sidecar,
    batbox::mcp::McpServerRegistry&               mcp_registry,
    batbox::agents::AgentSupervisor&              supervisor,
    batbox::agents::TeamRegistry&                 team_registry,
    batbox::plugins::SkillLoader&                 skill_loader,
    batbox::conversation::PlanMode&               plan_mode,
    batbox::tools::ExitPlanModeTool::ConfirmFn    confirm_fn = nullptr,
    batbox::tools::AskUserQuestionTool::PromptFn  prompt_fn  = nullptr);

} // namespace batbox::app
