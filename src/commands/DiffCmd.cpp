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

/// Strip leading and trailing ASCII whitespace.
[[nodiscard]] std::string_view trim(std::string_view s) noexcept {
    const auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return {};
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

/// Run a shell command via popen and capture its stdout + stderr combined.
/// Returns the captured output.  Sets `exit_code` to the process exit status.
/// Returns Err on popen failure.
[[nodiscard]] batbox::Result<std::string> run_command(
        const std::string& cmd,
        int&               exit_code)
{
    // Redirect stderr to stdout so we capture error messages from git.
    const std::string full_cmd = cmd + " 2>&1";

    FILE* fp = ::popen(full_cmd.c_str(), "r");
    if (!fp) {
        return batbox::Err(
            std::string("popen failed: ") + std::strerror(errno)
        );
    }

    std::string output;
    output.reserve(4096);
    std::array<char, 4096> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), fp) != nullptr) {
        output += buf.data();
        // Cap captured output at 1 MiB to avoid overwhelming the terminal.
        if (output.size() >= 1024 * 1024) {
            output += "\n[diff output truncated at 1 MiB]\n";
            break;
        }
    }

    const int status = ::pclose(fp);
    exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1; // NOLINT
    return output;
}

/// Detect whether popen captured a "not a git repository" message.
///
/// git diff in a non-repo directory emits either:
///   "fatal: not a git repository (or any of the parent directories): .git"
///   "warning: Not a git repository. Use --no-index to compare two paths..."
/// We match case-insensitively and also check the exit-code path (exit 128/129).
[[nodiscard]] bool is_not_a_git_repo(const std::string& output, int exit_code) noexcept {
    // Case-insensitive search for the key phrase.
    auto ci_find = [&](std::string_view needle) -> bool {
        // Convert both to lowercase for comparison.
        std::string lower_out = output;
        std::string lower_needle(needle);
        for (char& c : lower_out)   c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        for (char& c : lower_needle) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        return lower_out.find(lower_needle) != std::string::npos;
    };

    if (ci_find("not a git repository")) return true;
    if (ci_find("fatal:"))               return true;

    // git exits 128 for "not a git repository" (fatal error) and 129 for
    // the usage/warning path where it falls back to --no-index hint.
    if (exit_code == 128 || exit_code == 129) return true;

    return false;
}

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
    auto result = run_command(cmd, exit_code);

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
