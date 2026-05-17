// src/commands/ClearCmd.cpp
//
// batbox::commands::ClearCmd — implements the /clear slash command.
//
// Calls ctx.conversation.reset_messages() to discard all conversation turns.
// The TUI ChatView renders an empty message list on the next REPL cycle.
//
// No aliases.
//
// Registration entry point:
//   void register_clear_cmd(SlashCommandRegistry&);

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/repl/CommandContext.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace batbox::commands {

// ---------------------------------------------------------------------------
// ClearCmd
// ---------------------------------------------------------------------------

class ClearCmd final : public ISlashCommand {
public:
    // ---- Identity -----------------------------------------------------------

    [[nodiscard]] std::string_view name() const noexcept override {
        return "clear";
    }

    [[nodiscard]] std::string_view description() const noexcept override {
        return "Clear the conversation history and reset the ChatView scrollback.";
    }

    [[nodiscard]] std::string_view usage() const noexcept override {
        return "/clear";
    }

    [[nodiscard]] std::vector<std::string> aliases() const override {
        return {};
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
// Calls reset_messages() on the ConversationHandle reference stored in ctx.
// The REPL render cycle (CPP A.3) redraws ChatView with an empty message
// list on the next iteration.
// ---------------------------------------------------------------------------

batbox::Result<void> ClearCmd::execute(
    std::string_view /*args*/,
    CommandContext&  ctx)
{
    ctx.conversation.reset_messages();
    ctx.output << "Conversation cleared.\n";
    return {};
}

// ---------------------------------------------------------------------------
// Registration function
// ---------------------------------------------------------------------------

void register_clear_cmd(SlashCommandRegistry& registry) {
    auto res = registry.register_command(std::make_shared<ClearCmd>());
    (void)res;
}

} // namespace batbox::commands
