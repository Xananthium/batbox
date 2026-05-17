// src/commands/StatsCmd.cpp
//
// batbox::commands::StatsCmd — implements the /stats slash command.
//
// /stats prints a lifetime + session statistics summary to ctx.output:
//
//   Turns           — ctx.conversation.get_turn_count()
//   Prompt tokens   — UsageTracker::session_total().prompt_tokens
//   Completion toks — UsageTracker::session_total().completion_tokens
//   Total tokens    — UsageTracker::session_total().total_tokens
//   Session cost    — UsageTracker::session_total().cost_usd
//   Agents spawned  — AgentSupervisor::snapshot().size()
//
// ctx.usage_tracker and ctx.agent_supervisor are nullable.  When null the
// relevant counters print "(n/a)".
//
// No aliases.
//
// Registration entry point:
//   void register_stats_cmd(SlashCommandRegistry&);

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/repl/CommandContext.hpp>
#include <batbox/inference/UsageTracker.hpp>
#include <batbox/agents/AgentSupervisor.hpp>

#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace batbox::commands {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

/// Format a labelled counter row: "  <label padded to 22>  <value>"
[[nodiscard]] std::string stats_row(std::string_view label,
                                    std::string_view value)
{
    constexpr std::size_t kLabelWidth = 22;
    std::string row;
    row += "  ";
    row += label;
    if (label.size() < kLabelWidth) {
        row += std::string(kLabelWidth - label.size(), ' ');
    } else {
        row += ' ';
    }
    row += value;
    row += '\n';
    return row;
}

/// Format a USD cost value to 4 decimal places.
[[nodiscard]] std::string format_usd(double usd) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(4) << '$' << usd;
    return ss.str();
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// StatsCmd
// ---------------------------------------------------------------------------

class StatsCmd final : public ISlashCommand {
public:
    [[nodiscard]] std::string_view name() const noexcept override {
        return "stats";
    }

    [[nodiscard]] std::string_view description() const noexcept override {
        return "Show session statistics: turns, tokens, cost, agent spawns.";
    }

    [[nodiscard]] std::string_view usage() const noexcept override {
        return "/stats";
    }

    [[nodiscard]] std::vector<std::string> aliases() const override {
        return {};
    }

    [[nodiscard]] bool requires_args() const noexcept override { return false; }

    [[nodiscard]] batbox::Result<void> execute(
        std::string_view args,
        CommandContext&  ctx) override;
};

// ---------------------------------------------------------------------------
// execute
// ---------------------------------------------------------------------------

batbox::Result<void> StatsCmd::execute(
    std::string_view /*args*/,
    CommandContext&  ctx)
{
    std::ostream& out = ctx.output;

    out << "\n  Session statistics\n";
    out << "  ──────────────────\n\n";

    // ---- Turns --------------------------------------------------------------
    {
        const std::size_t turns = ctx.conversation.get_turn_count();
        out << stats_row("Turns", std::to_string(turns));
    }

    // ---- Token counts -------------------------------------------------------
    if (ctx.usage_tracker != nullptr) {
        const batbox::inference::UsageDelta s =
            ctx.usage_tracker->session_total();

        out << stats_row("Prompt tokens",      std::to_string(s.prompt_tokens));
        out << stats_row("Completion tokens",  std::to_string(s.completion_tokens));
        out << stats_row("Total tokens",       std::to_string(s.total_tokens));
        out << stats_row("Session cost",       format_usd(s.cost_usd));
    } else {
        out << stats_row("Prompt tokens",     "(n/a)");
        out << stats_row("Completion tokens", "(n/a)");
        out << stats_row("Total tokens",      "(n/a)");
        out << stats_row("Session cost",      "(n/a)");
    }

    // ---- Agent spawns -------------------------------------------------------
    if (ctx.agent_supervisor != nullptr) {
        const auto snapshots = ctx.agent_supervisor->snapshot();
        out << stats_row("Agents spawned",
                         std::to_string(snapshots.size()));
    } else {
        out << stats_row("Agents spawned", "(n/a)");
    }

    out << '\n';
    return {};
}

// ---------------------------------------------------------------------------
// Registration function
// ---------------------------------------------------------------------------

void register_stats_cmd(SlashCommandRegistry& registry) {
    auto res = registry.register_command(std::make_shared<StatsCmd>());
    (void)res;
}

} // namespace batbox::commands
