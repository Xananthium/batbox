// src/commands/AdvisorCmd.cpp
//
// batbox::commands::AdvisorCmd — implements the /advisor slash command.
//
// Behaviour:
//   /advisor        — show current advisor_mode state and toggle it
//   /advisor on     — enable advisor mode
//   /advisor off    — disable advisor mode
//   /advisor status — print current state without changing it
//
// Advisor mode
// ------------
// When enabled (ctx.advisor_mode == true) the REPL main loop (CPP A.3)
// injects a one-shot coaching prompt after each assistant turn, collecting
// suggestions on improving the ongoing session (tool usage patterns, prompt
// quality, workflow efficiency).
//
// /advisor itself only flips the ctx.advisor_mode flag.  The inference engine
// integration that actually fires the advisory sub-call is wired in CPP A.3
// (REPL main loop) which checks ctx.advisor_mode at the top of each turn.
//
// This decoupled design means /advisor is fully testable today (just flip the
// flag + verify output) without needing the REPL loop implemented.
//
// No aliases.
//
// Registration entry point:
//   void register_advisor_cmd(SlashCommandRegistry&);

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/commands/CommandHelpers.hpp>
#include <batbox/repl/CommandContext.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace batbox::commands {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

/// Print the current advisor state and instructions to `out`.
void print_advisor_status(std::ostream& out, bool active) {
    out << "\n  Advisor mode: " << (active ? "ON" : "OFF") << '\n';
    out << '\n';
    if (active) {
        out << "  The built-in advisor agent is active.\n";
        out << "  After each assistant turn, batbox injects a coaching prompt\n";
        out << "  and surfaces suggestions to improve your workflow.\n";
        out << '\n';
        out << "  /advisor off  — disable advisor mode\n";
    } else {
        out << "  The built-in advisor agent is inactive.\n";
        out << "  When active, batbox will inject a coaching prompt after each\n";
        out << "  assistant turn and surface suggestions to improve your workflow.\n";
        out << '\n';
        out << "  /advisor on   — enable advisor mode\n";
    }
    out << '\n';
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// AdvisorCmd
// ---------------------------------------------------------------------------

class AdvisorCmd final : public ISlashCommand {
public:
    // ---- Identity -----------------------------------------------------------

    [[nodiscard]] std::string_view name() const noexcept override {
        return "advisor";
    }

    [[nodiscard]] std::string_view description() const noexcept override {
        return "Toggle the built-in advisor agent that suggests session improvements.";
    }

    [[nodiscard]] std::string_view usage() const noexcept override {
        return "/advisor [on|off|status]";
    }

    [[nodiscard]] std::vector<std::string> aliases() const override {
        return {};
    }

    [[nodiscard]] bool requires_args() const noexcept override { return false; }

    // ---- Execute ------------------------------------------------------------

    [[nodiscard]] batbox::Result<void> execute(
        std::string_view   args,
        CommandContext&    ctx) override;
};

// ---------------------------------------------------------------------------
// execute
// ---------------------------------------------------------------------------

batbox::Result<void> AdvisorCmd::execute(
    std::string_view args,
    CommandContext&  ctx)
{
    const std::string_view sub = trim(args);

    if (sub.empty()) {
        // No arg: toggle and report.
        ctx.advisor_mode = !ctx.advisor_mode;
        ctx.output << "  Advisor mode " << (ctx.advisor_mode ? "enabled" : "disabled") << ".\n";
        print_advisor_status(ctx.output, ctx.advisor_mode);
        return {};
    }

    if (sub == "on") {
        if (ctx.advisor_mode) {
            ctx.output << "  Advisor mode is already on.\n";
        } else {
            ctx.advisor_mode = true;
            ctx.output << "  Advisor mode enabled.\n";
        }
        print_advisor_status(ctx.output, ctx.advisor_mode);
        return {};
    }

    if (sub == "off") {
        if (!ctx.advisor_mode) {
            ctx.output << "  Advisor mode is already off.\n";
        } else {
            ctx.advisor_mode = false;
            ctx.output << "  Advisor mode disabled.\n";
        }
        print_advisor_status(ctx.output, ctx.advisor_mode);
        return {};
    }

    if (sub == "status") {
        print_advisor_status(ctx.output, ctx.advisor_mode);
        return {};
    }

    return batbox::Err(
        std::string("/advisor: unknown subcommand '") + std::string(sub) +
        "'.\nUsage: " + std::string(usage())
    );
}

// ---------------------------------------------------------------------------
// Registration function
// ---------------------------------------------------------------------------

void register_advisor_cmd(SlashCommandRegistry& registry) {
    auto res = registry.register_command(std::make_shared<AdvisorCmd>());
    (void)res;
}

} // namespace batbox::commands
