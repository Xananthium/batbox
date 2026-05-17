// src/commands/UsageCmd.cpp
//
// batbox::commands::UsageCmd — implements the /usage slash command.
//
// /usage shows the cumulative token usage for the current session broken down
// by category (prompt, completion, total).  It reads from the nullable
// ctx.usage_tracker pointer; when null it prints a friendly "(no data)" note.
//
// Output format (tabular, aligned):
//
//   Token usage (session)
//   ─────────────────────
//
//   Prompt tokens      1 234
//   Completion tokens    456
//   Total tokens       1 690
//
// No aliases.
//
// Registration entry point:
//   void register_usage_cmd(SlashCommandRegistry&);

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/repl/CommandContext.hpp>
#include <batbox/inference/UsageTracker.hpp>

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

/// Format an integer with thousands-separator spaces (e.g. 1 234 567).
[[nodiscard]] std::string format_tokens(int value) {
    // Build reversed digits, insert space every 3.
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

    std::string result(reversed.rbegin(), reversed.rend());
    return result;
}

/// Format a labelled usage row: "  <label padded to 22>  <value right-aligned>"
[[nodiscard]] std::string usage_row(std::string_view label,
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
// UsageCmd
// ---------------------------------------------------------------------------

class UsageCmd final : public ISlashCommand {
public:
    [[nodiscard]] std::string_view name() const noexcept override {
        return "usage";
    }

    [[nodiscard]] std::string_view description() const noexcept override {
        return "Show cumulative token usage for the current session.";
    }

    [[nodiscard]] std::string_view usage() const noexcept override {
        return "/usage";
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

batbox::Result<void> UsageCmd::execute(
    std::string_view /*args*/,
    CommandContext&  ctx)
{
    std::ostream& out = ctx.output;

    out << "\n  Token usage (session)\n";
    out << "  ─────────────────────\n\n";

    if (ctx.usage_tracker == nullptr) {
        out << "  No usage data available.\n"
            << "  (UsageTracker not connected — start a conversation first.)\n";
        out << '\n';
        return {};
    }

    const batbox::inference::UsageDelta s = ctx.usage_tracker->session_total();

    out << usage_row("Prompt tokens",     format_tokens(s.prompt_tokens));
    out << usage_row("Completion tokens", format_tokens(s.completion_tokens));
    out << usage_row("Total tokens",      format_tokens(s.total_tokens));

    out << '\n';
    return {};
}

// ---------------------------------------------------------------------------
// Registration function
// ---------------------------------------------------------------------------

void register_usage_cmd(SlashCommandRegistry& registry) {
    auto res = registry.register_command(std::make_shared<UsageCmd>());
    (void)res;
}

} // namespace batbox::commands
