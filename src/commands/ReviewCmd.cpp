// src/commands/ReviewCmd.cpp
//
// batbox::commands::ReviewCmd — implements the /review slash command.
//
// /review [--pr <num>] [--branch <name>]
//
//   Obtains a git diff, presents it to the user for confirmation, then injects
//   the diff together with a general code-quality review prompt as a user
//   message into the current conversation, triggering a model turn.
//
//   Gate: if no diff is found (clean working tree with no --pr / --branch
//   flags), the command reports "no changes to review" and returns Ok without
//   injecting any message.
//
//   Diff source (in priority order):
//     1. --pr <num>     git diff from `gh pr diff <num> --no-color`
//     2. --branch <name> git diff from `git diff --no-color <name>...HEAD`
//     3. (default)      unstaged working-tree diff (`git diff --no-color`)
//
//   User confirmation:
//     After displaying the diff summary the user is asked:
//       "Send this diff to the model for review? [y/N] "
//     Any input other than 'y' or 'Y' cancels without injecting.
//
// No aliases.
//
// Registration entry point:
//   void register_review_cmd(SlashCommandRegistry&);

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

#ifdef _WIN32
#  include <cstdlib>   // for pclose/popen compatibility
#else
#  include <sys/wait.h>
#endif

namespace fs = std::filesystem;

namespace batbox::commands {

// ---------------------------------------------------------------------------
// Internal helpers (review namespace to avoid ODR conflicts with DiffCmd.cpp)
// ---------------------------------------------------------------------------

namespace review_detail {

/// Strip leading and trailing ASCII whitespace.
[[nodiscard]] std::string_view trim(std::string_view s) noexcept {
    const auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return {};
    const auto end   = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

/// Run a shell command via popen, capture combined stdout+stderr.
/// Sets exit_code on return.  Returns Err on popen failure.
[[nodiscard]] batbox::Result<std::string> run_command(
        const std::string& cmd,
        int&               exit_code)
{
    const std::string full_cmd = cmd + " 2>&1";
    FILE* fp = ::popen(full_cmd.c_str(), "r");
    if (!fp) {
        return batbox::Err(std::string("popen failed: ") + std::strerror(errno));
    }

    std::string output;
    output.reserve(4096);
    std::array<char, 4096> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), fp) != nullptr) {
        output += buf.data();
        if (output.size() >= 1024 * 1024) {
            output += "\n[diff output truncated at 1 MiB]\n";
            break;
        }
    }

    const int status = ::pclose(fp);
#ifdef _WIN32
    exit_code = status;
#else
    exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1; // NOLINT
#endif
    return output;
}

/// Detect a "not a git repository" error in captured output + exit code.
[[nodiscard]] bool is_not_a_git_repo(const std::string& output, int exit_code) noexcept {
    auto ci_find = [&](std::string_view needle) -> bool {
        std::string lo = output, ln(needle);
        for (char& c : lo) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        for (char& c : ln) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        return lo.find(ln) != std::string::npos;
    };
    if (ci_find("not a git repository")) return true;
    if (ci_find("fatal:"))              return true;
    if (exit_code == 128 || exit_code == 129) return true;
    return false;
}

/// Shell-quote a token by wrapping in single quotes and escaping embedded
/// single quotes as '\''.
[[nodiscard]] std::string shell_quote(std::string_view token) {
    std::string result;
    result.reserve(token.size() + 4);
    result += '\'';
    for (char c : token) {
        if (c == '\'') result += "'\\''";
        else           result += c;
    }
    result += '\'';
    return result;
}

/// Simple argument parser: extracts --pr and --branch flag values from args.
struct ReviewArgs {
    std::string pr_number;     // non-empty iff --pr was given
    std::string branch_name;   // non-empty iff --branch was given
};

[[nodiscard]] ReviewArgs parse_review_args(std::string_view args) {
    ReviewArgs ra;
    // Tokenise by whitespace.
    std::istringstream iss{std::string(args)};
    std::string tok;
    while (iss >> tok) {
        if (tok == "--pr") {
            std::string val;
            if (iss >> val) ra.pr_number = val;
        } else if (tok == "--branch") {
            std::string val;
            if (iss >> val) ra.branch_name = val;
        }
    }
    return ra;
}

/// Obtain diff text and a human-readable source description.
/// Returns Err on git/gh failure, Ok("") when no diff found (clean state).
struct DiffResult {
    std::string diff_text;
    std::string source_desc;   // e.g. "PR #42", "branch main", "working tree"
};

[[nodiscard]] batbox::Result<DiffResult> obtain_diff(
        const ReviewArgs& ra,
        const fs::path&   cwd)
{
    std::string cmd;
    std::string source_desc;
    int exit_code = 0;

    if (!ra.pr_number.empty()) {
        // gh pr diff <num> --no-color
        cmd         = "gh pr diff " + shell_quote(ra.pr_number) + " --no-color";
        source_desc = "PR #" + ra.pr_number;
    } else if (!ra.branch_name.empty()) {
        // git diff --no-color <branch>...HEAD
        const std::string cwd_q = shell_quote(cwd.string());
        cmd         = "git -C " + cwd_q + " diff --no-color "
                    + shell_quote(ra.branch_name) + "...HEAD";
        source_desc = "branch " + ra.branch_name;
    } else {
        // Default: unstaged working-tree diff.
        const std::string cwd_q = shell_quote(cwd.string());
        cmd         = "git -C " + cwd_q + " diff --no-color";
        source_desc = "working tree";
    }

    auto res = run_command(cmd, exit_code);
    if (!res) {
        return batbox::Err(std::string("/review: failed to obtain diff — ") + res.error());
    }

    const std::string& raw = res.value();

    // Check for git-not-a-repo error (only relevant for git-based diffs).
    if (ra.pr_number.empty() && is_not_a_git_repo(raw, exit_code)) {
        return batbox::Err(
            std::string("/review: not a git repository (or any parent directory).\n"
                        "  Run /review from inside a git repository.")
        );
    }

    return DiffResult{ raw, source_desc };
}

/// Count lines in a diff string; return 0 when empty.
[[nodiscard]] std::size_t count_lines(const std::string& s) noexcept {
    if (s.empty()) return 0;
    std::size_t n = 1;
    for (char c : s) if (c == '\n') ++n;
    return n;
}

/// Build the review prompt injected as a user message.
[[nodiscard]] std::string build_review_prompt(
        const std::string& diff_text,
        const std::string& source_desc)
{
    return
        "Please review the following diff (" + source_desc + ") for code quality.\n"
        "\n"
        "Focus on:\n"
        "- Correctness: logic errors, edge cases, off-by-one errors\n"
        "- Readability: naming, comments, complexity, dead code\n"
        "- Maintainability: coupling, cohesion, duplication, extensibility\n"
        "- Performance: unnecessary allocations, O(n²) patterns, blocking I/O\n"
        "- Test coverage: are critical paths exercised?\n"
        "- Style: consistency with the surrounding codebase\n"
        "\n"
        "For each issue, cite the specific file and line range, explain the "
        "problem, and suggest a concrete fix.\n"
        "\n"
        "--- BEGIN DIFF ---\n" + diff_text + "\n--- END DIFF ---\n";
}

} // namespace review_detail

// ---------------------------------------------------------------------------
// ReviewCmd
// ---------------------------------------------------------------------------

class ReviewCmd final : public ISlashCommand {
public:
    // ---- Identity -----------------------------------------------------------

    [[nodiscard]] std::string_view name() const noexcept override {
        return "review";
    }

    [[nodiscard]] std::string_view description() const noexcept override {
        return "Review current diff for code quality (gated: auto-runs /diff if needed).";
    }

    [[nodiscard]] std::string_view usage() const noexcept override {
        return "/review [--pr <num>] [--branch <name>]";
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

batbox::Result<void> ReviewCmd::execute(
    std::string_view args,
    CommandContext&  ctx)
{
    using namespace review_detail;

    const ReviewArgs ra = parse_review_args(args);

    // --- 1. Obtain diff ------------------------------------------------------

    auto diff_res = obtain_diff(ra, ctx.cwd);
    if (!diff_res) {
        return batbox::Err(diff_res.error());
    }

    const std::string& diff_text   = diff_res.value().diff_text;
    const std::string& source_desc = diff_res.value().source_desc;

    // Gate: no changes → nothing to review.
    if (trim(diff_text).empty()) {
        ctx.output << "\n  /review: no changes to review"
                   << " (" << source_desc << " is clean).\n\n";
        return {};
    }

    // --- 2. Show diff summary and ask for confirmation -----------------------

    const std::size_t line_count = count_lines(diff_text);

    ctx.output << "\n  /review — diff from: " << source_desc << "\n";
    ctx.output << "  " << std::string(50, '-') << "\n";
    ctx.output << "  " << line_count << " line(s) of diff to send for review.\n\n";

    // Print a compact preview (first 20 lines).
    {
        std::istringstream preview_stream(diff_text);
        std::string line;
        std::size_t shown = 0;
        while (std::getline(preview_stream, line) && shown < 20) {
            ctx.output << "  " << line << "\n";
            ++shown;
        }
        if (line_count > 20) {
            ctx.output << "  ... (" << (line_count - 20) << " more line(s))\n";
        }
    }

    ctx.output << "\n  Send this diff to the model for review? [y/N] ";
    ctx.output.flush();

    // Read one line of user input.
    std::string answer;
    if (!std::getline(ctx.input, answer)) {
        ctx.output << "\n  /review: cancelled (no input).\n\n";
        return {};
    }

    const std::string_view ans_trimmed = trim(answer);
    if (ans_trimmed.empty() || (ans_trimmed[0] != 'y' && ans_trimmed[0] != 'Y')) {
        ctx.output << "\n  /review: cancelled.\n\n";
        return {};
    }

    // --- 3. Build and inject the review prompt as a user message -------------

    const std::string prompt = build_review_prompt(diff_text, source_desc);
    ctx.conversation.inject_user_message(prompt);

    ctx.output << "\n  /review: diff injected — model is reviewing...\n\n";
    return {};
}

// ---------------------------------------------------------------------------
// Registration function
// ---------------------------------------------------------------------------

void register_review_cmd(SlashCommandRegistry& registry) {
    auto res = registry.register_command(std::make_shared<ReviewCmd>());
    (void)res;
}

} // namespace batbox::commands
