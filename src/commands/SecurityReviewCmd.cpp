// src/commands/SecurityReviewCmd.cpp
//
// batbox::commands::SecurityReviewCmd — implements the /security-review command.
//
// /security-review [--pr <num>] [--branch <name>]
//
//   Like /review, but the injected prompt instructs the model to perform a
//   security-focused audit targeting OWASP Top-10 vulnerability classes:
//     - Injection (SQL, command, LDAP, XPath, template)
//     - Broken Authentication / Session Management
//     - Sensitive Data Exposure
//     - XML External Entities (XXE)
//     - Broken Access Control
//     - Security Misconfiguration
//     - Cross-Site Scripting (XSS)
//     - Insecure Deserialization
//     - Using Components with Known Vulnerabilities
//     - Insufficient Logging & Monitoring
//   Plus common C++ / systems-programming concerns:
//     - Buffer overflows, use-after-free, integer overflow, format strings
//     - Race conditions and TOCTOU
//     - Improper error handling that leaks internal state
//     - Hard-coded credentials or cryptographic keys
//
//   Gate: identical to /review — if no diff is found the command reports
//   "no changes to review" and returns Ok without injecting any message.
//
//   Diff source and user-confirmation flow are identical to /review.
//
// No aliases.
//
// Registration entry point:
//   void register_security_review_cmd(SlashCommandRegistry&);

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

#ifdef _WIN32
#  include <cstdlib>
#else
#  include <sys/wait.h>
#endif

namespace fs = std::filesystem;

namespace batbox::commands {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace security_review_detail {

/// Shell-quote a token.
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

/// Parse --pr and --branch flags from args.
struct ReviewArgs {
    std::string pr_number;
    std::string branch_name;
};

[[nodiscard]] ReviewArgs parse_review_args(std::string_view args) {
    ReviewArgs ra;
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

/// Obtain diff text and source description.
struct DiffResult {
    std::string diff_text;
    std::string source_desc;
};

[[nodiscard]] batbox::Result<DiffResult> obtain_diff(
        const ReviewArgs& ra,
        const fs::path&   cwd)
{
    std::string cmd;
    std::string source_desc;
    int exit_code = 0;

    if (!ra.pr_number.empty()) {
        cmd         = "gh pr diff " + shell_quote(ra.pr_number) + " --no-color";
        source_desc = "PR #" + ra.pr_number;
    } else if (!ra.branch_name.empty()) {
        const std::string cwd_q = shell_quote(cwd.string());
        cmd         = "git -C " + cwd_q + " diff --no-color "
                    + shell_quote(ra.branch_name) + "...HEAD";
        source_desc = "branch " + ra.branch_name;
    } else {
        const std::string cwd_q = shell_quote(cwd.string());
        cmd         = "git -C " + cwd_q + " diff --no-color";
        source_desc = "working tree";
    }

    auto res = run_git_command(cmd, exit_code);
    if (!res) {
        return batbox::Err(
            std::string("/security-review: failed to obtain diff — ") + res.error());
    }

    const std::string& raw = res.value();

    if (ra.pr_number.empty() && is_not_a_git_repo(raw, exit_code)) {
        return batbox::Err(
            std::string("/security-review: not a git repository (or any parent directory).\n"
                        "  Run /security-review from inside a git repository.")
        );
    }

    return DiffResult{ raw, source_desc };
}

/// Count lines.
[[nodiscard]] std::size_t count_lines(const std::string& s) noexcept {
    if (s.empty()) return 0;
    std::size_t n = 1;
    for (char c : s) if (c == '\n') ++n;
    return n;
}

/// Build the security-focused review prompt injected as a user message.
[[nodiscard]] std::string build_security_prompt(
        const std::string& diff_text,
        const std::string& source_desc)
{
    return
        "You are a senior security engineer. Perform a focused security audit "
        "of the following diff (" + source_desc + ").\n"
        "\n"
        "Examine the changes for ALL of the following vulnerability classes. "
        "For each finding, state: the vulnerability class, the affected file "
        "and line range, a clear explanation of the risk, and a concrete "
        "remediation.\n"
        "\n"
        "OWASP Top-10 categories to check:\n"
        "1. Injection — SQL, command, LDAP, XPath, template injection\n"
        "2. Broken Authentication / Session Management — weak tokens, missing\n"
        "   expiry, plaintext credential storage\n"
        "3. Sensitive Data Exposure — PII/secrets in logs, weak crypto, missing\n"
        "   TLS enforcement\n"
        "4. XML External Entities (XXE) — unconstrained XML/HTML parsers\n"
        "5. Broken Access Control — missing authorization checks, IDOR, path\n"
        "   traversal, privilege escalation\n"
        "6. Security Misconfiguration — default credentials, verbose errors,\n"
        "   unnecessary features enabled\n"
        "7. Cross-Site Scripting (XSS) — unescaped output, innerHTML, eval()\n"
        "8. Insecure Deserialization — untrusted object graphs, class confusion\n"
        "9. Known Vulnerable Components — deprecated APIs, EOL dependencies\n"
        "10. Insufficient Logging & Monitoring — missing audit trails for\n"
        "    sensitive operations\n"
        "\n"
        "Additional C++/systems concerns:\n"
        "- Buffer overflows, out-of-bounds reads/writes\n"
        "- Use-after-free, double-free, dangling pointers\n"
        "- Integer overflow / underflow leading to memory errors\n"
        "- Format string vulnerabilities (printf family)\n"
        "- Race conditions and TOCTOU (time-of-check/time-of-use)\n"
        "- Improper error handling that leaks internal state or stack traces\n"
        "- Hard-coded credentials, API keys, or cryptographic material\n"
        "- Auth-bypass via type confusion, null dereference, or exception abuse\n"
        "\n"
        "If no issues are found in a category, explicitly state "
        "\"No issues found\" for that category.\n"
        "\n"
        "End your report with an overall risk rating: "
        "CRITICAL / HIGH / MEDIUM / LOW / INFORMATIONAL.\n"
        "\n"
        "--- BEGIN DIFF ---\n" + diff_text + "\n--- END DIFF ---\n";
}

} // namespace security_review_detail

// ---------------------------------------------------------------------------
// SecurityReviewCmd
// ---------------------------------------------------------------------------

class SecurityReviewCmd final : public ISlashCommand {
public:
    // ---- Identity -----------------------------------------------------------

    [[nodiscard]] std::string_view name() const noexcept override {
        return "security-review";
    }

    [[nodiscard]] std::string_view description() const noexcept override {
        return "Security audit of current diff: OWASP Top-10, injection, auth-bypass (gated).";
    }

    [[nodiscard]] std::string_view usage() const noexcept override {
        return "/security-review [--pr <num>] [--branch <name>]";
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

batbox::Result<void> SecurityReviewCmd::execute(
    std::string_view args,
    CommandContext&  ctx)
{
    using namespace security_review_detail;

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
        ctx.output << "\n  /security-review: no changes to review"
                   << " (" << source_desc << " is clean).\n\n";
        return {};
    }

    // --- 2. Show diff summary and ask for confirmation -----------------------

    const std::size_t line_count = count_lines(diff_text);

    ctx.output << "\n  /security-review — diff from: " << source_desc << "\n";
    ctx.output << "  " << std::string(50, '-') << "\n";
    ctx.output << "  " << line_count << " line(s) of diff to send for security audit.\n\n";

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

    ctx.output << "\n  Send this diff to the model for a security audit? [y/N] ";
    ctx.output.flush();

    // Read one line of user input.
    std::string answer;
    if (!std::getline(ctx.input, answer)) {
        ctx.output << "\n  /security-review: cancelled (no input).\n\n";
        return {};
    }

    const std::string_view ans_trimmed = trim(answer);
    if (ans_trimmed.empty() || (ans_trimmed[0] != 'y' && ans_trimmed[0] != 'Y')) {
        ctx.output << "\n  /security-review: cancelled.\n\n";
        return {};
    }

    // --- 3. Build and inject the security audit prompt as a user message -----

    const std::string prompt = build_security_prompt(diff_text, source_desc);
    ctx.conversation.inject_user_message(prompt);

    ctx.output << "\n  /security-review: diff injected — model is auditing for vulnerabilities...\n\n";
    return {};
}

// ---------------------------------------------------------------------------
// Registration function
// ---------------------------------------------------------------------------

void register_security_review_cmd(SlashCommandRegistry& registry) {
    auto res = registry.register_command(std::make_shared<SecurityReviewCmd>());
    (void)res;
}

} // namespace batbox::commands
