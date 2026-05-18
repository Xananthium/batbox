# src/commands

Slash command implementations (one .cpp per command) and the SlashCommandRegistry.

All command .cpp files implement the ISlashCommand interface. Each is registered by WireCommands.cpp into the SlashCommandRegistry.

## Files

### SlashCommandRegistry.cpp
Lookup table implementation: hash map by name + alias map; fuzzy_find() using score_match(); all() returns insertion-order vector.

### AddDirCmd.cpp
`/add-dir` — adds a directory path to the Autocomplete extra_dirs list so its files appear in @ completions.

### AdvisorCmd.cpp
`/advisor` — toggles the built-in Demon advisor agent; sets CommandContext::advisor_mode.

### AgentsCmd.cpp
`/agents` — lists all loaded AgentSpec entries from AgentLoader; shows name, description, source path.

### ClearCmd.cpp
`/clear` — calls ConversationHandle::reset_messages(); resets the session context to empty.

### CompactCmd.cpp
`/compact` — calls Compactor::compact() on the current message list; installs the compacted vector via set_messages_json().

### ConfigCmd.cpp
`/config` — get/set/list config fields via ConfigTool logic; reads cfg under cfg_mutex.

### ContextCmd.cpp
`/context` — calls ContextWindow::estimate_tokens() on current messages; reports token count, context limit, and compaction threshold.

### CopyCmd.cpp
`/copy` — copies the last (or Nth) assistant message to the clipboard via xclip/pbcopy/wl-copy.

### CostCmd.cpp
`/cost` — reads UsageTracker::session_total(); formats and prints total prompt/completion tokens and USD cost.

### DemonCmd.cpp
`/demon` — toggles the DemonPanel visibility; calls DemonPanel::toggle().

### DiffCmd.cpp
`/diff` — shows a unified diff of specified file paths; launches DiffCard for review.

### EffortCmd.cpp
`/effort` — sets the model's effort level for o-series models (low/medium/high); writes to live Config.

### ExitCmd.cpp
`/exit` — sets CommandContext::exit_requested = true; triggers clean REPL shutdown.

### FilesCmd.cpp
`/files` — lists files in the current working directory; optionally filters by glob pattern.

### HelpCmd.cpp
`/help` — enumerates all registered commands via SlashCommandRegistry::all(); formats name + description table; shows usage detail for a named command.

### HooksCmd.cpp
`/hooks` — lists configured pre/post tool hooks from settings.json; shows hook name and trigger pattern.

### IdeCmd.cpp
`/ide` — generates IDE integration files (e.g. .vscode/mcp.json) for the current project.

### InitCmd.cpp
`/init` — creates BATBOX.md in cwd with a project context template for the model.

### KeybindingsCmd.cpp
`/keybindings` — displays current key bindings; optionally opens keybindings.json in the resolved editor.

### McpCmd.cpp
`/mcp` — lists MCP servers and their health state from McpServerRegistry; supports /mcp restart <server>.

### MemoryCmd.cpp
`/memory` — shows or edits BATBOX.md (or ~/.batbox/BATBOX.md); calls edit_string_in_editor().

### ModelCmd.cpp
`/model` — switches the active model by writing to Config::api.model; resolves aliases via resolve_model_alias().

### OutputStyleCmd.cpp
`/output-style` — toggles between plain, markdown, and JSON output styles for --print mode.

### PermissionsCmd.cpp
`/permissions` — lists allow/deny/ask rules from PermissionStore; supports add/remove subcommands that call add_*_rule()/remove_rule().

### PlanCmd.cpp
`/plan` — prints the current plan text when in Planning state; shows Inactive/Approved status otherwise.

### PluginCmd.cpp
`/plugin` — list/enable/disable/add-local/remove plugin operations via PluginLoader and PluginRegistry.

### ResumeCmd.cpp
`/resume` — calls SessionStore::list_recent(); prompts user to select a session; loads via SessionStore::load(); installs messages via set_messages_json().

### ReviewCmd.cpp
`/review` — runs a code review skill on specified files; injects the review prompt as a user message.

### SecurityReviewCmd.cpp
`/security-review` — runs a security-focused review skill on specified files; injects prompt as a user message.

### SessionCmd.cpp
`/session` — displays current session id, file path, turn count, model, and message count from ConversationHandle.

### SkillsCmd.cpp
`/skills` — lists all loaded skills from SkillLoader; shows name, description, and source (bundled/user/plugin).

### SlashCommandRegistry.cpp
SlashCommandRegistry lookup and fuzzy_find implementations.

### StatsCmd.cpp
`/stats` — prints session stats: token usage from UsageTracker, agent spawn count from AgentSupervisor, sidecar state.

### StatusCmd.cpp
`/status` — shows current status: permission mode, sidecar state, MCP server count, model name, agent count.

### TasksCmd.cpp
`/tasks` — lists tasks from TaskStore with optional status filter; supports /tasks create and /tasks update.

### TerminalSetupCmd.cpp
`/terminal-setup` — writes terminal-specific config snippets (e.g. kitty.conf Ctrl+Enter bindings) to the config dir.

### ThemeCmd.cpp
`/theme` — switches the active theme by name; calls load_theme() and updates the live Config.

### UsageCmd.cpp
`/usage` — prints token usage breakdown for the current session from UsageTracker.

### VimCmd.cpp
`/vim` — toggles VimMode on/off via VimMode::toggle(); writes the new state to settings.json.

### CMakeLists.txt
Build rules for the commands static library.
