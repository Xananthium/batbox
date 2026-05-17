// src/commands/ThemeCmd.cpp
//
// batbox::commands::ThemeCmd — implements the /theme slash command.
//
// Without an argument, opens a numbered list of the 5 available themes and
// prompts the user to pick one by number.  With a theme name as an argument,
// applies it directly.
//
// The selected theme is:
//   1. Written into ctx.settings.theme.
//   2. Persisted atomically to ~/.batbox/settings.json via write_settings().
//
// Because the command runs in the CLI/REPL context (no live TUI screen), the
// ModalPicker TUI component is not available here — instead a simple numbered
// list is presented on ctx.output and the user selects by number via ctx.input.
// (ModalPicker is a TUI component that requires a running FTXUI screen; the
// REPL command layer uses std::ostream / std::istream.)
//
// Registration entry point:
//   void register_theme_cmd(SlashCommandRegistry&);

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/config/SettingsLoader.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/repl/CommandContext.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace batbox::commands {

// ---------------------------------------------------------------------------
// Theme catalogue
// ---------------------------------------------------------------------------

struct ThemeEntry {
    std::string_view name;
    std::string_view description;
};

static constexpr std::array<ThemeEntry, 5> kThemes = {{
    { "miss-kittin",    "Electroclash default — hot magenta + cyan on near-black" },
    { "stock-exchange", "Finance terminal  — ice cyan + silver on cold blue"      },
    { "frank-sinatra",  "Smoky 50s         — gold + cream on warm sepia"          },
    { "monochrome",     "High contrast     — strict white on black, no accents"   },
    { "classic",        "Original claude-code colours — amber on soft cream"      },
}};

// ---------------------------------------------------------------------------
// ThemeCmd
// ---------------------------------------------------------------------------

class ThemeCmd final : public ISlashCommand {
public:
    // ---- Identity -----------------------------------------------------------

    [[nodiscard]] std::string_view name() const noexcept override {
        return "theme";
    }

    [[nodiscard]] std::string_view description() const noexcept override {
        return "Pick or apply a UI colour theme (5 built-in themes).";
    }

    [[nodiscard]] std::string_view usage() const noexcept override {
        return "/theme [theme-name]";
    }

    [[nodiscard]] std::vector<std::string> aliases() const override {
        return {};
    }

    [[nodiscard]] bool requires_args() const noexcept override { return false; }

    // ---- Execute ------------------------------------------------------------

    [[nodiscard]] batbox::Result<void> execute(
        std::string_view  args,
        CommandContext&   ctx) override;

private:
    // Apply a validated theme name: persist to settings.json and confirm.
    [[nodiscard]] batbox::Result<void> apply_theme(
        std::string_view  theme_name,
        CommandContext&   ctx);
};

// ---------------------------------------------------------------------------
// execute
// ---------------------------------------------------------------------------

batbox::Result<void> ThemeCmd::execute(
    std::string_view args,
    CommandContext&  ctx)
{
    // Trim leading/trailing whitespace from the argument.
    std::string trimmed(args);
    const auto first = trimmed.find_first_not_of(" \t");
    if (first == std::string::npos) {
        trimmed.clear();
    } else {
        const auto last = trimmed.find_last_not_of(" \t");
        trimmed = trimmed.substr(first, last - first + 1);
    }

    if (!trimmed.empty()) {
        // Direct apply: validate theme name.
        bool found = false;
        for (const auto& entry : kThemes) {
            if (entry.name == trimmed) {
                found = true;
                break;
            }
        }
        if (!found) {
            return batbox::Err(
                std::string("/theme: unknown theme '") + trimmed +
                "'. Run /theme without arguments to see available themes.");
        }
        return apply_theme(trimmed, ctx);
    }

    // No argument — show picker.
    ctx.output << "\n  Available themes\n";
    ctx.output << "  ─────────────────\n\n";

    // Print numbered list.
    for (std::size_t i = 0; i < kThemes.size(); ++i) {
        ctx.output << "  [" << (i + 1) << "]  "
                   << kThemes[i].name
                   << "\n       " << kThemes[i].description << "\n\n";
    }

    ctx.output << "  Select a theme [1–" << kThemes.size() << "] (or press Enter to cancel): ";
    ctx.output.flush();

    std::string line;
    if (!std::getline(ctx.input, line)) {
        ctx.output << "\nCancelled.\n";
        return {};
    }

    // Trim input.
    const auto pos = line.find_first_not_of(" \t\r\n");
    if (pos == std::string::npos) {
        ctx.output << "Cancelled.\n";
        return {};
    }
    line = line.substr(pos, line.find_last_not_of(" \t\r\n") - pos + 1);

    // Parse selection: accept a number OR a name.
    // Try numeric first.
    bool is_numeric = !line.empty() &&
        std::all_of(line.begin(), line.end(), [](unsigned char c){ return std::isdigit(c); });

    std::string_view selected_name;

    if (is_numeric) {
        const int idx = std::stoi(line);
        if (idx < 1 || static_cast<std::size_t>(idx) > kThemes.size()) {
            return batbox::Err(
                std::string("/theme: invalid selection '") + line + "'. Choose 1–" +
                std::to_string(kThemes.size()) + ".");
        }
        selected_name = kThemes[static_cast<std::size_t>(idx - 1)].name;
    } else {
        // Accept a name typed directly.
        bool found = false;
        for (const auto& entry : kThemes) {
            if (entry.name == line) {
                selected_name = entry.name;
                found = true;
                break;
            }
        }
        if (!found) {
            return batbox::Err(
                std::string("/theme: unknown theme '") + line +
                "'. Run /theme without arguments to see available themes.");
        }
    }

    return apply_theme(selected_name, ctx);
}

// ---------------------------------------------------------------------------
// apply_theme
//
// Writes the chosen theme name into settings.json and prints a confirmation.
// ---------------------------------------------------------------------------

batbox::Result<void> ThemeCmd::apply_theme(
    std::string_view theme_name,
    CommandContext&  ctx)
{
    const std::filesystem::path settings_path =
        ctx.config_dir / "settings.json";

    // Load current settings (missing file → defaults, which is fine).
    auto load_res = batbox::config::load_settings(settings_path);
    if (!load_res.has_value()) {
        return batbox::Err(
            std::string("/theme: failed to read settings: ") + load_res.error());
    }

    batbox::config::Settings settings = std::move(load_res.value());
    settings.theme = std::string(theme_name);

    // Persist atomically.
    auto write_res = batbox::config::write_settings(settings_path, settings);
    if (!write_res.has_value()) {
        return batbox::Err(
            std::string("/theme: failed to persist settings: ") + write_res.error());
    }

    ctx.output << "\nTheme set to '" << theme_name
               << "'. Restart batbox (or trigger a hot-reload) to apply.\n";

    return {};
}

// ---------------------------------------------------------------------------
// Registration function
// ---------------------------------------------------------------------------

void register_theme_cmd(SlashCommandRegistry& registry) {
    auto res = registry.register_command(std::make_shared<ThemeCmd>());
    (void)res;
}

} // namespace batbox::commands
