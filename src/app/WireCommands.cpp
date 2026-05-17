// src/app/WireCommands.cpp
// =============================================================================
// wire_commands() — registers all 38 curated slash commands into
// SlashCommandRegistry at App::init time.
//
// Registration order follows curated-surface.md groupings:
//   Core UX → Model & Config → Display & Theme → Status & Stats →
//   Session Lifecycle → Memory & Context → Project Filesystem →
//   Code Review → Agents/Planning/Tasks → Permissions & Hooks →
//   Plugins & MCP → IDE & Editor → Misc → Party Monster Easter Egg
//
// Dependency injection:
//   Commands that depend on live subsystems (AgentSupervisor, PlanMode,
//   SkillLoader, PluginLoader/Registry, DemonPanel) receive raw pointers.
//   Null is safe — all such commands degrade gracefully.
//
// Fail-fast: assert(registry.size() == 38) fires at startup if any command
// is missing or double-registered.  This is an intentional programming-error
// trap, not a runtime-recoverable error.
// =============================================================================

#include <batbox/app/WireCommands.hpp>

#include <cassert>

// ---------------------------------------------------------------------------
// Forward declarations for all 38 command registration functions.
//
// Each function is defined in its own src/commands/XxxCmd.cpp translation
// unit.  No header files exist for individual commands — the register_xxx_cmd
// free functions are the public API, declared here to avoid creating 38
// thin header files just for one call site.
// ---------------------------------------------------------------------------
namespace batbox::commands {

// Group 1 — Core UX (4)
void register_help_cmd(SlashCommandRegistry&);
void register_exit_cmd(SlashCommandRegistry&);
void register_clear_cmd(SlashCommandRegistry&);
void register_init_cmd(SlashCommandRegistry&);

// Group 2 — Model & Config (3)
void register_model_cmd(SlashCommandRegistry&);
void register_config_cmd(SlashCommandRegistry&);
void register_effort_cmd(SlashCommandRegistry&);

// Group 3 — Display & Theme (2)
void register_theme_cmd(SlashCommandRegistry&);
void register_output_style_cmd(SlashCommandRegistry&);

// Group 4 — Status & Stats (4)
void register_status_cmd(SlashCommandRegistry&);
void register_stats_cmd(SlashCommandRegistry&);
void register_usage_cmd(SlashCommandRegistry&);
void register_cost_cmd(SlashCommandRegistry&);

// Group 5 — Session Lifecycle (3)
void register_resume_cmd(SlashCommandRegistry&);
void register_session_cmd(SlashCommandRegistry&);
void register_compact_cmd(SlashCommandRegistry&);

// Group 6 — Memory & Context (2)
void register_memory_cmd(SlashCommandRegistry&);
void register_context_cmd(SlashCommandRegistry&);

// Group 7 — Project Filesystem (3)
void register_add_dir_cmd(SlashCommandRegistry&);
void register_files_cmd(SlashCommandRegistry&);
void register_diff_cmd(SlashCommandRegistry&);

// Group 8 — Code Review (2)
void register_review_cmd(SlashCommandRegistry&);
void register_security_review_cmd(SlashCommandRegistry&);

// Group 9 — Agents / Planning / Tasks (4)
// /agents and /plan have overloads with dependency pointers.
void register_agents_cmd(SlashCommandRegistry&,
                          batbox::agents::AgentSupervisor*);
void register_plan_cmd(SlashCommandRegistry&,
                        batbox::conversation::PlanMode*);
void register_tasks_cmd(SlashCommandRegistry&);
void register_skills_cmd(SlashCommandRegistry&,
                          batbox::plugins::SkillLoader*);

// Group 10 — Permissions & Hooks (3)
void register_permissions_cmd(SlashCommandRegistry&);
void register_hooks_cmd(SlashCommandRegistry&);
void register_advisor_cmd(SlashCommandRegistry&);

// Group 11 — Plugins & MCP (2)
// /plugin overload takes PluginLoader* + PluginRegistry* for live mode.
void register_mcp_cmd(SlashCommandRegistry&);
void register_plugin_cmd(SlashCommandRegistry&,
                          batbox::plugins::PluginLoader*,
                          batbox::plugins::PluginRegistry*);

// Group 12 — IDE & Editor (4)
void register_ide_cmd(SlashCommandRegistry&);
void register_vim_cmd(SlashCommandRegistry&);
void register_keybindings_cmd(SlashCommandRegistry&);
void register_terminal_setup_cmd(SlashCommandRegistry&);

// Group 13 — Misc (1)
void register_copy_cmd(SlashCommandRegistry&);

// Group 14 — Party Monster Easter Egg (1)
// /demon overload takes DemonPanel* + AgentSupervisor* for live mode.
void register_demon_cmd(SlashCommandRegistry&,
                         batbox::tui::DemonPanel*,
                         batbox::agents::AgentSupervisor*);

} // namespace batbox::commands

namespace batbox::app {

void wire_commands(
    batbox::commands::SlashCommandRegistry& registry,
    batbox::agents::AgentSupervisor*        supervisor,
    batbox::conversation::PlanMode*         plan_mode,
    batbox::plugins::SkillLoader*           skill_loader,
    batbox::plugins::PluginLoader*          plugin_loader,
    batbox::plugins::PluginRegistry*        plugin_registry,
    batbox::tui::DemonPanel*                demon_panel)
{
    using namespace batbox::commands;

    // -------------------------------------------------------------------------
    // Group 1 — Core UX (4 commands)
    // Zero external dependencies — register unconditionally.
    // -------------------------------------------------------------------------

    // 1. /help — list all commands grouped by category
    register_help_cmd(registry);

    // 2. /exit — end the session and quit the REPL
    register_exit_cmd(registry);

    // 3. /clear — clear terminal output; does NOT reset conversation context
    register_clear_cmd(registry);

    // 4. /init — initialise a CLAUDE.md in the current project directory
    register_init_cmd(registry);

    // -------------------------------------------------------------------------
    // Group 2 — Model & Config (3 commands)
    // Zero external dependencies.
    // -------------------------------------------------------------------------

    // 5. /model — switch the active Claude model for this session
    register_model_cmd(registry);

    // 6. /config — read or write BATBOX_* config fields
    register_config_cmd(registry);

    // 7. /effort — set the thinking-effort level (low / medium / high)
    register_effort_cmd(registry);

    // -------------------------------------------------------------------------
    // Group 3 — Display & Theme (2 commands)
    // Zero external dependencies.
    // -------------------------------------------------------------------------

    // 8. /theme — switch colour palette (dark / light / dracula / etc.)
    register_theme_cmd(registry);

    // 9. /output-style — set response verbosity (concise / normal / verbose)
    register_output_style_cmd(registry);

    // -------------------------------------------------------------------------
    // Group 4 — Status & Stats (4 commands)
    // Zero external dependencies.
    // -------------------------------------------------------------------------

    // 10. /status — show API key status, model, and permission mode
    register_status_cmd(registry);

    // 11. /stats — show token usage and cost for the current session
    register_stats_cmd(registry);

    // 12. /usage — show cumulative token usage across all sessions
    register_usage_cmd(registry);

    // 13. /cost — show estimated cost for the current session
    register_cost_cmd(registry);

    // -------------------------------------------------------------------------
    // Group 5 — Session Lifecycle (3 commands)
    // Zero external dependencies.
    // -------------------------------------------------------------------------

    // 14. /resume — resume a previous session by ID or list recent sessions
    register_resume_cmd(registry);

    // 15. /session — show, rename, or delete the current session
    register_session_cmd(registry);

    // 16. /compact — summarise and compress conversation history
    register_compact_cmd(registry);

    // -------------------------------------------------------------------------
    // Group 6 — Memory & Context (2 commands)
    // Zero external dependencies.
    // -------------------------------------------------------------------------

    // 17. /memory — inspect or edit CLAUDE.md project memory
    register_memory_cmd(registry);

    // 18. /context — show token count and context window utilisation
    register_context_cmd(registry);

    // -------------------------------------------------------------------------
    // Group 7 — Project Filesystem (3 commands)
    // Zero external dependencies.
    // -------------------------------------------------------------------------

    // 19. /add-dir — add a directory to the project context
    register_add_dir_cmd(registry);

    // 20. /files — list files in the current project context
    register_files_cmd(registry);

    // 21. /diff — show a unified diff of staged or unstaged changes
    register_diff_cmd(registry);

    // -------------------------------------------------------------------------
    // Group 8 — Code Review (2 commands)
    // Zero external dependencies.
    // -------------------------------------------------------------------------

    // 22. /review — run a full code review on staged changes
    register_review_cmd(registry);

    // 23. /security-review — security-focused audit of the current diff
    register_security_review_cmd(registry);

    // -------------------------------------------------------------------------
    // Group 9 — Agents / Planning / Tasks (4 commands)
    // supervisor, plan_mode, skill_loader may be nullptr; commands degrade.
    // -------------------------------------------------------------------------

    // 24. /agents — list running sub-agents and their status
    register_agents_cmd(registry, supervisor);

    // 25. /plan — enter plan-mode or show the current plan
    register_plan_cmd(registry, plan_mode);

    // 26. /tasks — list, create, or update tasks in tasks.json
    register_tasks_cmd(registry);

    // 27. /skills — list or run bundled skills via SkillLoader
    register_skills_cmd(registry, skill_loader);

    // -------------------------------------------------------------------------
    // Group 10 — Permissions & Hooks (3 commands)
    // Zero external dependencies.
    // -------------------------------------------------------------------------

    // 28. /permissions — show or set the active permission mode
    register_permissions_cmd(registry);

    // 29. /hooks — list, enable, or disable lifecycle hooks
    register_hooks_cmd(registry);

    // 30. /advisor — toggle the advisor overlay for command suggestions
    register_advisor_cmd(registry);

    // -------------------------------------------------------------------------
    // Group 11 — Plugins & MCP (2 commands)
    // McpCmd has zero external deps (reads mcp_registry at call time via ctx).
    // PluginCmd receives PluginLoader* + PluginRegistry*; null → degraded mode.
    // -------------------------------------------------------------------------

    // 31. /mcp — list MCP servers or invoke a server method
    register_mcp_cmd(registry);

    // 32. /plugin — list, enable, disable, reload, add, or remove plugins
    register_plugin_cmd(registry, plugin_loader, plugin_registry);

    // -------------------------------------------------------------------------
    // Group 12 — IDE & Editor (4 commands)
    // Zero external dependencies.
    // -------------------------------------------------------------------------

    // 33. /ide — Phase-2 IDE integration status and configuration
    register_ide_cmd(registry);

    // 34. /vim — toggle vim-mode key bindings in the REPL
    register_vim_cmd(registry);

    // 35. /keybindings — show or customise key binding assignments
    register_keybindings_cmd(registry);

    // 36. /terminal-setup — print shell integration setup instructions
    register_terminal_setup_cmd(registry);

    // -------------------------------------------------------------------------
    // Group 13 — Misc (1 command)
    // Zero external dependencies.
    // -------------------------------------------------------------------------

    // 37. /copy — copy the last assistant message to the clipboard
    register_copy_cmd(registry);

    // -------------------------------------------------------------------------
    // Group 14 — Party Monster Easter Egg (1 command)
    // DemonCmd receives DemonPanel* + AgentSupervisor*; null → degraded mode.
    // -------------------------------------------------------------------------

    // 38. /demon — summon the Party Monster demon panel (easter egg)
    register_demon_cmd(registry, demon_panel, supervisor);

    // -------------------------------------------------------------------------
    // Fail-fast assertion: exactly 38 commands must be registered.
    // A miscount here is a programming error; abort at startup rather than
    // silently shipping an incomplete command surface.
    // -------------------------------------------------------------------------
    assert(registry.size() == 38 &&
           "wire_commands: expected exactly 38 commands — "
           "a command was added or removed without updating this count");
}

} // namespace batbox::app
