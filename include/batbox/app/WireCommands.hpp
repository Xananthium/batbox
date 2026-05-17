// include/batbox/app/WireCommands.hpp
// =============================================================================
// wire_commands() — registers all 38 curated slash commands into
// SlashCommandRegistry at App::init time.
//
// Blueprint contract (task CPP S.15):
//   function  batbox::app::wire_commands
//   file      include/batbox/app/WireCommands.hpp
//   file      src/app/WireCommands.cpp
//
// Registration groups (following curated-surface.md order):
//   1. Core UX (4):        /help, /exit, /clear, /init
//   2. Model & Config (3): /model, /config, /effort
//   3. Display & Theme (2): /theme, /output-style
//   4. Status & Stats (4): /status, /stats, /usage, /cost
//   5. Session Lifecycle (3): /resume, /session, /compact
//   6. Memory & Context (2): /memory, /context
//   7. Project Filesystem (3): /add-dir, /files, /diff
//   8. Code Review (2):    /review, /security-review
//   9. Agents/Planning/Tasks (4): /agents, /plan, /tasks, /skills
//  10. Permissions & Hooks (3): /permissions, /hooks, /advisor
//  11. Plugins & MCP (2):  /mcp, /plugin
//  12. IDE & Editor (4):   /ide, /vim, /keybindings, /terminal-setup
//  13. Misc (1):           /copy
//  14. Party Monster Easter Egg (1): /demon
//
// Fail-fast contract:
//   After registration, assert(registry.size() == 38) fires at startup if any
//   command was accidentally omitted or duplicated.
//
// Dependency injection:
//   Commands that require live subsystems receive raw pointers.  All deps
//   MUST outlive the SlashCommandRegistry and all registered commands.
//   Null pointers are safe (commands degrade gracefully in headless/test mode).
// =============================================================================

#pragma once

#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/agents/AgentSupervisor.hpp>
#include <batbox/conversation/PlanMode.hpp>
#include <batbox/plugins/SkillLoader.hpp>
#include <batbox/plugins/PluginLoader.hpp>
#include <batbox/plugins/PluginRegistry.hpp>
#include <batbox/tui/DemonPanel.hpp>

namespace batbox::app {

/// Register all 38 curated slash commands into \p registry.
///
/// Commands with external dependencies receive raw pointers.  A null pointer
/// means the dependency is not yet available (Phase-2 subsystem not wired);
/// in that case the command degrades gracefully (prints an "unavailable"
/// message rather than crashing).
///
/// All dependency objects MUST outlive the SlashCommandRegistry.
///
/// Aborts (via assert) if the final command count is not exactly 38.
///
/// @param registry       Target SlashCommandRegistry — must be empty on entry.
/// @param supervisor     AgentSupervisor for /agents.  May be nullptr.
/// @param plan_mode      PlanMode for /plan.  May be nullptr.
/// @param skill_loader   SkillLoader for /skills.  May be nullptr.
/// @param plugin_loader  PluginLoader for /plugin.  May be nullptr.
/// @param plugin_registry PluginRegistry for /plugin.  May be nullptr.
/// @param demon_panel    DemonPanel for /demon.  May be nullptr.
void wire_commands(
    batbox::commands::SlashCommandRegistry& registry,
    batbox::agents::AgentSupervisor*        supervisor,
    batbox::conversation::PlanMode*         plan_mode,
    batbox::plugins::SkillLoader*           skill_loader,
    batbox::plugins::PluginLoader*          plugin_loader,
    batbox::plugins::PluginRegistry*        plugin_registry,
    batbox::tui::DemonPanel*                demon_panel);

} // namespace batbox::app
