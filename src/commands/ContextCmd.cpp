// src/commands/ContextCmd.cpp
//
// batbox::commands::ContextCmd — implements the /context slash command.
//
// /context shows the current context-window utilisation for the active
// conversation:
//
//   Model:          gpt-4o
//   Turns:          12
//   Tokens:         4 312 / 128 000  (3.4%)
//   Compact at:     80%  (102 400 tokens)
//   Status:         OK
//
//   [####.......................................]  3.4%
//
// Token estimation:
//   Uses ContextWindow::estimate_tokens() with messages deserialised from
//   ctx.conversation.get_messages_json().  When the message list is empty
//   the token count reports 0.
//
// Model info:
//   Derives context limit via ContextWindow::context_limit_for_model() on the
//   model name from ctx.conversation.get_model_name().
//   Falls back to 128 000 for unknown models.
//
// No aliases.
//
// Registration entry point:
//   void register_context_cmd(SlashCommandRegistry&);

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/config/Config.hpp>
#include <batbox/conversation/ContextWindow.hpp>
#include <batbox/conversation/Message.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/repl/CommandContext.hpp>

#include <algorithm>
#include <cstddef>
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

/// Format a token count with space-separated thousands groups.
/// Example: 128000 -> "128 000"
static std::string format_tokens(std::size_t n) {
    const std::string raw = std::to_string(n);
    std::string result;
    result.reserve(raw.size() + raw.size() / 3);

    const std::size_t len = raw.size();
    for (std::size_t i = 0; i < len; ++i) {
        if (i > 0 && (len - i) % 3 == 0) {
            result += ' ';
        }
        result += raw[i];
    }
    return result;
}

/// Render an ASCII progress bar of `width` fill characters.
/// `pct` is clamped to [0, 100].
/// Example (pct=25, width=40): "[##########..............................]"
static std::string render_bar(double pct, int width) {
    if (width <= 0) return {};

    if (pct < 0.0)   pct = 0.0;
    if (pct > 100.0) pct = 100.0;

    const int filled = static_cast<int>(pct / 100.0 * static_cast<double>(width) + 0.5);
    const int empty  = width - filled;

    std::string bar;
    bar.reserve(2 + static_cast<std::size_t>(width));
    bar += '[';
    for (int i = 0; i < filled; ++i) bar += '#';
    for (int i = 0; i < empty;  ++i) bar += '.';
    bar += ']';
    return bar;
}

/// Return a status label for the current fill level.
static std::string_view fill_status(double pct, int compact_at_pct) {
    if (pct >= 100.0)                                              return "FULL";
    if (pct >= static_cast<double>(compact_at_pct))                return "COMPACT";
    if (pct >= static_cast<double>(compact_at_pct) * 0.85)         return "WARN";
    return "OK";
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// ContextCmd
// ---------------------------------------------------------------------------

class ContextCmd final : public ISlashCommand {
public:
    // ---- Identity -----------------------------------------------------------

    [[nodiscard]] std::string_view name() const noexcept override {
        return "context";
    }

    [[nodiscard]] std::string_view description() const noexcept override {
        return "Show current context-window usage (tokens, turns, % of limit).";
    }

    [[nodiscard]] std::string_view usage() const noexcept override {
        return "/context";
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
// ---------------------------------------------------------------------------

batbox::Result<void> ContextCmd::execute(
    std::string_view /*args*/,
    CommandContext&  ctx)
{
    // --- 1. Load config to get compact threshold. ---------------------------

    batbox::config::Config cfg = batbox::config::Config::load_default();
    const int compact_at_pct = cfg.compact.auto_compact_at_pct;

    // --- 2. Get model name and derive context limit. -------------------------

    const std::string model_name    = ctx.conversation.get_model_name();
    const std::string display_model = model_name.empty() ? "(unknown)" : model_name;

    const std::size_t context_limit =
        model_name.empty()
            ? std::size_t{128'000}
            : batbox::conversation::ContextWindow::context_limit_for_model(model_name);

    // --- 3. Get turn count. --------------------------------------------------

    const std::size_t turn_count = ctx.conversation.get_turn_count();

    // --- 4. Estimate token usage. -------------------------------------------
    //
    // Deserialise the JSON message list into vector<Message> and pass to
    // ContextWindow::estimate_tokens().  On any deserialisation error we fall
    // back to 0 tokens (graceful degradation: the user still sees the limit
    // and bar, just with 0 used tokens).

    const batbox::Json messages_json = ctx.conversation.get_messages_json();

    std::size_t estimated_tokens = 0;

    if (messages_json.is_array() && !messages_json.empty()) {
        std::vector<batbox::conversation::Message> messages;
        messages.reserve(messages_json.size());

        bool ok = true;
        try {
            for (const auto& item : messages_json) {
                messages.push_back(batbox::conversation::from_json(item));
            }
        } catch (const std::exception&) {
            ok = false;
        }

        if (ok && !messages.empty()) {
            batbox::conversation::ContextWindow cw{cfg};
            if (!model_name.empty()) {
                cw.set_model(model_name);
            }
            estimated_tokens = cw.estimate_tokens(messages);
        }
    }

    // --- 5. Compute percentage and threshold. --------------------------------

    const double pct = (context_limit > 0)
        ? (static_cast<double>(estimated_tokens) /
           static_cast<double>(context_limit) * 100.0)
        : 0.0;

    const double pct_clamped = std::min(pct, 100.0);

    const std::size_t compact_threshold_tokens =
        static_cast<std::size_t>(
            static_cast<double>(context_limit) *
            static_cast<double>(compact_at_pct) / 100.0);

    // --- 6. Render output. --------------------------------------------------

    constexpr int kLabel = 16;  // label column width (including leading spaces)
    constexpr int kBarW  = 40;  // visual bar fill width

    std::ostream& out = ctx.output;

    std::ostringstream pct_ss;
    pct_ss << std::fixed << std::setprecision(1) << pct_clamped;
    const std::string pct_str = pct_ss.str();

    const std::string      bar    = render_bar(pct_clamped, kBarW);
    const std::string_view status = fill_status(pct_clamped, compact_at_pct);

    out << "\n";
    out << std::left << std::setw(kLabel) << "  Model:"
        << display_model << "\n";
    out << std::setw(kLabel) << "  Turns:"
        << turn_count << "\n";
    out << std::setw(kLabel) << "  Tokens:"
        << format_tokens(estimated_tokens)
        << " / " << format_tokens(context_limit)
        << "  (" << pct_str << "%)\n";
    out << std::setw(kLabel) << "  Compact at:"
        << compact_at_pct << "%"
        << "  (" << format_tokens(compact_threshold_tokens) << " tokens)\n";
    out << std::setw(kLabel) << "  Status:"
        << status << "\n";
    out << "\n";
    out << "  " << bar << "  " << pct_str << "%\n";
    out << "\n";

    return {};
}

// ---------------------------------------------------------------------------
// Registration function
// ---------------------------------------------------------------------------

void register_context_cmd(SlashCommandRegistry& registry) {
    auto res = registry.register_command(std::make_shared<ContextCmd>());
    (void)res;
}

} // namespace batbox::commands
