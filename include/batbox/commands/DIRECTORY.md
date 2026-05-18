# include/batbox/commands

Slash command interface and registry headers.

## Files

### ISlashCommand.hpp
Pure-virtual interface all slash commands implement.

- `ISlashCommand::name() -> string_view` — returns the primary slash command name (e.g. "clear")
- `ISlashCommand::description() -> string_view` — returns the one-line description shown by /help
- `ISlashCommand::aliases() -> vector<string_view>` — returns alternate names that invoke this command
- `ISlashCommand::usage() -> string_view` — returns the usage synopsis shown in /help detail view
- `ISlashCommand::execute(args, ctx) -> void` — runs the command; writes output to ctx.output
- `ISlashCommand::requires_args() -> bool` — returns true when the command errors without arguments
- `ISlashCommand::phase() -> CommandPhase` — returns Phase1 (startup) or Phase2 (runtime); Phase1 commands run before the TUI is ready

### SlashCommandRegistry.hpp
Lookup table for slash commands by name and alias.

- `SlashCommandRegistry::register_command(cmd)` — registers a shared_ptr<ISlashCommand>; throws on duplicate name
- `SlashCommandRegistry::lookup(name_or_alias) -> shared_ptr<ISlashCommand>` — finds command by name or alias; returns nullptr if not found
- `SlashCommandRegistry::fuzzy_find(query, k=10) -> vector<shared_ptr<ISlashCommand>>` — returns up to k commands whose names fuzzy-match query, ranked by score
- `SlashCommandRegistry::all() -> vector<shared_ptr<ISlashCommand>>` — returns all registered commands in insertion order
- `SlashCommandRegistry::names() -> vector<string>` — returns all primary command names
- `SlashCommandRegistry::size() -> size_t` — returns count of registered commands
