// src/commands/MemoryCmd.cpp
//
// batbox::commands::MemoryCmd — implements the /memory slash command.
//
// /memory (no args)    — display the content of both BATBOX.md files:
//                        user   (~/.batbox/BATBOX.md)
//                        project (<project-root>/BATBOX.md, walked up from ctx.cwd)
//
// /memory edit         — open the project BATBOX.md (or create it at ctx.cwd)
//                        in $EDITOR, $VISUAL, or nano (fallback) as a subprocess.
//                        If $BATBOX_MEMORY_EDITOR is set it takes full priority
//                        (used by tests to inject a no-op editor).
//
// No aliases.
//
// Registration entry point:
//   void register_memory_cmd(SlashCommandRegistry&);

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/core/Paths.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/repl/CommandContext.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace batbox::commands {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

/// Read the entire content of a text file into a string.
/// Returns an empty string if the file cannot be opened (does not throw).
static std::string read_file_silent(const std::filesystem::path& p) {
    std::ifstream f{p};
    if (!f.is_open()) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// Walk up from `start_dir` looking for a BATBOX.md file.
/// Returns the first path found, or an empty path if none exists.
static std::filesystem::path find_project_batbox_md(
    const std::filesystem::path& start_dir)
{
    namespace fs = std::filesystem;

    fs::path dir = start_dir.empty() ? fs::current_path() : start_dir;

    // Ensure absolute.
    if (!dir.is_absolute()) {
        try {
            dir = fs::canonical(dir);
        } catch (...) {
            dir = fs::current_path() / dir;
        }
    }

    fs::path prev;
    while (dir != prev) {
        const fs::path candidate = dir / "BATBOX.md";
        std::error_code ec;
        if (fs::exists(candidate, ec) && !ec) {
            return candidate;
        }
        prev = dir;
        dir  = dir.parent_path();
    }
    return {};
}

/// Detect the editor to use for `memory edit`.
///
/// Priority:
///   1. BATBOX_MEMORY_EDITOR env var  (test/user override)
///   2. $VISUAL
///   3. $EDITOR
///   4. "nano" (fallback)
static std::string detect_editor() {
    if (const char* e = std::getenv("BATBOX_MEMORY_EDITOR")) {
        return std::string(e);
    }
    if (const char* e = std::getenv("VISUAL"); e && e[0] != '\0') {
        return std::string(e);
    }
    if (const char* e = std::getenv("EDITOR"); e && e[0] != '\0') {
        return std::string(e);
    }
    return "nano";
}

/// Write `header` and `content` to `out` in a styled block.
/// If `content` is empty, writes a "(empty)" notice.
static void emit_section(std::ostream&      out,
                         std::string_view   label,
                         const std::string& path_str,
                         const std::string& content)
{
    out << "\n  " << label << "\n";
    out << "  Path: " << path_str << "\n";
    out << "  " << std::string(60, '-') << "\n";

    if (content.empty()) {
        out << "  (empty)\n";
    } else {
        // Indent each content line by two spaces for readability.
        std::istringstream in{content};
        std::string line;
        while (std::getline(in, line)) {
            out << "  " << line << "\n";
        }
    }
    out << "\n";
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// MemoryCmd
// ---------------------------------------------------------------------------

class MemoryCmd final : public ISlashCommand {
public:
    // ---- Identity -----------------------------------------------------------

    [[nodiscard]] std::string_view name() const noexcept override {
        return "memory";
    }

    [[nodiscard]] std::string_view description() const noexcept override {
        return "Show or edit BATBOX.md memory files (user and project).";
    }

    [[nodiscard]] std::string_view usage() const noexcept override {
        return "/memory [edit]";
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

batbox::Result<void> MemoryCmd::execute(
    std::string_view args,
    CommandContext&  ctx)
{
    namespace fs = std::filesystem;

    // Trim leading/trailing whitespace from args.
    const auto trim = [](std::string_view sv) -> std::string_view {
        const auto s = sv.find_first_not_of(" \t\r\n");
        if (s == std::string_view::npos) return {};
        const auto e = sv.find_last_not_of(" \t\r\n");
        return sv.substr(s, e - s + 1);
    };

    const std::string_view sub = trim(args);

    // ----------------------------------------------------------------
    // Sub-command: "edit" — open project BATBOX.md in $EDITOR
    // ----------------------------------------------------------------
    if (sub == "edit") {
        // Determine path: walk up from cwd; if not found, create at cwd.
        fs::path target = find_project_batbox_md(ctx.cwd);
        if (target.empty()) {
            target = ctx.cwd / "BATBOX.md";
        }

        const std::string editor = detect_editor();
        if (editor.empty()) {
            return batbox::Err(
                std::string("/memory edit: cannot determine editor. "
                            "Set $EDITOR, $VISUAL, or BATBOX_MEMORY_EDITOR."));
        }

        // Build the command: editor <path>
        // Quote the path to handle spaces.
        const std::string cmd = editor + " " + "\"" + target.string() + "\"";

        ctx.output << "Opening " << target.string()
                   << " in " << editor << " ...\n";

        const int rc = std::system(cmd.c_str());  // NOLINT(cert-env33-c)
        if (rc != 0) {
            return batbox::Err(
                std::string("/memory edit: editor exited with status ") +
                std::to_string(rc));
        }

        ctx.output << "Saved. BATBOX.md changes take effect on the next turn.\n";
        return {};
    }

    // ----------------------------------------------------------------
    // Sub-command: (none) — display both BATBOX.md files
    // ----------------------------------------------------------------
    if (!sub.empty() && sub != "view") {
        return batbox::Err(
            std::string("/memory: unknown sub-command '") + std::string(sub) +
            "'. Usage: " + std::string(usage()));
    }

    ctx.output << "\n  BatBox Memory (BATBOX.md)\n";
    ctx.output << "  " << std::string(60, '=') << "\n";

    // ---- User BATBOX.md (~/.batbox/BATBOX.md) ----
    {
        const fs::path user_md = batbox::paths::config_dir() / "BATBOX.md";
        const std::string content = read_file_silent(user_md);
        emit_section(ctx.output, "User memory", user_md.string(), content);
    }

    // ---- Project BATBOX.md (walk up from ctx.cwd) ----
    {
        const fs::path proj_md = find_project_batbox_md(ctx.cwd);

        if (proj_md.empty()) {
            ctx.output << "  Project memory\n";
            ctx.output << "  Path: (none found — use /memory edit to create "
                       << (ctx.cwd / "BATBOX.md").string() << ")\n";
            ctx.output << "  " << std::string(60, '-') << "\n";
            ctx.output << "  (not present)\n\n";
        } else {
            const std::string content = read_file_silent(proj_md);
            emit_section(ctx.output, "Project memory", proj_md.string(), content);
        }
    }

    ctx.output << "  Use /memory edit to open the project BATBOX.md in your editor.\n\n";

    return {};
}

// ---------------------------------------------------------------------------
// Registration function
// ---------------------------------------------------------------------------

void register_memory_cmd(SlashCommandRegistry& registry) {
    auto res = registry.register_command(std::make_shared<MemoryCmd>());
    (void)res;
}

} // namespace batbox::commands
