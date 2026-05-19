// include/batbox/commands/SlashCommand.hpp
//
// batbox::commands::SlashCommand — plain-data record describing a built-in
// slash command.
//
// K1-a context (PEXT3 2.1)
// ------------------------
// This is the first step of a multi-wave migration away from the
// `ISlashCommand` virtual-dispatch interface toward a data-abstraction design:
//
//   K1-a  (this file) — introduce SlashCommand POD + adapter shim so the
//                        registry accepts both registration styles; no built-in
//                        Cmd files are changed yet.
//   K1-b … K1-N       — migrate one command category per wave (38 built-ins
//                        across 13 categories from HelpCmd::kCategories).
//   K1-z              — retire ISlashCommand for built-ins once all have
//                        migrated; keep it ONLY for the plugin loader path
//                        (src/plugins/CommandLoader.cpp → UserSlashCommand).
//
// Design
// ------
// SlashCommand is a plain struct — no constructors, no methods, no virtual
// dispatch.  Metadata fields are std::string_view pointing into string
// literals (compile-time stable, zero allocation).  aliases is a
// std::span<const std::string_view> over a constexpr array — no per-call
// vector materialisation.  The execute field is a plain function pointer;
// no std::function, no captures needed for built-in commands.
//
// Usage pattern in a *Cmd.cpp file (post-migration):
//
//   namespace batbox::commands {
//   Result<void> execute_exit_cmd(std::string_view, CommandContext& ctx) {
//       ctx.exit_requested = true;
//       return {};
//   }
//   constexpr std::string_view kExitAliases[] = { "quit", "q" };
//   constexpr SlashCommand kExitCmd = {
//       .name        = "exit",
//       .description = "Exit batbox, flush session, and shut down cleanly.",
//       .usage       = "/exit",
//       .aliases     = kExitAliases,
//       .execute     = &execute_exit_cmd,
//   };
//   void register_exit_cmd(SlashCommandRegistry& r) {
//       (void)r.register_command(&kExitCmd);
//   }
//   } // namespace
//
// Registration of plugins keeps the ISlashCommand path; the adapter shim in
// SlashCommandRegistry.cpp translates POD registrations into ISlashCommand
// objects at insertion time, keeping the lookup path uniform.

#pragma once

#include <batbox/commands/ISlashCommand.hpp>  // CommandPhase, CommandContext fwd-decl
#include <batbox/core/Result.hpp>

#include <span>
#include <string_view>

namespace batbox::commands {

// CommandContext is forward-declared in ISlashCommand.hpp (via
// include/batbox/repl/CommandContext.hpp pull-in from SlashCommandRegistry.hpp
// consumers); it is also defined there.  We only need it as a reference
// parameter in the execute function pointer signature below — no full
// definition required at this point.

// ---------------------------------------------------------------------------
// SlashCommand POD
// ---------------------------------------------------------------------------

/// Plain-data record that describes a built-in slash command.
///
/// All pointer/view fields must point to storage with at least registry
/// lifetime — constexpr string literals and constexpr arrays satisfy this.
///
/// The execute field must not be null; the registry rejects null-execute PODs.
struct SlashCommand {
    /// Primary name used to invoke the command (without leading slash).
    /// Must be lowercase, non-empty.  Example: "exit"
    std::string_view name;

    /// One-line description shown in the palette and /help output.
    std::string_view description;

    /// Usage string shown on bad-argument errors.  Should include the slash.
    /// Example: "/model [model-name]"
    std::string_view usage;

    /// Optional short-form aliases (without leading slash).
    /// Point at a constexpr array or leave defaulted (empty span).
    std::span<const std::string_view> aliases = {};

    /// True when the command requires a non-empty argument string.
    bool requires_args = false;

    /// Deployment phase.  Phase2 commands are visible but not yet callable.
    CommandPhase phase = CommandPhase::Phase1;

    /// Execute the command.  Must not be null.
    ///
    /// @param args  Everything typed after the command name (leading whitespace
    ///              stripped).  Empty string_view when no argument was given.
    /// @param ctx   Mutable reference to the live application context.
    /// @returns     Ok on success; Err(message) on user-visible error.
    batbox::Result<void> (*execute)(std::string_view args, CommandContext&) = nullptr;
};

} // namespace batbox::commands
