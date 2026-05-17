// src/commands/CostCmd.cpp
//
// batbox::commands::CostCmd — implements the /cost slash command.
//
// /cost shows the estimated USD cost for the current session, computed by
// reading accumulated token counts from the nullable ctx.usage_tracker and
// reporting the pre-computed cost_usd field (which UsageTracker::add()
// accumulates via ModelPricing at inference time).
//
// Output format:
//
//   Session cost
//   ────────────
//
//   Prompt tokens       1 234
//   Completion tokens     456
//   Estimated cost     $0.0123
//
// If the session cost is zero (either no usage or a zero-priced model) the
// note "($0.0000 — model may be locally hosted or zero-cost)" is appended.
//
// ctx.usage_tracker is nullable — prints a friendly "(no data)" when null.
//
// No aliases.
//
// Registration entry point:
//   void register_cost_cmd(SlashCommandRegistry&);

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/repl/CommandContext.hpp>
#include <batbox/inference/UsageTracker.hpp>
#include <batbox/inference/ModelPricing.hpp>

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

/// Format integer token count with thousands-separator spaces.
[[nodiscard]] std::string fmt_tokens(int value) {
    if (value == 0) return "0";

    const bool negative = value < 0;
    unsigned long long abs_val = negative
        ? static_cast<unsigned long long>(-(static_cast<long long>(value)))
        : static_cast<unsigned long long>(value);

    std::string reversed;
    reversed.reserve(16);
    int digit_count = 0;
    do {
        if (digit_count > 0 && digit_count % 3 == 0) {
            reversed += ' ';
        }
        reversed += static_cast<char>('0' + (abs_val % 10));
        abs_val /= 10;
        ++digit_count;
    } while (abs_val > 0);

    if (negative) reversed += '-';
    return std::string(reversed.rbegin(), reversed.rend());
}

/// Format a USD value to exactly 4 decimal places.
[[nodiscard]] std::string fmt_usd(double usd) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(4) << '$' << usd;
    return ss.str();
}

/// Format a labelled cost row: "  <label padded to 22>  <value>"
[[nodiscard]] std::string cost_row(std::string_view label,
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

} // anonymous namespace

// ---------------------------------------------------------------------------
// CostCmd
// ---------------------------------------------------------------------------

class CostCmd final : public ISlashCommand {
public:
    [[nodiscard]] std::string_view name() const noexcept override {
        return "cost";
    }

    [[nodiscard]] std::string_view description() const noexcept override {
        return "Show estimated USD cost for the current session.";
    }

    [[nodiscard]] std::string_view usage() const noexcept override {
        return "/cost";
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

batbox::Result<void> CostCmd::execute(
    std::string_view /*args*/,
    CommandContext&  ctx)
{
    std::ostream& out = ctx.output;

    out << "\n  Session cost\n";
    out << "  ────────────\n\n";

    if (ctx.usage_tracker == nullptr) {
        out << "  No cost data available.\n"
            << "  (UsageTracker not connected — start a conversation first.)\n";
        out << '\n';
        return {};
    }

    const batbox::inference::UsageDelta s = ctx.usage_tracker->session_total();

    out << cost_row("Prompt tokens",     fmt_tokens(s.prompt_tokens));
    out << cost_row("Completion tokens", fmt_tokens(s.completion_tokens));
    out << cost_row("Total tokens",      fmt_tokens(s.total_tokens));
    out << '\n';
    out << cost_row("Estimated cost",    fmt_usd(s.cost_usd));

    // If cost is exactly zero, add an explanatory note.
    if (s.cost_usd == 0.0) {
        out << '\n';
        out << "  Note: $0.0000 — the model may be locally hosted or zero-cost.\n"
            << "        Cost is computed via ModelPricing; unknown models return 0.\n";
    }

    out << '\n';
    return {};
}

// ---------------------------------------------------------------------------
// Registration function
// ---------------------------------------------------------------------------

void register_cost_cmd(SlashCommandRegistry& registry) {
    auto res = registry.register_command(std::make_shared<CostCmd>());
    (void)res;
}

} // namespace batbox::commands
