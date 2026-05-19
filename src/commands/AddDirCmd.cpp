// src/commands/AddDirCmd.cpp
//
// batbox::commands::AddDirCmd — implements the /add-dir slash command.
//
// /add-dir <path>
//   Validates that <path> exists and is a readable directory, then appends it
//   to an in-process list of extra search roots exposed via CommandContext.
//   Persists the list to ctx.config_dir / "settings.json" under the
//   "extra_dirs" key using the same minimal-upsert strategy as VimCmd.
//
// Path traversal safety:
//   - Path is lexically normalised (canonical if exists).
//   - Symlinks are allowed as long as the resolved target is readable.
//   - Duplicates are silently skipped (idempotent).
//
// No aliases.
//
// Registration entry point:
//   void register_add_dir_cmd(SlashCommandRegistry&);

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/commands/CommandHelpers.hpp>
#include <batbox/repl/CommandContext.hpp>

#include <algorithm>
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
// Extra dirs in-process registry
// ---------------------------------------------------------------------------
// Global list of extra directories added via /add-dir.
// The REPL main loop (CPP A.3) will expose this through the tool permission
// layer so the Read tool can access files under these roots.

static std::vector<fs::path>& extra_dirs_registry() {
    static std::vector<fs::path> g_extra_dirs;
    return g_extra_dirs;
}

// Public accessor used by the Read tool integration (CPP 5.x).
std::vector<std::filesystem::path> get_extra_dirs() {
    return extra_dirs_registry();
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

/// Expand a leading ~/ to the user's home directory.
[[nodiscard]] std::string expand_tilde(std::string_view path) {
    if (path.size() >= 1 && path[0] == '~') {
        const char* home = std::getenv("HOME");
        if (home && path.size() == 1) {
            return std::string(home);
        }
        if (home && path.size() >= 2 && path[1] == '/') {
            return std::string(home) + std::string(path.substr(1));
        }
    }
    return std::string(path);
}

/// Persist extra_dirs list to settings.json.
/// Uses a minimal JSON-upsert: replaces existing "extra_dirs" array or
/// injects it before the closing brace.  Non-fatal on errors.
[[nodiscard]] std::string persist_extra_dirs(
        const fs::path& config_dir,
        const std::vector<fs::path>& dirs)
{
    const fs::path settings_path = config_dir / "settings.json";

    // Build the JSON array string.
    std::string arr = "[";
    for (std::size_t i = 0; i < dirs.size(); ++i) {
        if (i > 0) arr += ", ";
        arr += '"';
        // Escape backslashes and quotes in the path string.
        const std::string p = dirs[i].string();
        for (char c : p) {
            if (c == '"') arr += "\\\"";
            else if (c == '\\') arr += "\\\\";
            else arr += c;
        }
        arr += '"';
    }
    arr += ']';

    const std::string replacement_value = "\"extra_dirs\": " + arr;

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
        f << "{\n  " << replacement_value << "\n}\n";
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

    // Replace existing "extra_dirs": [...] or inject before closing brace.
    const std::regex arr_re(R"("extra_dirs"\s*:\s*\[[^\]]*\])");
    if (std::regex_search(content, arr_re)) {
        content = std::regex_replace(content, arr_re, replacement_value);
    } else {
        const auto last_brace = content.rfind('}');
        if (last_brace == std::string::npos) {
            return "settings.json does not appear to contain a JSON object; "
                   "extra_dirs not persisted";
        }
        content.insert(last_brace,
                       std::string("  ") + replacement_value + ",\n");
    }

    // Atomic write-and-rename.
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
        fs::remove(tmp_path, ec);
        return std::string("cannot atomically replace '") + settings_path.string()
               + "': " + ec.message();
    }
    return {};
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// AddDirCmd
// ---------------------------------------------------------------------------

class AddDirCmd final : public ISlashCommand {
public:
    // ---- Identity -----------------------------------------------------------

    [[nodiscard]] std::string_view name() const noexcept override {
        return "add-dir";
    }

    [[nodiscard]] std::string_view description() const noexcept override {
        return "Add a directory to the autocomplete/filesystem search roots.";
    }

    [[nodiscard]] std::string_view usage() const noexcept override {
        return "/add-dir <path>";
    }

    [[nodiscard]] std::vector<std::string> aliases() const override {
        return {};
    }

    [[nodiscard]] bool requires_args() const noexcept override { return true; }

    // ---- Execute ------------------------------------------------------------

    [[nodiscard]] batbox::Result<void> execute(
        std::string_view   args,
        CommandContext&    ctx) override;
};

// ---------------------------------------------------------------------------
// execute
// ---------------------------------------------------------------------------

batbox::Result<void> AddDirCmd::execute(
    std::string_view args,
    CommandContext&  ctx)
{
    const std::string_view raw = trim(args);
    if (raw.empty()) {
        return batbox::Err(
            std::string("Usage: ") + std::string(usage()) +
            "\nProvide a directory path to add."
        );
    }

    // Expand ~ and build a fs::path.
    const std::string expanded = expand_tilde(raw);
    fs::path candidate(expanded);

    // Resolve to absolute path relative to cwd when not absolute.
    if (candidate.is_relative()) {
        candidate = ctx.cwd / candidate;
    }

    // Normalize lexically to remove . and .. components.
    candidate = candidate.lexically_normal();

    // Verify the path exists and is a directory.
    std::error_code ec;
    const fs::file_status st = fs::status(candidate, ec);
    if (ec) {
        // ENOENT / ENOTDIR → friendly "does not exist" message.
        // Other errors (permission denied etc.) → report the system error.
        if (ec == std::errc::no_such_file_or_directory
            || ec == std::errc::not_a_directory) {
            return batbox::Err(
                std::string("/add-dir: '") + candidate.string()
                + "' does not exist."
            );
        }
        return batbox::Err(
            std::string("/add-dir: cannot stat '") + candidate.string()
            + "': " + ec.message()
        );
    }
    if (!fs::exists(st)) {
        return batbox::Err(
            std::string("/add-dir: '") + candidate.string()
            + "' does not exist."
        );
    }
    if (!fs::is_directory(st)) {
        return batbox::Err(
            std::string("/add-dir: '") + candidate.string()
            + "' is not a directory."
        );
    }

    // Check readability by attempting to open a directory iterator.
    {
        fs::directory_iterator it(candidate, ec);
        if (ec) {
            return batbox::Err(
                std::string("/add-dir: '") + candidate.string()
                + "' is not readable: " + ec.message()
            );
        }
    }

    // Check for duplicate.
    auto& dirs = extra_dirs_registry();
    const auto it = std::find(dirs.begin(), dirs.end(), candidate);
    if (it != dirs.end()) {
        ctx.output << "/add-dir: '" << candidate.string()
                   << "' is already in the search roots.\n";
        return {};
    }

    // Add to in-process registry.
    dirs.push_back(candidate);

    ctx.output << "Added '" << candidate.string()
               << "' to filesystem search roots.\n";
    ctx.output << "  " << dirs.size()
               << " extra " << (dirs.size() == 1 ? "root" : "roots")
               << " active.\n";

    // Persist to settings.json (non-fatal).
    const std::string persist_err = persist_extra_dirs(ctx.config_dir, dirs);
    if (!persist_err.empty()) {
        ctx.output << "  Warning: could not persist setting — " << persist_err << "\n";
    }

    return {};
}

// ---------------------------------------------------------------------------
// Registration function
// ---------------------------------------------------------------------------

void register_add_dir_cmd(SlashCommandRegistry& registry) {
    auto res = registry.register_command(std::make_shared<AddDirCmd>());
    (void)res;
}

} // namespace batbox::commands
