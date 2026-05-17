// src/commands/OutputStyleCmd.cpp
//
// batbox::commands::OutputStyleCmd — implements the /output-style slash command.
//
// /output-style [markdown|plain|auto]
//
// Selects how assistant responses are rendered.  The active style is persisted
// in settings.json under the "output_style" key.  On the next inference turn
// SystemPrompt::compose() reads this value and includes a formatting-persona
// paragraph that instructs the model to respond accordingly.
//
// Style semantics
// ---------------
//   markdown (default)  — model may use rich Markdown; MarkdownRenderer applies
//                          FTXUI styling (bold, code blocks, lists, tables).
//   plain               — model is asked to respond in plain prose with no
//                          Markdown syntax; MarkdownRenderer passes text through.
//   auto                — model uses its own judgment; renderer tries Markdown
//                          first and falls back to plain if no structure detected.
//
// Without an argument a numbered list of styles is shown and the user selects
// interactively, matching the pattern established by ThemeCmd.
//
// Registration entry point:
//   void register_output_style_cmd(SlashCommandRegistry&);

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
// Style catalogue
// ---------------------------------------------------------------------------

struct StyleEntry {
    std::string_view name;
    std::string_view description;
};

static constexpr std::array<StyleEntry, 3> kOutputStyles = {{
    { "markdown", "Rich Markdown rendered with FTXUI styling (default)"     },
    { "plain",    "Plain prose — no Markdown syntax, clean paragraph text"  },
    { "auto",     "Model's own judgment; renderer adapts to detected format" },
}};

// ---------------------------------------------------------------------------
// OutputStyleCmd
// ---------------------------------------------------------------------------

class OutputStyleCmd final : public ISlashCommand {
public:
    // ---- Identity -----------------------------------------------------------

    [[nodiscard]] std::string_view name() const noexcept override {
        return "output-style";
    }

    [[nodiscard]] std::string_view description() const noexcept override {
        return "Set assistant response rendering: markdown (default), plain, or auto.";
    }

    [[nodiscard]] std::string_view usage() const noexcept override {
        return "/output-style [markdown|plain|auto]";
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
    [[nodiscard]] batbox::Result<void> apply_style(
        std::string_view  style_name,
        CommandContext&   ctx);
};

// ---------------------------------------------------------------------------
// execute
// ---------------------------------------------------------------------------

batbox::Result<void> OutputStyleCmd::execute(
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
        // Direct apply: validate style name.
        bool found = false;
        for (const auto& entry : kOutputStyles) {
            if (entry.name == trimmed) {
                found = true;
                break;
            }
        }
        if (!found) {
            return batbox::Err(
                std::string("/output-style: unknown style '") + trimmed +
                "'. Valid values: markdown, plain, auto.");
        }
        return apply_style(trimmed, ctx);
    }

    // No argument — show picker.
    ctx.output << "\n  Output styles\n";
    ctx.output << "  ─────────────\n\n";

    for (std::size_t i = 0; i < kOutputStyles.size(); ++i) {
        ctx.output << "  [" << (i + 1) << "]  "
                   << kOutputStyles[i].name
                   << "\n       " << kOutputStyles[i].description << "\n\n";
    }

    ctx.output << "  Select a style [1–" << kOutputStyles.size()
               << "] (or press Enter to cancel): ";
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

    // Accept number or name.
    bool is_numeric = !line.empty() &&
        std::all_of(line.begin(), line.end(), [](unsigned char c){ return std::isdigit(c); });

    std::string_view selected_name;

    if (is_numeric) {
        const int idx = std::stoi(line);
        if (idx < 1 || static_cast<std::size_t>(idx) > kOutputStyles.size()) {
            return batbox::Err(
                std::string("/output-style: invalid selection '") + line + "'. Choose 1–" +
                std::to_string(kOutputStyles.size()) + ".");
        }
        selected_name = kOutputStyles[static_cast<std::size_t>(idx - 1)].name;
    } else {
        bool found = false;
        for (const auto& entry : kOutputStyles) {
            if (entry.name == line) {
                selected_name = entry.name;
                found = true;
                break;
            }
        }
        if (!found) {
            return batbox::Err(
                std::string("/output-style: unknown style '") + line +
                "'. Valid values: markdown, plain, auto.");
        }
    }

    return apply_style(selected_name, ctx);
}

// ---------------------------------------------------------------------------
// apply_style
//
// Writes the chosen style name into settings.json.  SystemPrompt::compose()
// reads settings.output_style and prepends a formatting-persona paragraph that
// instructs the model to respond in the requested format.
// ---------------------------------------------------------------------------

batbox::Result<void> OutputStyleCmd::apply_style(
    std::string_view style_name,
    CommandContext&  ctx)
{
    const std::filesystem::path settings_path =
        ctx.config_dir / "settings.json";

    // Load current settings (missing file → defaults, which is fine).
    auto load_res = batbox::config::load_settings(settings_path);
    if (!load_res.has_value()) {
        return batbox::Err(
            std::string("/output-style: failed to read settings: ") + load_res.error());
    }

    batbox::config::Settings settings = std::move(load_res.value());
    settings.output_style = std::string(style_name);

    // Persist atomically.
    auto write_res = batbox::config::write_settings(settings_path, settings);
    if (!write_res.has_value()) {
        return batbox::Err(
            std::string("/output-style: failed to persist settings: ") + write_res.error());
    }

    ctx.output << "\nOutput style set to '" << style_name
               << "'. Applied to the next inference turn.\n";

    return {};
}

// ---------------------------------------------------------------------------
// Registration function
// ---------------------------------------------------------------------------

void register_output_style_cmd(SlashCommandRegistry& registry) {
    auto res = registry.register_command(std::make_shared<OutputStyleCmd>());
    (void)res;
}

} // namespace batbox::commands
