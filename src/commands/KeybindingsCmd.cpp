// src/commands/KeybindingsCmd.cpp
//
// batbox::commands::KeybindingsCmd — implements the /keybindings slash command.
//
// Usage:
//   /keybindings         — print the current action → key table
//   /keybindings edit    — open keybindings.json in $EDITOR (or $VISUAL)
//   /keybindings reload  — re-read keybindings.json and call apply_override()
//
// The command reads the live Keybindings instance via ctx.keybindings when
// available.  When ctx.keybindings is null (headless / test mode) it prints
// the built-in defaults instead.
//
// The keybindings.json file is located at ctx.config_dir / "keybindings.json".
//
// No aliases.
//
// Registration entry point:
//   void register_keybindings_cmd(SlashCommandRegistry&);

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/config/KeybindingsConfig.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/repl/CommandContext.hpp>
#include <batbox/repl/Keybindings.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace batbox::commands {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

/// Convert a ReplAction enum value to its canonical action-name string.
/// Used for the display table.
[[nodiscard]] std::string_view action_name(batbox::repl::ReplAction action) {
    using batbox::repl::ReplAction;
    switch (action) {
        case ReplAction::None:          return "none";
        case ReplAction::Send:          return "send";
        case ReplAction::Cancel:        return "cancel";
        case ReplAction::Newline:       return "newline";
        case ReplAction::HistoryUp:     return "history_up";
        case ReplAction::HistoryDown:   return "history_down";
        case ReplAction::Clear:         return "clear";
        case ReplAction::CycleMode:     return "cycle_mode";
        case ReplAction::VimToggle:     return "vim_toggle";
        case ReplAction::HistorySearch: return "history_search";
    }
    return "unknown";
}

/// Print the action→key table.
void print_keybindings(
    std::ostream& out,
    const std::unordered_map<batbox::repl::ReplAction, std::string>& desc_map)
{
    // Collect and sort by action name for stable output.
    using batbox::repl::ReplAction;
    std::vector<std::pair<std::string, std::string>> rows;
    rows.reserve(desc_map.size());
    for (const auto& [action, desc] : desc_map) {
        if (action == ReplAction::None) continue;
        rows.emplace_back(std::string(action_name(action)), desc);
    }
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    // Find column widths.
    std::size_t action_col = 7;  // "action"
    std::size_t key_col    = 3;  // "key"
    for (const auto& [a, k] : rows) {
        if (a.size() > action_col) action_col = a.size();
        if (k.size() > key_col)    key_col    = k.size();
    }

    const std::string sep(action_col + key_col + 7, '-');

    out << "\n  Current keybindings\n";
    out << "  " << sep << "\n";
    out << "  " << std::left << std::setw(static_cast<int>(action_col)) << "Action"
        << "  "
        << std::left << std::setw(static_cast<int>(key_col)) << "Key" << "\n";
    out << "  " << sep << "\n";
    for (const auto& [a, k] : rows) {
        out << "  " << std::left << std::setw(static_cast<int>(action_col)) << a
            << "  " << k << "\n";
    }
    out << "  " << sep << "\n\n";
}

/// Return the path to the user's keybindings.json file.
[[nodiscard]] fs::path keybindings_json_path(const fs::path& config_dir) {
    return config_dir / "keybindings.json";
}

/// Open `path` in $EDITOR (or $VISUAL).
/// Returns an empty string on success, or a description of the error.
[[nodiscard]] std::string open_in_editor(const fs::path& path) {
    const char* editor = std::getenv("VISUAL");
    if (!editor || editor[0] == '\0') {
        editor = std::getenv("EDITOR");
    }
    if (!editor || editor[0] == '\0') {
        return "no editor found — set $EDITOR or $VISUAL to your preferred editor";
    }
    const std::string cmd = std::string(editor) + " \"" + path.string() + "\"";
    const int rc = std::system(cmd.c_str());  // NOLINT(concurrency-mt-unsafe)
    if (rc != 0) {
        return std::string(editor) + " exited with status " + std::to_string(rc);
    }
    return {};
}

/// Create a default keybindings.json skeleton if it does not exist.
/// Returns an empty string on success, or a description of the error.
[[nodiscard]] std::string ensure_keybindings_file(const fs::path& config_dir) {
    std::error_code ec;
    fs::create_directories(config_dir, ec);
    if (ec) {
        return std::string("cannot create config dir: ") + ec.message();
    }

    const fs::path kbp = keybindings_json_path(config_dir);
    if (fs::exists(kbp, ec)) return {};  // already present

    // Write default skeleton.
    std::ofstream f(kbp);
    if (!f) {
        return std::string("cannot create '") + kbp.string()
               + "': " + std::strerror(errno);
    }
    f << "{\n"
      << "  \"send\":         \"Ctrl+Enter\",\n"
      << "  \"cancel\":       \"Escape\",\n"
      << "  \"cycle_mode\":   \"Shift+Tab\",\n"
      << "  \"newline\":      \"Shift+Enter\",\n"
      << "  \"history_up\":   \"Up\",\n"
      << "  \"history_down\": \"Down\",\n"
      << "  \"clear\":        \"Ctrl+L\",\n"
      << "  \"vim_toggle\":   \"Escape\",\n"
      << "  \"history_search\": \"Ctrl+R\"\n"
      << "}\n";
    return {};
}

}  // namespace

// ---------------------------------------------------------------------------
// KeybindingsCmd
// ---------------------------------------------------------------------------

class KeybindingsCmd final : public ISlashCommand {
public:
    // ---- Identity -----------------------------------------------------------

    [[nodiscard]] std::string_view name() const noexcept override {
        return "keybindings";
    }

    [[nodiscard]] std::string_view description() const noexcept override {
        return "Show current keybindings; /keybindings edit to open in $EDITOR.";
    }

    [[nodiscard]] std::string_view usage() const noexcept override {
        return "/keybindings [edit|reload]";
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

batbox::Result<void> KeybindingsCmd::execute(
    std::string_view args,
    CommandContext&  ctx)
{
    // Strip leading/trailing whitespace from args.
    const auto start = args.find_first_not_of(" \t");
    const auto end   = args.find_last_not_of(" \t");
    const std::string_view sub = (start == std::string_view::npos)
        ? std::string_view{}
        : args.substr(start, end - start + 1);

    // --- /keybindings edit --------------------------------------------------
    if (sub == "edit") {
        const fs::path kbp = keybindings_json_path(ctx.config_dir);

        // Create skeleton file if it does not exist.
        const std::string create_err = ensure_keybindings_file(ctx.config_dir);
        if (!create_err.empty()) {
            return batbox::Err("/keybindings edit: " + create_err);
        }

        ctx.output << "Opening " << kbp.string() << " in $EDITOR ...\n";
        const std::string edit_err = open_in_editor(kbp);
        if (!edit_err.empty()) {
            return batbox::Err("/keybindings edit: " + edit_err);
        }

        // After editing, reload if we have a live Keybindings instance.
        if (ctx.keybindings != nullptr) {
            const fs::path cfg_path = keybindings_json_path(ctx.config_dir);
            auto load_result = batbox::config::load_keybindings(cfg_path);
            if (load_result.has_value()) {
                ctx.keybindings->apply_override(load_result.value());
                ctx.output << "Keybindings reloaded.\n";
            } else {
                ctx.output << "  Warning: could not parse keybindings.json — "
                           << load_result.error() << "\n"
                           << "  Previous bindings remain active.\n";
            }
        }
        return {};
    }

    // --- /keybindings reload ------------------------------------------------
    if (sub == "reload") {
        if (ctx.keybindings == nullptr) {
            ctx.output << "No live keybindings instance (headless mode); "
                       << "reload will take effect on next launch.\n";
            return {};
        }
        const fs::path cfg_path = keybindings_json_path(ctx.config_dir);
        auto load_result = batbox::config::load_keybindings(cfg_path);
        if (!load_result.has_value()) {
            return batbox::Err("/keybindings reload: " + load_result.error());
        }
        ctx.keybindings->apply_override(load_result.value());
        ctx.output << "Keybindings reloaded from " << cfg_path.string() << ".\n";
        return {};
    }

    // --- /keybindings (no arg) — display table ------------------------------
    if (!sub.empty()) {
        return batbox::Err(
            std::string("/keybindings: unknown subcommand '") + std::string(sub)
            + "'. " + std::string(usage()));
    }

    // Get the current descriptor map.
    std::unordered_map<batbox::repl::ReplAction, std::string> desc_map;
    if (ctx.keybindings != nullptr) {
        desc_map = ctx.keybindings->descriptor_map();
    } else {
        // Headless mode: use built-in defaults.
        desc_map = batbox::repl::Keybindings::default_keybindings();
    }

    print_keybindings(ctx.output, desc_map);

    const fs::path kbp = keybindings_json_path(ctx.config_dir);
    ctx.output << "  File: " << kbp.string() << "\n";
    ctx.output << "  Run '/keybindings edit' to open in $EDITOR, "
               << "'/keybindings reload' to reload from file.\n\n";

    return {};
}

// ---------------------------------------------------------------------------
// Registration function
// ---------------------------------------------------------------------------

void register_keybindings_cmd(SlashCommandRegistry& registry) {
    auto res = registry.register_command(std::make_shared<KeybindingsCmd>());
    (void)res;
}

} // namespace batbox::commands
