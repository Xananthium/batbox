// src/app/WireTools.cpp
// =============================================================================
// wire_tools() — registers all 39 curated tools into ToolRegistry at App init.
//
// Registration order rationale:
//   - Read-only, zero-dep tools first (no external refs to worry about).
//   - Config-dependent tools after config is fully loaded.
//   - Shared-state tools after their backing stores are constructed.
//   - MCP tools after McpServerRegistry is ready.
//   - ToolSearch AFTER all other tools so its registry reference is populated.
//   - Agent-family tools after AgentSupervisor and TeamRegistry are ready.
//   - Skill tool after SkillLoader is populated.
//   - AskUserQuestion last (no deps; optionally uses PromptFn for TUI mode).
//
// Fail-fast: assert(registry.size() == 39) fires at startup if any tool is
// missing or double-registered.  This is an intentional programming-error trap,
// not a runtime recoverable error.
// =============================================================================

#include <batbox/app/WireTools.hpp>

// S1+S4 (DIS-980): closed tool-subagent distillation install hook.
#include <batbox/tools/SubagentDistiller.hpp>

// Read-only / zero-dep tools
#include <batbox/tools/ReadTool.hpp>
#include <batbox/tools/WriteTool.hpp>
#include <batbox/tools/EditTool.hpp>
#include <batbox/tools/GlobTool.hpp>
#include <batbox/tools/GrepTool.hpp>
#include <batbox/tools/TodoWriteTool.hpp>
#include <batbox/tools/SleepTool.hpp>
#include <batbox/tools/SnipTool.hpp>
#include <batbox/tools/PowerShellTool.hpp>
#include <batbox/tools/CtxInspectTool.hpp>

// Config-dependent tools
#include <batbox/tools/BashTool.hpp>
#include <batbox/tools/WebFetchTool.hpp>
#include <batbox/tools/WebSearchTool.hpp>
#include <batbox/tools/RemoteTriggerTool.hpp>
#include <batbox/tools/ConfigTool.hpp>

// Shared-state tools — TaskStore family
#include <batbox/tools/TaskStore.hpp>
#include <batbox/tools/TaskCreateTool.hpp>
#include <batbox/tools/TaskListTool.hpp>
#include <batbox/tools/TaskGetTool.hpp>
#include <batbox/tools/TaskUpdateTool.hpp>

// Shared-state tools — CronScheduler family
#include <batbox/tools/CronScheduler.hpp>
#include <batbox/tools/CronCreateTool.hpp>
#include <batbox/tools/CronDeleteTool.hpp>
#include <batbox/tools/CronListTool.hpp>

// PlanMode family
#include <batbox/tools/EnterPlanModeTool.hpp>
#include <batbox/tools/ExitPlanModeTool.hpp>
#include <batbox/tools/VerifyPlanExecutionTool.hpp>

// MCP family
#include <batbox/tools/ListMcpResourcesTool.hpp>
#include <batbox/tools/ReadMcpResourceTool.hpp>
#include <batbox/tools/McpTool.hpp>

// ToolSearch (needs populated registry — registered after all others)
#include <batbox/tools/ToolSearchTool.hpp>

// Agent-family tools
#include <batbox/tools/TaskTool.hpp>
#include <batbox/tools/TaskOutputTool.hpp>
#include <batbox/tools/TaskStopTool.hpp>
#include <batbox/tools/SendMessageTool.hpp>
#include <batbox/tools/TeamCreateTool.hpp>
#include <batbox/tools/TeamDeleteTool.hpp>
#include <batbox/tools/ListPeersTool.hpp>
#include <batbox/tools/WorkflowTool.hpp>

// Skill tool
#include <batbox/tools/SkillTool.hpp>

// AskUserQuestion
#include <batbox/tools/AskUserQuestionTool.hpp>

#include <cassert>
#include <memory>

namespace batbox::app {

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
    batbox::tools::ExitPlanModeTool::ConfirmFn    confirm_fn,
    batbox::tools::AskUserQuestionTool::PromptFn  prompt_fn)
{
    using namespace batbox::tools;

    // -------------------------------------------------------------------------
    // Group 1 — Read-only / zero-dep tools (10 tools)
    // These have no external references; safe to register immediately.
    // -------------------------------------------------------------------------

    // 1. Read — read file content with optional offset/limit (cat -n format)
    registry.register_tool(std::make_unique<ReadTool>());

    // 2. Write — create/overwrite file atomically; create parent dirs
    registry.register_tool(std::make_unique<WriteTool>());

    // 3. Edit — exact string match-and-replace; atomic write; unified diff
    registry.register_tool(std::make_unique<EditTool>());

    // 4. Glob — recursive filesystem glob; sorted file list
    registry.register_tool(std::make_unique<GlobTool>());

    // 5. Grep — regex search in files; ripgrep wrapper or pure C++
    registry.register_tool(std::make_unique<GrepTool>());

    // 6. TodoWrite — append to ./BATBOX.md project memory
    registry.register_tool(std::make_unique<TodoWriteTool>());

    // 7. Sleep — interruptible sleep with CancelToken support
    registry.register_tool(std::make_unique<SleepTool>());

    // 8. Snip — copy text snippet to clipboard or save to ~/.batbox/snips/
    registry.register_tool(std::make_unique<SnipTool>());

    // 9. PowerShell — no-op on macOS/Linux; returns platform-unsupported error
    registry.register_tool(std::make_unique<PowerShellTool>());

    // 10. CtxInspect — token count + model limit + pct_used (read-only)
    registry.register_tool(std::make_unique<CtxInspectTool>());

    // -------------------------------------------------------------------------
    // Group 2 — Config-dependent tools (5 tools)
    // Pull timeout/limit values from cfg to avoid duplicating defaults here.
    // -------------------------------------------------------------------------

    // 11. Bash — pty/pipe backend; watchdog timeout from config
    registry.register_tool(std::make_unique<BashTool>(
        cfg.tools.bash_timeout_sec,
        static_cast<std::size_t>(cfg.tools.bash_max_output_bytes)));

    // 12. WebFetch — routes through SidecarManager /fetch
    registry.register_tool(std::make_unique<WebFetchTool>(
        sidecar,
        cfg.search.webfetch_timeout_sec,
        cfg.search.webfetch_max_bytes));

    // 13. WebSearch — routes through SidecarManager /search (ddg or searxng)
    registry.register_tool(std::make_unique<WebSearchTool>(cfg, sidecar));

    // 14. RemoteTrigger — POST to configured trigger_url with payload
    //     Allowed URLs are empty at registration time; callers provide at runtime
    //     via the tool args. The tool validates the target URL per-call.
    registry.register_tool(std::make_unique<RemoteTriggerTool>());

    // 15. Config — read/write BATBOX_* config fields; triggers reload on set
    registry.register_tool(std::make_unique<ConfigTool>(cfg, cfg_mutex));

    // -------------------------------------------------------------------------
    // Group 3 — TaskStore family (4 tools)
    // All four CRUD tools share one TaskStore backed by ~/.batbox/tasks.json.
    // -------------------------------------------------------------------------
    auto task_store = std::make_shared<batbox::tools::TaskStore>(
        cfg.general.config_dir / "tasks.json");

    // 16. TaskCreate — persist task with id/title/status/due to tasks.json
    registry.register_tool(std::make_unique<TaskCreateTool>(task_store));

    // 17. TaskList — list tasks; is_read_only()=true
    registry.register_tool(std::make_unique<TaskListTool>(task_store));

    // 18. TaskGet — retrieve single task by id; is_read_only()=true
    registry.register_tool(std::make_unique<TaskGetTool>(task_store));

    // 19. TaskUpdate — partial update of task fields; atomic write
    registry.register_tool(std::make_unique<TaskUpdateTool>(task_store));

    // -------------------------------------------------------------------------
    // Group 4 — CronScheduler family (3 tools)
    // All three share one CronScheduler backed by ~/.batbox/cron.json.
    // The scheduler's background thread is started inside the CronScheduler
    // constructor; it polls every 1 s and is joined in ~CronScheduler().
    // -------------------------------------------------------------------------
    auto cron_scheduler = std::make_shared<batbox::tools::CronScheduler>(
        cfg.general.config_dir / "cron.json");

    // 20. CronCreate — schedule repeating task; scheduler thread checks every 1 s
    registry.register_tool(std::make_unique<CronCreateTool>(cron_scheduler));

    // 21. CronDelete — remove cron entry by id
    registry.register_tool(std::make_unique<CronDeleteTool>(cron_scheduler));

    // 22. CronList — list active cron entries; is_read_only()=true
    registry.register_tool(std::make_unique<CronListTool>(cron_scheduler));

    // -------------------------------------------------------------------------
    // Group 5 — PlanMode family (3 tools)
    // All three reference the same PlanMode instance owned by Conversation.
    // -------------------------------------------------------------------------

    // 23. EnterPlanMode — transition Conversation to Planning state
    registry.register_tool(std::make_unique<EnterPlanModeTool>(plan_mode));

    // 24. ExitPlanMode — presents plan for user approval; transitions Approved.
    // When confirm_fn is wired (TUI mode), ExitPlanMode shows a PlanApprovalCard
    // modal instead of the broken stdin fallback.  In headless/stdin mode,
    // confirm_fn is null and the TTY stdin path is used (or an error is returned).
    if (confirm_fn) {
        registry.register_tool(std::make_unique<ExitPlanModeTool>(
            plan_mode, std::move(confirm_fn)));
    } else {
        registry.register_tool(std::make_unique<ExitPlanModeTool>(plan_mode));
    }

    // 25. VerifyPlanExecution — check plan against actual execution in Approved
    registry.register_tool(std::make_unique<VerifyPlanExecutionTool>(plan_mode));

    // -------------------------------------------------------------------------
    // Group 6 — MCP family (3 tools)
    // All three proxy through McpServerRegistry which owns transport lifecycle.
    // -------------------------------------------------------------------------

    // 26. ListMcpResources — list resources from named MCP server; read-only
    registry.register_tool(std::make_unique<ListMcpResourcesTool>(mcp_registry));

    // 27. ReadMcpResource — read specific resource from named MCP server; read-only
    registry.register_tool(std::make_unique<ReadMcpResourceTool>(mcp_registry));

    // 28. MCP — generic proxy: {server, method, params} → McpClient::request
    registry.register_tool(std::make_unique<McpTool>(mcp_registry));

    // -------------------------------------------------------------------------
    // Group 7 — Agent-family tools (8 tools)
    // Task/TaskOutput/TaskStop/SendMessage/Workflow all need AgentSupervisor.
    // TeamCreate/TeamDelete/ListPeers need TeamRegistry.
    // -------------------------------------------------------------------------

    // 29. Task — spawn sub-agent; returns agent_id immediately
    registry.register_tool(std::make_unique<TaskTool>(supervisor));

    // 30. TaskOutput — poll agent state snapshot; is_read_only()=true
    registry.register_tool(std::make_unique<TaskOutputTool>(supervisor));

    // 31. TaskStop — cancel running sub-agent by agent_id
    registry.register_tool(std::make_unique<TaskStopTool>(supervisor));

    // 32. SendMessage — enqueue message into peer agent input queue
    registry.register_tool(std::make_unique<SendMessageTool>(supervisor));

    // 33. TeamCreate — create named agent team with members + blackboard
    registry.register_tool(std::make_unique<TeamCreateTool>(team_registry));

    // 34. TeamDelete — disband named team; cancel member agents
    registry.register_tool(std::make_unique<TeamDeleteTool>(team_registry));

    // 35. ListPeers — list active team members visible to caller; read-only
    registry.register_tool(std::make_unique<ListPeersTool>(team_registry));

    // 36. Workflow — execute a DAG of agent steps via Workflow engine
    registry.register_tool(std::make_unique<WorkflowTool>(supervisor));

    // -------------------------------------------------------------------------
    // Group 8 — Skill tool (1 tool)
    // SkillLoader must be populated (scan_directories() called) before this.
    // -------------------------------------------------------------------------

    // 37. Skill — invoke named skill via SkillLoader; inject prompt + run script
    registry.register_tool(std::make_unique<SkillTool>(skill_loader));

    // -------------------------------------------------------------------------
    // Group 9 — AskUserQuestion (1 tool)
    // When prompt_fn is wired (TUI mode), AskUserQuestion posts a QuestionShow
    // event and blocks via QuestionCard::await_user_answer() — the correct path
    // when FTXUI owns the terminal in raw mode.
    // When prompt_fn is null (headless/test mode), AskUserQuestion falls back to
    // the stdin path (errors in non-TTY environments as documented).
    // -------------------------------------------------------------------------

    // 38. AskUserQuestion — pause agent; surface question modal to user
    if (prompt_fn) {
        registry.register_tool(std::make_unique<AskUserQuestionTool>(
            std::move(prompt_fn)));
    } else {
        registry.register_tool(std::make_unique<AskUserQuestionTool>());
    }

    // -------------------------------------------------------------------------
    // Group 10 — ToolSearch (1 tool)
    // MUST be registered last: it holds a const-ref to registry and queries
    // the name+description of every already-registered tool.
    // -------------------------------------------------------------------------

    // 39. ToolSearch — fuzzy search over registry name+description
    registry.register_tool(std::make_unique<ToolSearchTool>(registry));

    // -------------------------------------------------------------------------
    // Fail-fast assertion: exactly 39 tools must be registered.
    // A miscount here is a programming error; abort at startup rather than
    // silently shipping an incomplete tool surface.
    // -------------------------------------------------------------------------
    assert(registry.size() == 39 &&
           "wire_tools: expected exactly 39 tools — "
           "a tool was added or removed without updating this count");

    // -------------------------------------------------------------------------
    // S1+S4 (DIS-980) — closed tool-subagent distillation.
    //
    // Install the threshold engulf decider + the subagent distiller into the
    // registry's existing ToolSubagentEnvelope (the S7 seam, unmodified).  This
    // is a no-op unless cfg.distill.enabled is true, so the default tool surface
    // is byte-identical to S7.  report_gold is the distiller's INTERNAL contract
    // (the main model never calls it) and is therefore deliberately NOT one of
    // the 39 curated tools — the count assertion above is unaffected.
    // -------------------------------------------------------------------------
    install_subagent_distillation(registry, cfg);
}

} // namespace batbox::app
