// src/commands/ExitCmd.cpp
//
// batbox::commands::ExitCmd — implements the /exit slash command.
//
// Sets ctx.exit_requested = true, which the main REPL loop checks at the top
// of every iteration before dispatching the next user input.  This is a
// cooperative, clean-shutdown signal: inference drains its SSE stream, the
// session store flushes, and the TUI tears down gracefully.
//
// Aliases: /quit, /q → /exit
//
// Registration entry point:
//   void register_exit_cmd(SlashCommandRegistry&);

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/repl/CommandContext.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace batbox::commands {

// ---------------------------------------------------------------------------
// ExitCmd
// ---------------------------------------------------------------------------

class ExitCmd final : public ISlashCommand {
public:
    // ---- Identity -----------------------------------------------------------

    [[nodiscard]] std::string_view name() const noexcept override {
        return "exit";
    }

    [[nodiscard]] std::string_view description() const noexcept override {
        return "Exit batbox, flush session, and shut down cleanly.";
    }

    [[nodiscard]] std::string_view usage() const noexcept override {
        return "/exit";
    }

    [[nodiscard]] std::vector<std::string> aliases() const override {
        return { "quit", "q" };
    }

    [[nodiscard]] bool requires_args() const noexcept override { return false; }

    // ---- Execute ------------------------------------------------------------

    [[nodiscard]] batbox::Result<void> execute(
        std::string_view   /*args*/,
        CommandContext&    ctx) override;
};

// ---------------------------------------------------------------------------
// execute
//
// Sets the cooperative shutdown flag.  The REPL main loop (CPP A.3) checks
// ctx.exit_requested after every command dispatch and exits its event loop
// when true.  App::shutdown() handles downstream cleanup (session flush,
// sidecar termination, TUI teardown).
// ---------------------------------------------------------------------------

batbox::Result<void> ExitCmd::execute(
    std::string_view /*args*/,
    CommandContext&  ctx)
{
    ctx.output << "Goodbye.\n";
    ctx.exit_requested = true;
    return {};
}

// ---------------------------------------------------------------------------
// Registration function
// ---------------------------------------------------------------------------

void register_exit_cmd(SlashCommandRegistry& registry) {
    auto res = registry.register_command(std::make_shared<ExitCmd>());
    (void)res;
}

} // namespace batbox::commands
