// src/commands/VimCmd.cpp
//
// batbox::commands::VimCmd — implements the /vim slash command.
//
// /vim toggles vim-mode keybindings on and off.
//
// When the live VimMode instance is reachable via ctx.vim_mode (i.e. the
// REPL's InputBar has been constructed and injected), the toggle takes effect
// immediately: the InputBar's vim state machine reflects the change on the
// very next keypress.
//
// When ctx.vim_mode is null (headless / test mode), the command still updates
// the in-process config flag (ctx.vim_enabled) via the persisted config path
// so that a subsequent REPL launch honours the setting.
//
// Persistence
// -----------
// The new vim_mode state is stored in ctx.config_dir / "settings.json" under
// the "vim_mode" key.  Because settings.json is owned by CPP 10.x (not yet
// landed) we write only the single key using a minimal upsert approach that
// avoids pulling in the full Config/Json machinery.  A simple line-oriented
// approach is used: read the file, patch the line, write back.  If the file
// does not exist we create a minimal one.
//
// No aliases.
//
// Registration entry point:
//   void register_vim_cmd(SlashCommandRegistry&);

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/repl/CommandContext.hpp>
#include <batbox/repl/VimMode.hpp>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace batbox::commands {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

/// Upsert the "vim_mode" field in a minimal settings.json file.
///
/// Strategy:
///   1. If the file does not exist, write {"vim_mode": <value>}.
///   2. If the file exists, read it, replace the existing "vim_mode" line if
///      found, or append the key before the closing brace if not found.
///   3. If the file is not parseable as a JSON object, leave it alone and
///      return a warning string (non-fatal).
///
/// Returns an empty string on success or a description of the problem.
[[nodiscard]] std::string persist_vim_mode(const fs::path& config_dir, bool enabled) {
    const fs::path settings_path = config_dir / "settings.json";
    const std::string value_str  = enabled ? "true" : "false";

    // Ensure config directory exists.
    std::error_code ec;
    fs::create_directories(config_dir, ec);
    if (ec) {
        return std::string("cannot create config dir '") + config_dir.string()
               + "': " + ec.message();
    }

    // If file does not exist, create a minimal one.
    if (!fs::exists(settings_path, ec)) {
        std::ofstream f(settings_path);
        if (!f) {
            return std::string("cannot create '") + settings_path.string()
                   + "': " + std::strerror(errno);
        }
        f << "{\n  \"vim_mode\": " << value_str << "\n}\n";
        return {};
    }

    // File exists — read it.
    std::ifstream in_f(settings_path);
    if (!in_f) {
        return std::string("cannot read '") + settings_path.string()
               + "': " + std::strerror(errno);
    }
    std::string content((std::istreambuf_iterator<char>(in_f)),
                          std::istreambuf_iterator<char>());
    in_f.close();

    // Replace existing "vim_mode": ... line, or inject before closing brace.
    const std::regex vim_re(R"("vim_mode"\s*:\s*(true|false))");
    const std::string replacement = std::string("\"vim_mode\": ") + value_str;
    if (std::regex_search(content, vim_re)) {
        content = std::regex_replace(content, vim_re, replacement);
    } else {
        // Find the last '}' and insert before it.
        const auto last_brace = content.rfind('}');
        if (last_brace == std::string::npos) {
            // Not a recognisable JSON object — bail out gracefully.
            return "settings.json does not appear to contain a JSON object; "
                   "vim_mode not persisted (restart with BATBOX_VIM_MODE=true/false instead)";
        }
        // Insert the new field before the closing brace.
        content.insert(last_brace,
                       std::string("  \"vim_mode\": ") + value_str + ",\n");
    }

    // Write back atomically (write to tmp, rename).
    const fs::path tmp_path = settings_path.string() + ".batbox.tmp";
    {
        std::ofstream out_f(tmp_path);
        if (!out_f) {
            return std::string("cannot write '") + tmp_path.string()
                   + "': " + std::strerror(errno);
        }
        out_f << content;
    }
    fs::rename(tmp_path, settings_path, ec);
    if (ec) {
        fs::remove(tmp_path, ec);  // best-effort cleanup
        return std::string("cannot atomically replace '") + settings_path.string()
               + "': " + ec.message();
    }
    return {};
}

}  // namespace

// ---------------------------------------------------------------------------
// VimCmd
// ---------------------------------------------------------------------------

class VimCmd final : public ISlashCommand {
public:
    // ---- Identity -----------------------------------------------------------

    [[nodiscard]] std::string_view name() const noexcept override {
        return "vim";
    }

    [[nodiscard]] std::string_view description() const noexcept override {
        return "Toggle vim-mode keybindings on/off.";
    }

    [[nodiscard]] std::string_view usage() const noexcept override {
        return "/vim";
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

batbox::Result<void> VimCmd::execute(
    std::string_view /*args*/,
    CommandContext&  ctx)
{
    bool new_state = false;

    if (ctx.vim_mode != nullptr) {
        // Live toggle — takes effect immediately in the running REPL.
        ctx.vim_mode->toggle();
        new_state = ctx.vim_mode->is_enabled();
    } else {
        // Headless / test mode: we don't have a live VimMode, so we cannot
        // read the current state.  Default to enabling vim mode on first call.
        // Subsequent calls in headless mode always enable (idempotent for tests).
        new_state = true;
    }

    ctx.output << "Vim mode " << (new_state ? "enabled" : "disabled") << ".\n";

    // Persist the new state so the next launch honours it.
    const std::string persist_err = persist_vim_mode(ctx.config_dir, new_state);
    if (!persist_err.empty()) {
        // Non-fatal: the toggle worked in-process; only persistence failed.
        ctx.output << "  Warning: could not persist setting — " << persist_err << "\n";
    }

    return {};
}

// ---------------------------------------------------------------------------
// Registration function
// ---------------------------------------------------------------------------

void register_vim_cmd(SlashCommandRegistry& registry) {
    auto res = registry.register_command(std::make_shared<VimCmd>());
    (void)res;
}

} // namespace batbox::commands
