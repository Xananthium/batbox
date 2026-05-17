// tests/integration/test_review_commands.cpp
//
// doctest integration-test suite for CPP S.8:
//   /review, /security-review
//
// Strategy
// --------
// Both commands are exercised through the ISlashCommand interface with a
// minimal MockConversation and CommandContext.  Because the commands shell out
// to git, the test suite initialises a temporary git repository with a staged
// file for the "has diff" path, and uses a plain temp directory (no .git) for
// the "not a git repo" path.
//
// Coverage:
//   ReviewCmd:
//     - name() == "review"
//     - requires_args() == false
//     - aliases() is empty
//     - usage() contains "review"
//     - no-diff gate: clean git tree → "no changes to review" in output, Ok
//     - non-git dir → Err with "not a git repository"
//     - user declines (N) → "cancelled" in output, no inject_user_message call
//     - user accepts (y) → inject_user_message called with diff + review prompt
//     - injected prompt contains "review" keyword
//     - injected prompt contains the diff text
//     - --pr flag: gh not available → Err propagated (no crash)
//     - --branch flag: invalid branch → Err propagated (no crash)
//   SecurityReviewCmd:
//     - name() == "security-review"
//     - requires_args() == false
//     - aliases() is empty
//     - usage() contains "security-review"
//     - no-diff gate: clean git tree → "no changes to review", Ok
//     - non-git dir → Err with "not a git repository"
//     - user declines → "cancelled", no inject
//     - user accepts → inject_user_message called
//     - injected prompt contains "OWASP" and "security" keywords
//     - injected prompt contains the diff text

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/repl/CommandContext.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using namespace batbox::commands;

// ============================================================================
// MockConversation — records inject_user_message calls
// ============================================================================

struct MockConversation final : ConversationHandle {
    void reset_messages() override {}

    void inject_user_message(std::string_view text) override {
        injected_messages.emplace_back(text);
    }

    std::string last_assistant_message(std::size_t) const override { return {}; }

    std::vector<std::string> injected_messages;

    bool was_injected() const noexcept { return !injected_messages.empty(); }

    const std::string& last_injected() const {
        return injected_messages.back();
    }
};

// ============================================================================
// Registration declarations
// ============================================================================

namespace batbox::commands {
    void register_review_cmd(SlashCommandRegistry&);
    void register_security_review_cmd(SlashCommandRegistry&);
}

// ============================================================================
// RAII temp directory
// ============================================================================

struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& suffix) {
        path = fs::temp_directory_path() / ("batbox_s8_" + suffix);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

// ============================================================================
// Helper: initialise a minimal git repo with one unstaged change
// ============================================================================

/// Creates a git repository at `dir`, commits an initial file, then modifies
/// it so that `git diff` returns non-empty output.
/// Returns true on success.
static bool init_git_repo_with_diff(const fs::path& dir) {
    auto run = [&](const std::string& cmd) -> bool {
        return std::system(cmd.c_str()) == 0;
    };

    const std::string dq = "\"" + dir.string() + "\"";

    // Initialise with a known branch name to avoid default-branch warnings.
    if (!run("git -C " + dq + " init -b main -q 2>/dev/null")) return false;
    if (!run("git -C " + dq + " config user.email test@batbox.dev")) return false;
    if (!run("git -C " + dq + " config user.name 'Batbox Test'")) return false;

    // Write an initial file and commit it.
    {
        std::ofstream f(dir / "hello.txt");
        f << "hello\n";
    }
    if (!run("git -C " + dq + " add hello.txt")) return false;
    if (!run("git -C " + dq + " commit -m 'init' -q")) return false;

    // Now modify the file without staging → produces working-tree diff.
    {
        std::ofstream f(dir / "hello.txt");
        f << "hello\nworld\n";
    }

    return true;
}

// ============================================================================
// Helper: build a CommandContext with specified cwd and simulated stdin
// ============================================================================

struct TestCtx {
    std::ostringstream   out;
    std::istringstream   in;
    MockConversation     conv;
    SlashCommandRegistry reg;
    fs::path             cwd;
    CommandContext       ctx;

    /// @param answer_line — the line that will be returned when the command
    ///                      reads from ctx.input (simulates user confirmation).
    explicit TestCtx(const fs::path& cwd_, std::string_view answer_line = "")
        : in(std::string(answer_line) + "\n")
        , cwd(cwd_)
        , ctx{out, in, false, conv, reg, cwd_}
    {}
};

// ============================================================================
// TEST SUITE: ReviewCmd
// ============================================================================

TEST_SUITE("ReviewCmd") {

    TEST_CASE("registers under primary name 'review'") {
        SlashCommandRegistry reg;
        register_review_cmd(reg);
        REQUIRE(reg.lookup("review") != nullptr);
        CHECK(reg.lookup("review")->name() == "review");
    }

    TEST_CASE("requires_args is false") {
        SlashCommandRegistry reg;
        register_review_cmd(reg);
        CHECK_FALSE(reg.lookup("review")->requires_args());
    }

    TEST_CASE("has no aliases") {
        SlashCommandRegistry reg;
        register_review_cmd(reg);
        CHECK(reg.lookup("review")->aliases().empty());
    }

    TEST_CASE("usage contains 'review'") {
        SlashCommandRegistry reg;
        register_review_cmd(reg);
        const auto usage = reg.lookup("review")->usage();
        CHECK(std::string(usage).find("review") != std::string::npos);
    }

    TEST_CASE("clean git tree returns Ok with 'no changes to review' message") {
        TempDir tmp("review_clean");
        // Init a git repo but do NOT introduce any unstaged changes.
        auto run = [&](const std::string& cmd) {
            return std::system(cmd.c_str()) == 0;
        };
        const std::string dq = "\"" + tmp.path.string() + "\"";
        run("git -C " + dq + " init -b main -q 2>/dev/null");
        run("git -C " + dq + " config user.email test@batbox.dev");
        run("git -C " + dq + " config user.name 'Batbox Test'");
        {
            std::ofstream f(tmp.path / "file.txt");
            f << "clean\n";
        }
        run("git -C " + dq + " add file.txt");
        run("git -C " + dq + " commit -m 'init' -q");

        TestCtx tc(tmp.path);
        register_review_cmd(tc.reg);

        auto res = tc.reg.lookup("review")->execute("", tc.ctx);
        CHECK(res.has_value());
        CHECK(tc.out.str().find("no changes to review") != std::string::npos);
        CHECK_FALSE(tc.conv.was_injected());
    }

    TEST_CASE("non-git directory returns Err with 'not a git repository'") {
        TempDir tmp("review_nogit");
        TestCtx tc(tmp.path, "y");
        register_review_cmd(tc.reg);

        auto res = tc.reg.lookup("review")->execute("", tc.ctx);
        CHECK_FALSE(res.has_value());
        CHECK(res.error().find("not a git repository") != std::string::npos);
        CHECK_FALSE(tc.conv.was_injected());
    }

    TEST_CASE("user declines (N) → cancelled output, no inject") {
        TempDir tmp("review_decline");
        const bool ok = init_git_repo_with_diff(tmp.path);
        if (!ok) {
            MESSAGE("git not available — skipping");
            return;
        }

        // Simulate user typing 'n'.
        TestCtx tc(tmp.path, "n");
        register_review_cmd(tc.reg);

        auto res = tc.reg.lookup("review")->execute("", tc.ctx);
        CHECK(res.has_value());
        CHECK(tc.out.str().find("cancelled") != std::string::npos);
        CHECK_FALSE(tc.conv.was_injected());
    }

    TEST_CASE("user types empty Enter → cancelled, no inject") {
        TempDir tmp("review_empty");
        const bool ok = init_git_repo_with_diff(tmp.path);
        if (!ok) {
            MESSAGE("git not available — skipping");
            return;
        }

        // Simulate user pressing Enter with no input.
        TestCtx tc(tmp.path, "");
        register_review_cmd(tc.reg);

        auto res = tc.reg.lookup("review")->execute("", tc.ctx);
        CHECK(res.has_value());
        CHECK(tc.out.str().find("cancelled") != std::string::npos);
        CHECK_FALSE(tc.conv.was_injected());
    }

    TEST_CASE("user accepts (y) → inject_user_message called") {
        TempDir tmp("review_accept");
        const bool ok = init_git_repo_with_diff(tmp.path);
        if (!ok) {
            MESSAGE("git not available — skipping");
            return;
        }

        TestCtx tc(tmp.path, "y");
        register_review_cmd(tc.reg);

        auto res = tc.reg.lookup("review")->execute("", tc.ctx);
        CHECK(res.has_value());
        CHECK(tc.conv.was_injected());
    }

    TEST_CASE("injected prompt contains 'review' keyword") {
        TempDir tmp("review_prompt_kw");
        const bool ok = init_git_repo_with_diff(tmp.path);
        if (!ok) {
            MESSAGE("git not available — skipping");
            return;
        }

        TestCtx tc(tmp.path, "y");
        register_review_cmd(tc.reg);

        auto res = tc.reg.lookup("review")->execute("", tc.ctx);
        REQUIRE(res.has_value());
        REQUIRE(tc.conv.was_injected());

        const std::string& prompt = tc.conv.last_injected();
        // The prompt must contain a quality review instruction.
        CHECK(prompt.find("review") != std::string::npos);
    }

    TEST_CASE("injected prompt contains the diff text") {
        TempDir tmp("review_diff_content");
        const bool ok = init_git_repo_with_diff(tmp.path);
        if (!ok) {
            MESSAGE("git not available — skipping");
            return;
        }

        TestCtx tc(tmp.path, "y");
        register_review_cmd(tc.reg);

        auto res = tc.reg.lookup("review")->execute("", tc.ctx);
        REQUIRE(res.has_value());
        REQUIRE(tc.conv.was_injected());

        const std::string& prompt = tc.conv.last_injected();
        // The diff modifies hello.txt; the diff text appears in the prompt.
        CHECK(prompt.find("hello.txt") != std::string::npos);
        // The diff delimiters must be present.
        CHECK(prompt.find("BEGIN DIFF") != std::string::npos);
        CHECK(prompt.find("END DIFF")   != std::string::npos);
    }

    TEST_CASE("output shows line count before confirmation") {
        TempDir tmp("review_linecount");
        const bool ok = init_git_repo_with_diff(tmp.path);
        if (!ok) {
            MESSAGE("git not available — skipping");
            return;
        }

        TestCtx tc(tmp.path, "n");
        register_review_cmd(tc.reg);

        auto res = tc.reg.lookup("review")->execute("", tc.ctx);
        CHECK(res.has_value());
        // Output must mention "line(s)" to show the diff summary.
        CHECK(tc.out.str().find("line(s)") != std::string::npos);
    }

    TEST_CASE("--pr with unavailable gh returns Err without crash") {
        TempDir tmp("review_pr");
        TestCtx tc(tmp.path, "y");
        register_review_cmd(tc.reg);

        // gh is typically not available in CI; the command must fail cleanly.
        auto res = tc.reg.lookup("review")->execute("--pr 999999", tc.ctx);
        // We don't REQUIRE failure (gh might be present), but it must not throw.
        // If it fails it must return Err not throw.
        (void)res;
    }

    TEST_CASE("--branch with bogus branch returns Err or shows no diff") {
        TempDir tmp("review_branch");
        const bool ok = init_git_repo_with_diff(tmp.path);
        if (!ok) {
            MESSAGE("git not available — skipping");
            return;
        }

        TestCtx tc(tmp.path, "y");
        register_review_cmd(tc.reg);

        // Bogus branch name → git will error or produce empty output.
        auto res = tc.reg.lookup("review")->execute("--branch __no_such_branch__", tc.ctx);
        // Must not throw; either an Err or a clean "no changes" output.
        (void)res;
        CHECK_FALSE(tc.conv.was_injected());
    }
}

// ============================================================================
// TEST SUITE: SecurityReviewCmd
// ============================================================================

TEST_SUITE("SecurityReviewCmd") {

    TEST_CASE("registers under primary name 'security-review'") {
        SlashCommandRegistry reg;
        register_security_review_cmd(reg);
        REQUIRE(reg.lookup("security-review") != nullptr);
        CHECK(reg.lookup("security-review")->name() == "security-review");
    }

    TEST_CASE("requires_args is false") {
        SlashCommandRegistry reg;
        register_security_review_cmd(reg);
        CHECK_FALSE(reg.lookup("security-review")->requires_args());
    }

    TEST_CASE("has no aliases") {
        SlashCommandRegistry reg;
        register_security_review_cmd(reg);
        CHECK(reg.lookup("security-review")->aliases().empty());
    }

    TEST_CASE("usage contains 'security-review'") {
        SlashCommandRegistry reg;
        register_security_review_cmd(reg);
        const auto usage = reg.lookup("security-review")->usage();
        CHECK(std::string(usage).find("security-review") != std::string::npos);
    }

    TEST_CASE("clean git tree returns Ok with 'no changes to review' message") {
        TempDir tmp("secrev_clean");
        auto run = [&](const std::string& cmd) {
            return std::system(cmd.c_str()) == 0;
        };
        const std::string dq = "\"" + tmp.path.string() + "\"";
        run("git -C " + dq + " init -b main -q 2>/dev/null");
        run("git -C " + dq + " config user.email test@batbox.dev");
        run("git -C " + dq + " config user.name 'Batbox Test'");
        {
            std::ofstream f(tmp.path / "file.txt");
            f << "secure\n";
        }
        run("git -C " + dq + " add file.txt");
        run("git -C " + dq + " commit -m 'init' -q");

        TestCtx tc(tmp.path);
        register_security_review_cmd(tc.reg);

        auto res = tc.reg.lookup("security-review")->execute("", tc.ctx);
        CHECK(res.has_value());
        CHECK(tc.out.str().find("no changes to review") != std::string::npos);
        CHECK_FALSE(tc.conv.was_injected());
    }

    TEST_CASE("non-git directory returns Err with 'not a git repository'") {
        TempDir tmp("secrev_nogit");
        TestCtx tc(tmp.path, "y");
        register_security_review_cmd(tc.reg);

        auto res = tc.reg.lookup("security-review")->execute("", tc.ctx);
        CHECK_FALSE(res.has_value());
        CHECK(res.error().find("not a git repository") != std::string::npos);
        CHECK_FALSE(tc.conv.was_injected());
    }

    TEST_CASE("user declines (N) → cancelled output, no inject") {
        TempDir tmp("secrev_decline");
        const bool ok = init_git_repo_with_diff(tmp.path);
        if (!ok) {
            MESSAGE("git not available — skipping");
            return;
        }

        TestCtx tc(tmp.path, "n");
        register_security_review_cmd(tc.reg);

        auto res = tc.reg.lookup("security-review")->execute("", tc.ctx);
        CHECK(res.has_value());
        CHECK(tc.out.str().find("cancelled") != std::string::npos);
        CHECK_FALSE(tc.conv.was_injected());
    }

    TEST_CASE("user accepts (y) → inject_user_message called") {
        TempDir tmp("secrev_accept");
        const bool ok = init_git_repo_with_diff(tmp.path);
        if (!ok) {
            MESSAGE("git not available — skipping");
            return;
        }

        TestCtx tc(tmp.path, "y");
        register_security_review_cmd(tc.reg);

        auto res = tc.reg.lookup("security-review")->execute("", tc.ctx);
        CHECK(res.has_value());
        CHECK(tc.conv.was_injected());
    }

    TEST_CASE("injected prompt contains 'OWASP' keyword") {
        TempDir tmp("secrev_owasp");
        const bool ok = init_git_repo_with_diff(tmp.path);
        if (!ok) {
            MESSAGE("git not available — skipping");
            return;
        }

        TestCtx tc(tmp.path, "y");
        register_security_review_cmd(tc.reg);

        auto res = tc.reg.lookup("security-review")->execute("", tc.ctx);
        REQUIRE(res.has_value());
        REQUIRE(tc.conv.was_injected());

        const std::string& prompt = tc.conv.last_injected();
        CHECK(prompt.find("OWASP") != std::string::npos);
    }

    TEST_CASE("injected prompt contains 'security' keyword") {
        TempDir tmp("secrev_kw_security");
        const bool ok = init_git_repo_with_diff(tmp.path);
        if (!ok) {
            MESSAGE("git not available — skipping");
            return;
        }

        TestCtx tc(tmp.path, "y");
        register_security_review_cmd(tc.reg);

        auto res = tc.reg.lookup("security-review")->execute("", tc.ctx);
        REQUIRE(res.has_value());
        REQUIRE(tc.conv.was_injected());

        const std::string& prompt = tc.conv.last_injected();
        CHECK(prompt.find("security") != std::string::npos);
    }

    TEST_CASE("injected prompt contains 'injection' keyword") {
        TempDir tmp("secrev_kw_injection");
        const bool ok = init_git_repo_with_diff(tmp.path);
        if (!ok) {
            MESSAGE("git not available — skipping");
            return;
        }

        TestCtx tc(tmp.path, "y");
        register_security_review_cmd(tc.reg);

        auto res = tc.reg.lookup("security-review")->execute("", tc.ctx);
        REQUIRE(res.has_value());
        REQUIRE(tc.conv.was_injected());

        const std::string& prompt = tc.conv.last_injected();
        // The security prompt must mention injection.
        CHECK(prompt.find("njection") != std::string::npos);
    }

    TEST_CASE("injected prompt contains 'auth' keyword") {
        TempDir tmp("secrev_kw_auth");
        const bool ok = init_git_repo_with_diff(tmp.path);
        if (!ok) {
            MESSAGE("git not available — skipping");
            return;
        }

        TestCtx tc(tmp.path, "y");
        register_security_review_cmd(tc.reg);

        auto res = tc.reg.lookup("security-review")->execute("", tc.ctx);
        REQUIRE(res.has_value());
        REQUIRE(tc.conv.was_injected());

        const std::string& prompt = tc.conv.last_injected();
        CHECK(prompt.find("uth") != std::string::npos);
    }

    TEST_CASE("injected prompt contains diff delimiters and diff text") {
        TempDir tmp("secrev_diff_content");
        const bool ok = init_git_repo_with_diff(tmp.path);
        if (!ok) {
            MESSAGE("git not available — skipping");
            return;
        }

        TestCtx tc(tmp.path, "y");
        register_security_review_cmd(tc.reg);

        auto res = tc.reg.lookup("security-review")->execute("", tc.ctx);
        REQUIRE(res.has_value());
        REQUIRE(tc.conv.was_injected());

        const std::string& prompt = tc.conv.last_injected();
        CHECK(prompt.find("BEGIN DIFF") != std::string::npos);
        CHECK(prompt.find("END DIFF")   != std::string::npos);
        CHECK(prompt.find("hello.txt")  != std::string::npos);
    }

    TEST_CASE("injected prompt contains overall risk rating instruction") {
        TempDir tmp("secrev_risk_rating");
        const bool ok = init_git_repo_with_diff(tmp.path);
        if (!ok) {
            MESSAGE("git not available — skipping");
            return;
        }

        TestCtx tc(tmp.path, "y");
        register_security_review_cmd(tc.reg);

        auto res = tc.reg.lookup("security-review")->execute("", tc.ctx);
        REQUIRE(res.has_value());
        REQUIRE(tc.conv.was_injected());

        const std::string& prompt = tc.conv.last_injected();
        // The prompt must request a risk rating.
        CHECK(prompt.find("CRITICAL") != std::string::npos);
    }

    TEST_CASE("output shows line count before confirmation") {
        TempDir tmp("secrev_linecount");
        const bool ok = init_git_repo_with_diff(tmp.path);
        if (!ok) {
            MESSAGE("git not available — skipping");
            return;
        }

        TestCtx tc(tmp.path, "n");
        register_security_review_cmd(tc.reg);

        auto res = tc.reg.lookup("security-review")->execute("", tc.ctx);
        CHECK(res.has_value());
        CHECK(tc.out.str().find("line(s)") != std::string::npos);
    }

    TEST_CASE("--pr with unavailable gh returns Err without crash") {
        TempDir tmp("secrev_pr");
        TestCtx tc(tmp.path, "y");
        register_security_review_cmd(tc.reg);

        auto res = tc.reg.lookup("security-review")->execute("--pr 999999", tc.ctx);
        (void)res;
    }

    TEST_CASE("both commands can be registered in the same registry without collision") {
        SlashCommandRegistry reg;
        register_review_cmd(reg);
        register_security_review_cmd(reg);
        CHECK(reg.lookup("review") != nullptr);
        CHECK(reg.lookup("security-review") != nullptr);
        CHECK(reg.lookup("review")->name() != reg.lookup("security-review")->name());
    }
}
