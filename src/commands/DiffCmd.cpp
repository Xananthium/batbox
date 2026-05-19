// src/commands/DiffCmd.cpp
//
// batbox::commands::DiffCmd — implements the /diff slash command.
//
// /diff [file]
//   Shells out to `git diff --no-color` (via popen) to obtain the unstaged
//   working-tree diff.  With an optional file argument only that file's diff
//   is shown.
//
//   The diff output is rendered to ctx.output with minimal ANSI-free
//   decoration: added lines (+) are preceded by a '+' indicator, removed
//   lines (-) by a '-' indicator, hunk headers (@@ … @@) by a '~' indicator,
//   and context lines are shown unchanged.
//
//   When the process is not inside a git repository the command prints a
//   clear, user-readable error message (no crash or exception).
//
// No aliases.
//
// Registration entry point:
//   void register_diff_cmd(SlashCommandRegistry&);

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/commands/CommandHelpers.hpp>
#include <batbox/repl/CommandContext.hpp>

#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace batbox::commands {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

/// Render a raw unified diff string to `out` with minimal decoration.
///
/// Format:
///   added lines    — "  + <text>"
///   removed lines  — "  - <text>"
///   hunk headers   — "  ~ <hunk>"
///   file headers   — shown verbatim with "  | " prefix
///   context lines  — "    <text>"
void render_diff(std::ostream& out, const std::string& diff_text) {
    std::istringstream stream(diff_text);
    std::string line;
    std::size_t line_count = 0;

    while (std::getline(stream, line)) {
        ++line_count;
        if (line.empty()) {
            out << '\n';
            continue;
        }

        const char first = line[0];

        if (first == '+' && line.size() >= 3 && line[1] == '+' && line[2] == '+') {
            // File header "+++"
            out << "  | " << line << '\n';
        } else if (first == '-' && line.size() >= 3 && line[1] == '-' && line[2] == '-') {
            // File header "---"
            out << "  | " << line << '\n';
        } else if (first == '@') {
            // Hunk header
            out << "  ~ " << line << '\n';
        } else if (first == '+') {
            out << "  + " << line.substr(1) << '\n';
        } else if (first == '-') {
            out << "  - " << line.substr(1) << '\n';
        } else if (first == '\\') {
            // "\ No newline at end of file"
            out << "    " << line << '\n';
        } else {
            // Context line (starts with space or no recognisable prefix)
            if (line[0] == ' ') {
                out << "    " << line.substr(1) << '\n';
            } else {
                out << "    " << line << '\n';
            }
        }
    }

    if (line_count == 0) {
        out << "  (no diff output)\n";
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// DiffCmd
// ---------------------------------------------------------------------------

class DiffCmd final : public ISlashCommand {
public:
    // ---- Identity -----------------------------------------------------------

    [[nodiscard]] std::string_view name() const noexcept override {
        return "diff";
    }

    [[nodiscard]] std::string_view description() const noexcept override {
        return "Show the unstaged working-tree diff (git diff --no-color).";
    }

    [[nodiscard]] std::string_view usage() const noexcept override {
        return "/diff [file]";
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

batbox::Result<void> DiffCmd::execute(
    std::string_view args,
    CommandContext&  ctx)
{
    const std::string_view file_arg = trim(args);

    // Build the git diff command.
    // --no-color ensures ANSI-free output regardless of user git config.
    std::string cmd = "git -C ";

    // Quote the cwd path to handle spaces.
    const std::string cwd_str = ctx.cwd.string();
    cmd += '"';
    for (char c : cwd_str) {
        if (c == '"') cmd += "\\\"";
        else cmd += c;
    }
    cmd += '"';

    cmd += " diff --no-color";

    if (!file_arg.empty()) {
        cmd += " -- ";
        // Quote the file argument.
        cmd += '"';
        const std::string fa(file_arg);
        for (char c : fa) {
            if (c == '"') cmd += "\\\"";
            else cmd += c;
        }
        cmd += '"';
    }

    int exit_code = 0;
    auto result = run_git_command(cmd, exit_code);

    if (!result) {
        return batbox::Err(
            std::string("/diff: failed to run git diff — ") + result.error()
        );
    }

    const std::string& output = result.value();

    // Check if we're not in a git repository.
    if (is_not_a_git_repo(output, exit_code)) {
        return batbox::Err(
            std::string("/diff: not a git repository (or any parent directory).\n"
                        "  Run /diff from inside a git repository.\n"
                        "  Use `git init` to initialise one if needed.")
        );
    }

    // Empty diff = clean working tree.
    if (output.empty()) {
        ctx.output << "\n  No unstaged changes";
        if (!file_arg.empty()) {
            ctx.output << " in '" << file_arg << "'";
        }
        ctx.output << ".\n\n";
        return {};
    }

    // Render header.
    ctx.output << "\n  Unstaged diff";
    if (!file_arg.empty()) {
        ctx.output << " — " << file_arg;
    }
    ctx.output << "\n  " << std::string(50, '-') << "\n";

    // Render the diff with decoration.
    render_diff(ctx.output, output);

    ctx.output << "\n  Legend:  + added   - removed   ~ hunk   | header\n\n";

    return {};
}

// ---------------------------------------------------------------------------
// Registration function
// ---------------------------------------------------------------------------

void register_diff_cmd(SlashCommandRegistry& registry) {
    auto res = registry.register_command(std::make_shared<DiffCmd>());
    (void)res;
}

} // namespace batbox::commands
