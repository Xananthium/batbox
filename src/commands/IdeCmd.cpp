// src/commands/IdeCmd.cpp
//
// batbox::commands::IdeCmd — implements the /ide slash command.
//
// /ide is a Phase-2 command: it is registered in the palette and autocomplete
// so users can discover it, but execute() returns a friendly "not yet
// available" notice rather than doing real IDE integration work.
//
// Decision of Record #10: /ide is Phase-2.
//
// Registration entry point:
//   void register_ide_cmd(SlashCommandRegistry&);

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/repl/CommandContext.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace batbox::commands {

// ---------------------------------------------------------------------------
// IdeCmd
// ---------------------------------------------------------------------------

class IdeCmd final : public ISlashCommand {
public:
    // ---- Identity -----------------------------------------------------------

    [[nodiscard]] std::string_view name() const noexcept override {
        return "ide";
    }

    [[nodiscard]] std::string_view description() const noexcept override {
        return "IDE bridge integration (Phase 2 — not yet available).";
    }

    [[nodiscard]] std::string_view usage() const noexcept override {
        return "/ide";
    }

    [[nodiscard]] std::vector<std::string> aliases() const override {
        return {};
    }

    [[nodiscard]] bool requires_args() const noexcept override { return false; }

    /// This command is Phase-2: visible in the palette but execute() returns
    /// a user-friendly notice.
    [[nodiscard]] CommandPhase phase() const noexcept override {
        return CommandPhase::Phase2;
    }

    // ---- Execute ------------------------------------------------------------

    [[nodiscard]] batbox::Result<void> execute(
        std::string_view   /*args*/,
        CommandContext&    ctx) override;
};

// ---------------------------------------------------------------------------
// execute
//
// Writes the Phase-2 notice to ctx.output and returns Ok (not Err) so the
// notice is printed as normal command output rather than as an error.
// ---------------------------------------------------------------------------

batbox::Result<void> IdeCmd::execute(
    std::string_view /*args*/,
    CommandContext&  ctx)
{
    ctx.output
        << "\n"
        << "  /ide — IDE bridge integration\n"
        << "\n"
        << "  Status: Phase 2 — not yet available in this build.\n"
        << "\n"
        << "  The IDE bridge will allow batbox to:\n"
        << "    • Open files in your configured editor at a specific line\n"
        << "    • Receive context from VS Code / Neovim / JetBrains via extension\n"
        << "    • Push inline diff suggestions directly into the editor buffer\n"
        << "\n"
        << "  This feature ships in a future release.\n"
        << "  Follow project updates: https://github.com/batbox/batbox\n"
        << "\n";
    return {};
}

// ---------------------------------------------------------------------------
// Registration function
// ---------------------------------------------------------------------------

void register_ide_cmd(SlashCommandRegistry& registry) {
    auto res = registry.register_command(std::make_shared<IdeCmd>());
    (void)res;
}

} // namespace batbox::commands
