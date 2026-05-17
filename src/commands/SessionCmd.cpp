// src/commands/SessionCmd.cpp
//
// batbox::commands::SessionCmd — implements the /session slash command.
//
// /session (no args) shows metadata for the currently active session:
//   Session:   <uuid>
//   File:      /path/to/session.json
//   Turns:     42
//   Model:     gpt-4o
//   Directory: /home/user/project
//
// These fields are retrieved from ctx.conversation via the CPP S.5 virtual
// accessors added to ConversationHandle (get_session_id, get_session_file_path,
// get_turn_count, get_model_name).  When no session is active (session id is
// empty) a friendly "No active session" message is printed instead.
//
// No aliases.
//
// Registration entry point:
//   void register_session_cmd(SlashCommandRegistry&);

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/repl/CommandContext.hpp>

#include <iomanip>
#include <string>
#include <string_view>
#include <vector>

namespace batbox::commands {

// ---------------------------------------------------------------------------
// SessionCmd
// ---------------------------------------------------------------------------

class SessionCmd final : public ISlashCommand {
public:
    // ---- Identity -----------------------------------------------------------

    [[nodiscard]] std::string_view name() const noexcept override {
        return "session";
    }

    [[nodiscard]] std::string_view description() const noexcept override {
        return "Show metadata for the current session (id, file, turns, model).";
    }

    [[nodiscard]] std::string_view usage() const noexcept override {
        return "/session";
    }

    [[nodiscard]] std::vector<std::string> aliases() const override {
        return {};
    }

    [[nodiscard]] bool requires_args() const noexcept override { return false; }

    // ---- Execute ------------------------------------------------------------

    [[nodiscard]] batbox::Result<void> execute(
        std::string_view args,
        CommandContext&  ctx) override;
};

// ---------------------------------------------------------------------------
// execute
//
// Reads the current session metadata from ctx.conversation and writes a
// formatted summary to ctx.output.
// ---------------------------------------------------------------------------

batbox::Result<void> SessionCmd::execute(
    std::string_view /*args*/,
    CommandContext&  ctx)
{
    const std::string session_id    = ctx.conversation.get_session_id();
    const std::filesystem::path fp  = ctx.conversation.get_session_file_path();
    const std::size_t turn_count    = ctx.conversation.get_turn_count();
    const std::string model_name    = ctx.conversation.get_model_name();

    if (session_id.empty()) {
        ctx.output << "No active session.\n"
                   << "Start a new conversation to create one, "
                   << "or use /resume to load a previous session.\n";
        return {};
    }

    // Column width for the label column.
    constexpr int kLabel = 12;

    ctx.output << "\n"
               << std::left << std::setw(kLabel) << "Session:"   << session_id << "\n"
               << std::setw(kLabel) << "File:"
                   << (fp.empty() ? "(none)" : fp.string()) << "\n"
               << std::setw(kLabel) << "Turns:"     << turn_count << "\n"
               << std::setw(kLabel) << "Model:"
                   << (model_name.empty() ? "(unknown)" : model_name) << "\n"
               << std::setw(kLabel) << "Directory:" << ctx.cwd.string() << "\n"
               << "\n";

    return {};
}

// ---------------------------------------------------------------------------
// Registration function
// ---------------------------------------------------------------------------

void register_session_cmd(SlashCommandRegistry& registry) {
    auto res = registry.register_command(std::make_shared<SessionCmd>());
    (void)res;
}

} // namespace batbox::commands
