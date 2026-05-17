// include/batbox/commands/ISlashCommand.hpp
//
// batbox::commands::ISlashCommand — pure-virtual interface that every slash
// command implements.
//
// Design notes
// ------------
// All 38 slash commands share this contract.  The registry holds
// `std::shared_ptr<ISlashCommand>` so commands can own state (e.g.
// /model remembers the last picker selection).
//
// Phase
// -----
// Most commands are Phase-1 (available at MVP launch).
// Phase-2 commands are present in the registry so they appear in the palette
// and autocomplete, but their `execute()` returns a friendly "coming soon"
// error rather than crashing.  Callers must NOT special-case Phase-2 —
// the command handles it internally via `execute()`.
//
// Decision of Record #10: /ide is Phase-2.
//
// CommandContext
// --------------
// A lightweight pass-by-reference bag of live application state.  It is
// forward-declared here (full definition lives in App / Repl layer, task
// CPP 2.6 / CPP A.3) so this header stays dependency-free.
//
// aliases()
// ---------
// Commands may expose short-form aliases (e.g. /q for /exit).  The
// SlashCommandRegistry builds a reverse map keyed by each alias.  Aliases
// must be globally unique — the registry enforces this.
//
// requires_args()
// ---------------
// Hint for the InputBar palette: when true the user is prompted for an
// argument string before dispatch.  When false the command fires immediately
// on selection.

#pragma once

#include <batbox/core/Result.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace batbox::commands {

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

struct CommandContext;  // defined in include/batbox/repl/CommandContext.hpp (CPP 2.6)

// ---------------------------------------------------------------------------
// Phase enum
// ---------------------------------------------------------------------------

/// Deployment phase for a slash command.
/// Phase1 = available at MVP.
/// Phase2 = registered/visible but execute() returns a not-implemented notice.
enum class CommandPhase {
    Phase1,  ///< Fully implemented — normal dispatch.
    Phase2,  ///< Stub — execute() returns a "not yet available" message.
};

// ---------------------------------------------------------------------------
// ISlashCommand interface
// ---------------------------------------------------------------------------

class ISlashCommand {
public:
    virtual ~ISlashCommand() = default;

    // ---- Identity -----------------------------------------------------------

    /// Primary name used to invoke the command (without leading slash).
    /// Must be lowercase, non-empty, no whitespace.
    /// Example: "help", "exit", "ide"
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /// One-line description shown in the `/` palette and `/help` output.
    [[nodiscard]] virtual std::string_view description() const noexcept = 0;

    /// Optional short-form aliases (without leading slash).
    /// Default: empty vector (no aliases).
    /// Example: {"q", "quit"} for the /exit command.
    [[nodiscard]] virtual std::vector<std::string> aliases() const { return {}; }

    /// Usage string shown when the user runs the command with bad arguments.
    /// Should include the leading slash.
    /// Example: "/model [model-name]"
    [[nodiscard]] virtual std::string_view usage() const noexcept = 0;

    // ---- Dispatch -----------------------------------------------------------

    /// Execute the command.
    ///
    /// @param args  Everything typed after the command name, leading whitespace
    ///              stripped.  Empty string_view when no argument was given.
    /// @param ctx   Mutable reference to the live application context.
    ///
    /// @returns     Ok on success.
    ///              Err(message) on user-visible error (printed by the caller).
    ///
    /// Phase-2 commands return Err("/<name> is not available in this build.
    /// It will ship in a future release.").
    [[nodiscard]] virtual batbox::Result<void> execute(
        std::string_view     args,
        CommandContext&      ctx) = 0;

    // ---- Metadata -----------------------------------------------------------

    /// True when the command requires a non-empty argument string.
    /// The InputBar uses this to decide whether to open an argument-input
    /// prompt when the command is selected from the palette.
    [[nodiscard]] virtual bool requires_args() const noexcept { return false; }

    /// Deployment phase of this command.
    /// Phase2 commands are registered and visible but not yet fully implemented.
    [[nodiscard]] virtual CommandPhase phase() const noexcept { return CommandPhase::Phase1; }
};

} // namespace batbox::commands
