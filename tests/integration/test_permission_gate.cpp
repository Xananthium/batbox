// tests/integration/test_permission_gate.cpp
// ---------------------------------------------------------------------------
// doctest integration tests for batbox::permissions::PermissionGate.
//
// Covers all acceptance criteria from task CPP 12.4:
//   [AC1] Nuclear mode → Always returns Allow regardless of rules or tool
//   [AC2] Plan mode + write tool → Returns Deny with "plan mode: read-only" reason
//   [AC3] AcceptEdits mode + Edit tool → Returns Allow (no prompt)
//   [AC4] Default mode + matching deny rule → Returns Deny
//   [AC5] Default mode + matching allow rule → Returns Allow
//   [AC6] Default mode + no matching rule → Calls prompt_user_ callback
//   [AC7] Unit tests use a mock prompt_user_ callback (no TUI required)
//   [AC8] BATBOX_AUTO_APPROVE_READS=true: read-only tools auto-allowed
//
// Also tests:
//   - Decision struct factories (allow, deny, allow_with_rule, deny_with_rule)
//   - set_mode() / current_mode() thread-safety
//   - Persist-rule: "always allow" callback result is written to PermissionStore
//   - Persist-rule: "always deny" callback result is written to PermissionStore
//   - Deny rules checked before allow rules (priority)
//   - AcceptEdits + Write → Allow; AcceptEdits + Bash → prompt
//   - AcceptEdits + MultiEdit → Allow
//   - Plan mode + Read (read-only tool) → Allow (passes through)
//   - Null prompt_fn defaults to Deny
//   - Nuclear short-circuits before any rule checking
//
// Build (standalone, no CMake):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/integration/test_permission_gate.cpp \
//       src/permissions/PermissionGate.cpp \
//       src/permissions/PermissionMode.cpp \
//       src/permissions/PermissionRule.cpp \
//       src/permissions/PatternMatcher.cpp \
//       src/permissions/PermissionStore.cpp \
//       src/config/SettingsLoader.cpp \
//       src/core/Json.cpp \
//       src/core/Paths.cpp \
//       src/core/CancelToken.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       -o /tmp/test_pg && /tmp/test_pg
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/permissions/PermissionGate.hpp>
#include <batbox/permissions/PermissionStore.hpp>
#include <batbox/permissions/PermissionMode.hpp>
#include <batbox/permissions/PermissionRule.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace batbox::permissions;
using batbox::Json;
using batbox::tools::ToolContext;

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

/// Create a unique temporary settings file path for a test.
static fs::path make_temp_settings() {
    static std::atomic<int> counter{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const int seq = ++counter;
    const std::string dir_name =
        "test_pgate_" + std::to_string(now) + "_" + std::to_string(seq);
    const fs::path tmp = fs::temp_directory_path() / dir_name;
    fs::create_directories(tmp);
    return tmp / "settings.json";
}

/// Build a minimal ToolContext with the given permission mode.
static ToolContext make_ctx(PermissionMode mode = PermissionMode::Default) {
    ToolContext ctx;
    ctx.cwd        = fs::temp_directory_path();
    ctx.mode       = mode;
    ctx.session_id = "test-session";
    ctx.agent_id   = "";
    return ctx;
}

/// A mock PromptFn that always returns Allow (one-shot).
static Decision mock_allow(std::string_view, const Json&) {
    return Decision::allow();
}

/// A mock PromptFn that always returns Deny (one-shot).
static Decision mock_deny(std::string_view, const Json&) {
    return Decision::deny();
}

/// Build a PermissionGate with an empty store and the given mode + callback.
static PermissionGate make_gate(PermissionMode mode,
                                 PermissionGate::PromptFn callback,
                                 fs::path* out_settings_path = nullptr)
{
    const fs::path p = make_temp_settings();
    if (out_settings_path) *out_settings_path = p;
    auto store = std::make_shared<PermissionStore>(p);
    return PermissionGate(store, mode, std::move(callback));
}

/// Build a Bash args object with the given command.
static Json bash_args(const std::string& cmd) {
    return {{"command", cmd}};
}

/// Build a Read args object with the given file path.
static Json read_args(const std::string& path) {
    return {{"file_path", path}};
}

/// Build a Write args object with the given file path.
static Json write_args(const std::string& path) {
    return {{"file_path", path}};
}

// ===========================================================================
// SUITE 1: Decision struct factories
// ===========================================================================
TEST_SUITE("Decision factories") {

    TEST_CASE("allow() produces Allow kind, no persist_rule, no edit_text") {
        auto d = Decision::allow();
        CHECK(d.kind == Decision::Kind::Allow);
        CHECK(!d.persist_rule.has_value());
        CHECK(!d.edit_text.has_value());
    }

    TEST_CASE("deny() produces Deny kind, no persist_rule, no edit_text") {
        auto d = Decision::deny();
        CHECK(d.kind == Decision::Kind::Deny);
        CHECK(!d.persist_rule.has_value());
        CHECK(!d.edit_text.has_value());
    }

    TEST_CASE("allow_with_rule() produces Allow kind + Allow persist_rule") {
        auto d = Decision::allow_with_rule("Bash(git *)");
        CHECK(d.kind == Decision::Kind::Allow);
        REQUIRE(d.persist_rule.has_value());
        CHECK(d.persist_rule->pattern == "Bash(git *)");
        CHECK(d.persist_rule->kind == PermissionRule::Kind::Allow);
    }

    TEST_CASE("deny_with_rule() produces Deny kind + Deny persist_rule") {
        auto d = Decision::deny_with_rule("Bash(rm -rf *)");
        CHECK(d.kind == Decision::Kind::Deny);
        REQUIRE(d.persist_rule.has_value());
        CHECK(d.persist_rule->pattern == "Bash(rm -rf *)");
        CHECK(d.persist_rule->kind == PermissionRule::Kind::Deny);
    }
}

// ===========================================================================
// SUITE 2: set_mode / current_mode
// ===========================================================================
TEST_SUITE("PermissionGate — set_mode / current_mode") {

    TEST_CASE("current_mode() returns initial mode") {
        auto gate = make_gate(PermissionMode::Nuclear, mock_allow);
        CHECK(gate.current_mode() == PermissionMode::Nuclear);
    }

    TEST_CASE("set_mode() updates mode returned by current_mode()") {
        auto gate = make_gate(PermissionMode::Default, mock_allow);
        CHECK(gate.current_mode() == PermissionMode::Default);

        gate.set_mode(PermissionMode::Plan);
        CHECK(gate.current_mode() == PermissionMode::Plan);

        gate.set_mode(PermissionMode::AcceptEdits);
        CHECK(gate.current_mode() == PermissionMode::AcceptEdits);

        gate.set_mode(PermissionMode::Nuclear);
        CHECK(gate.current_mode() == PermissionMode::Nuclear);
    }
}

// ===========================================================================
// SUITE 3: Nuclear mode (AC1)
// ===========================================================================
TEST_SUITE("PermissionGate — Nuclear mode") {

    TEST_CASE("[AC1] Nuclear: returns Allow for any tool, no rules checked") {
        // Even with a deny rule that would match, Nuclear short-circuits.
        const fs::path p = make_temp_settings();
        auto store = std::make_shared<PermissionStore>(p);
        REQUIRE(store->add_deny_rule("Bash(rm -rf *)").has_value());

        // prompt_fn that would fail the test if called
        bool prompt_called = false;
        auto prompt = [&](std::string_view, const Json&) {
            prompt_called = true;
            return Decision::deny();
        };

        PermissionGate gate(store, PermissionMode::Default, prompt);

        // Use ctx with Nuclear mode — this is the authoritative mode per contract
        auto ctx = make_ctx(PermissionMode::Nuclear);
        auto d = gate.ask("Bash", bash_args("rm -rf /tmp/foo"), ctx);

        CHECK(d.kind == Decision::Kind::Allow);
        CHECK(!prompt_called);  // prompt must NOT be called in Nuclear mode

        fs::remove_all(p.parent_path());
    }

    TEST_CASE("Nuclear: allows Read, Write, Edit, and unknown tools") {
        auto gate = make_gate(PermissionMode::Default, mock_deny);
        auto ctx = make_ctx(PermissionMode::Nuclear);

        CHECK(gate.ask("Read",    read_args("./secret.txt"), ctx).kind == Decision::Kind::Allow);
        CHECK(gate.ask("Write",   write_args("./out.txt"),   ctx).kind == Decision::Kind::Allow);
        CHECK(gate.ask("Edit",    write_args("./main.cpp"),  ctx).kind == Decision::Kind::Allow);
        CHECK(gate.ask("Bash",    bash_args("rm -rf /"),     ctx).kind == Decision::Kind::Allow);
        CHECK(gate.ask("Unknown", Json::object(),            ctx).kind == Decision::Kind::Allow);
    }
}

// ===========================================================================
// SUITE 4: Plan mode (AC2)
// ===========================================================================
TEST_SUITE("PermissionGate — Plan mode") {

    TEST_CASE("[AC2] Plan + Write → Deny") {
        auto gate = make_gate(PermissionMode::Default, mock_allow);
        auto ctx = make_ctx(PermissionMode::Plan);

        auto d = gate.ask("Write", write_args("./output.txt"), ctx);
        CHECK(d.kind == Decision::Kind::Deny);
    }

    TEST_CASE("Plan + Edit → Deny") {
        auto gate = make_gate(PermissionMode::Default, mock_allow);
        auto ctx = make_ctx(PermissionMode::Plan);

        auto d = gate.ask("Edit", write_args("./main.cpp"), ctx);
        CHECK(d.kind == Decision::Kind::Deny);
    }

    TEST_CASE("Plan + Bash → Deny") {
        auto gate = make_gate(PermissionMode::Default, mock_allow);
        auto ctx = make_ctx(PermissionMode::Plan);

        auto d = gate.ask("Bash", bash_args("npm install"), ctx);
        CHECK(d.kind == Decision::Kind::Deny);
    }

    TEST_CASE("Plan + Read → Allow (read-only tool passes through)") {
        auto gate = make_gate(PermissionMode::Default, mock_allow);
        auto ctx = make_ctx(PermissionMode::Plan);

        auto d = gate.ask("Read", read_args("./src/main.cpp"), ctx);
        CHECK(d.kind == Decision::Kind::Allow);
    }

    TEST_CASE("Plan + Glob → Allow (read-only tool)") {
        auto gate = make_gate(PermissionMode::Default, mock_allow);
        auto ctx = make_ctx(PermissionMode::Plan);

        auto d = gate.ask("Glob", Json{{"pattern", "src/**/*.cpp"}}, ctx);
        CHECK(d.kind == Decision::Kind::Allow);
    }

    TEST_CASE("Plan + Grep → Allow (read-only tool)") {
        auto gate = make_gate(PermissionMode::Default, mock_allow);
        auto ctx = make_ctx(PermissionMode::Plan);

        auto d = gate.ask("Grep", Json{{"pattern", "TODO"}}, ctx);
        CHECK(d.kind == Decision::Kind::Allow);
    }

    TEST_CASE("Plan + CtxInspect → Allow (read-only tool)") {
        auto gate = make_gate(PermissionMode::Default, mock_allow);
        auto ctx = make_ctx(PermissionMode::Plan);

        auto d = gate.ask("CtxInspect", Json::object(), ctx);
        CHECK(d.kind == Decision::Kind::Allow);
    }

    TEST_CASE("Plan: prompt_fn is NOT called for write tools (Deny is early-exit)") {
        bool prompt_called = false;
        auto prompt = [&](std::string_view, const Json&) {
            prompt_called = true;
            return Decision::allow();
        };
        auto gate = make_gate(PermissionMode::Default, prompt);
        auto ctx = make_ctx(PermissionMode::Plan);

        gate.ask("Bash", bash_args("echo hi"), ctx);
        CHECK(!prompt_called);
    }
}

// ===========================================================================
// SUITE 5: AcceptEdits mode (AC3)
// ===========================================================================
TEST_SUITE("PermissionGate — AcceptEdits mode") {

    TEST_CASE("[AC3] AcceptEdits + Edit → Allow (no prompt)") {
        bool prompt_called = false;
        auto prompt = [&](std::string_view, const Json&) {
            prompt_called = true;
            return Decision::allow();
        };
        auto gate = make_gate(PermissionMode::Default, prompt);
        auto ctx = make_ctx(PermissionMode::AcceptEdits);

        auto d = gate.ask("Edit", write_args("./main.cpp"), ctx);
        CHECK(d.kind == Decision::Kind::Allow);
        CHECK(!prompt_called);
    }

    TEST_CASE("AcceptEdits + Write → Allow") {
        bool prompt_called = false;
        auto prompt = [&](std::string_view, const Json&) {
            prompt_called = true;
            return Decision::allow();
        };
        auto gate = make_gate(PermissionMode::Default, prompt);
        auto ctx = make_ctx(PermissionMode::AcceptEdits);

        auto d = gate.ask("Write", write_args("./output.txt"), ctx);
        CHECK(d.kind == Decision::Kind::Allow);
        CHECK(!prompt_called);
    }

    TEST_CASE("AcceptEdits + MultiEdit → Allow") {
        bool prompt_called = false;
        auto prompt = [&](std::string_view, const Json&) {
            prompt_called = true;
            return Decision::allow();
        };
        auto gate = make_gate(PermissionMode::Default, prompt);
        auto ctx = make_ctx(PermissionMode::AcceptEdits);

        auto d = gate.ask("MultiEdit", Json{{"file_path", "./main.cpp"}}, ctx);
        CHECK(d.kind == Decision::Kind::Allow);
        CHECK(!prompt_called);
    }

    TEST_CASE("AcceptEdits + Bash → falls through to prompt (not auto-approved)") {
        bool prompt_called = false;
        auto prompt = [&](std::string_view, const Json&) {
            prompt_called = true;
            return Decision::allow();
        };
        auto gate = make_gate(PermissionMode::Default, prompt);
        auto ctx = make_ctx(PermissionMode::AcceptEdits);

        auto d = gate.ask("Bash", bash_args("npm install"), ctx);
        CHECK(prompt_called);
        CHECK(d.kind == Decision::Kind::Allow);  // prompt returned Allow
    }

    TEST_CASE("AcceptEdits + Read → falls through (mock allow → Allow)") {
        auto gate = make_gate(PermissionMode::Default, mock_allow);
        auto ctx = make_ctx(PermissionMode::AcceptEdits);

        auto d = gate.ask("Read", read_args("./README.md"), ctx);
        CHECK(d.kind == Decision::Kind::Allow);
    }
}

// ===========================================================================
// SUITE 6: Deny rules (AC4)
// ===========================================================================
TEST_SUITE("PermissionGate — deny rules") {

    TEST_CASE("[AC4] Default mode + matching deny rule → Deny") {
        const fs::path p = make_temp_settings();
        auto store = std::make_shared<PermissionStore>(p);
        // Use ** to match paths containing slashes (e.g. /tmp/test)
        REQUIRE(store->add_deny_rule("Bash(rm -rf **)").has_value());

        bool prompt_called = false;
        auto prompt = [&](std::string_view, const Json&) {
            prompt_called = true;
            return Decision::allow();
        };

        PermissionGate gate(store, PermissionMode::Default, prompt);
        auto ctx = make_ctx(PermissionMode::Default);

        auto d = gate.ask("Bash", bash_args("rm -rf /tmp/test"), ctx);
        CHECK(d.kind == Decision::Kind::Deny);
        CHECK(!prompt_called);  // Deny rule short-circuits before prompt

        fs::remove_all(p.parent_path());
    }

    TEST_CASE("Deny rule with Read pattern → Deny for matching Read call") {
        const fs::path p = make_temp_settings();
        auto store = std::make_shared<PermissionStore>(p);
        REQUIRE(store->add_deny_rule("Read(./secrets/**)").has_value());

        PermissionGate gate(store, PermissionMode::Default, mock_allow);
        auto ctx = make_ctx(PermissionMode::Default);

        auto d = gate.ask("Read", read_args("./secrets/api_key.txt"), ctx);
        CHECK(d.kind == Decision::Kind::Deny);

        fs::remove_all(p.parent_path());
    }

    TEST_CASE("Non-matching deny rule → falls through to allow rule → Allow") {
        const fs::path p = make_temp_settings();
        auto store = std::make_shared<PermissionStore>(p);
        REQUIRE(store->add_deny_rule("Bash(rm -rf *)").has_value());
        REQUIRE(store->add_allow_rule("Bash(git *)").has_value());

        PermissionGate gate(store, PermissionMode::Default, mock_deny);
        auto ctx = make_ctx(PermissionMode::Default);

        // "git status" does NOT match "rm -rf *" deny rule → passes to allow rules
        auto d = gate.ask("Bash", bash_args("git status"), ctx);
        CHECK(d.kind == Decision::Kind::Allow);

        fs::remove_all(p.parent_path());
    }

    TEST_CASE("Deny rules checked before allow rules (priority)") {
        const fs::path p = make_temp_settings();
        auto store = std::make_shared<PermissionStore>(p);
        // Add BOTH allow and deny for the same pattern — deny must win
        REQUIRE(store->add_allow_rule("Bash(git *)").has_value());
        REQUIRE(store->add_deny_rule("Bash(git *)").has_value());

        PermissionGate gate(store, PermissionMode::Default, mock_allow);
        auto ctx = make_ctx(PermissionMode::Default);

        auto d = gate.ask("Bash", bash_args("git push --force"), ctx);
        CHECK(d.kind == Decision::Kind::Deny);

        fs::remove_all(p.parent_path());
    }
}

// ===========================================================================
// SUITE 7: Allow rules (AC5)
// ===========================================================================
TEST_SUITE("PermissionGate — allow rules") {

    TEST_CASE("[AC5] Default mode + matching allow rule → Allow") {
        const fs::path p = make_temp_settings();
        auto store = std::make_shared<PermissionStore>(p);
        REQUIRE(store->add_allow_rule("Bash(git *)").has_value());

        bool prompt_called = false;
        auto prompt = [&](std::string_view, const Json&) {
            prompt_called = true;
            return Decision::deny();
        };

        PermissionGate gate(store, PermissionMode::Default, prompt);
        auto ctx = make_ctx(PermissionMode::Default);

        auto d = gate.ask("Bash", bash_args("git status"), ctx);
        CHECK(d.kind == Decision::Kind::Allow);
        CHECK(!prompt_called);  // Allow rule short-circuits before prompt

        fs::remove_all(p.parent_path());
    }

    TEST_CASE("Allow rule: Read pattern matches exact path") {
        const fs::path p = make_temp_settings();
        auto store = std::make_shared<PermissionStore>(p);
        REQUIRE(store->add_allow_rule("Read(./src/**)").has_value());

        PermissionGate gate(store, PermissionMode::Default, mock_deny);
        auto ctx = make_ctx(PermissionMode::Default);

        auto d = gate.ask("Read", read_args("./src/permissions/PermissionGate.cpp"), ctx);
        CHECK(d.kind == Decision::Kind::Allow);

        fs::remove_all(p.parent_path());
    }

    TEST_CASE("Non-matching allow rule → falls through to prompt") {
        const fs::path p = make_temp_settings();
        auto store = std::make_shared<PermissionStore>(p);
        REQUIRE(store->add_allow_rule("Read(./src/**)").has_value());

        bool prompt_called = false;
        auto prompt = [&](std::string_view, const Json&) {
            prompt_called = true;
            return Decision::deny();
        };

        PermissionGate gate(store, PermissionMode::Default, prompt);
        auto ctx = make_ctx(PermissionMode::Default);

        // ./tests/** does NOT match ./src/**
        auto d = gate.ask("Read", read_args("./tests/unit/test_foo.cpp"), ctx);
        CHECK(prompt_called);
        CHECK(d.kind == Decision::Kind::Deny);

        fs::remove_all(p.parent_path());
    }
}

// ===========================================================================
// SUITE 8: Prompt callback (AC6, AC7)
// ===========================================================================
TEST_SUITE("PermissionGate — prompt callback") {

    TEST_CASE("[AC6] Default mode + no rule → calls prompt_user_ callback") {
        bool prompt_called = false;
        std::string captured_tool;
        auto prompt = [&](std::string_view tn, const Json&) {
            prompt_called = true;
            captured_tool = tn;
            return Decision::allow();
        };
        auto gate = make_gate(PermissionMode::Default, prompt);
        auto ctx = make_ctx(PermissionMode::Default);

        auto d = gate.ask("Bash", bash_args("npm install"), ctx);
        CHECK(prompt_called);
        CHECK(captured_tool == "Bash");
        CHECK(d.kind == Decision::Kind::Allow);
    }

    TEST_CASE("[AC7] Mock prompt returns Deny → gate returns Deny") {
        auto gate = make_gate(PermissionMode::Default, mock_deny);
        auto ctx = make_ctx(PermissionMode::Default);

        auto d = gate.ask("Bash", bash_args("make clean"), ctx);
        CHECK(d.kind == Decision::Kind::Deny);
    }

    TEST_CASE("Null prompt_fn → default-deny (safe fallback)") {
        const fs::path p = make_temp_settings();
        auto store = std::make_shared<PermissionStore>(p);
        PermissionGate gate(store, PermissionMode::Default, nullptr);
        auto ctx = make_ctx(PermissionMode::Default);

        auto d = gate.ask("Bash", bash_args("ls"), ctx);
        CHECK(d.kind == Decision::Kind::Deny);

        fs::remove_all(p.parent_path());
    }

    TEST_CASE("Prompt receives tool_name and args") {
        std::string got_tool;
        Json got_args;
        auto prompt = [&](std::string_view tn, const Json& a) {
            got_tool = tn;
            got_args = a;
            return Decision::allow();
        };
        auto gate = make_gate(PermissionMode::Default, prompt);
        auto ctx = make_ctx(PermissionMode::Default);

        const Json expected_args = bash_args("echo hello");
        gate.ask("Bash", expected_args, ctx);

        CHECK(got_tool == "Bash");
        CHECK(got_args == expected_args);
    }
}

// ===========================================================================
// SUITE 9: Persist-rule (always-allow / always-deny)
// ===========================================================================
TEST_SUITE("PermissionGate — persist_rule") {

    TEST_CASE("Callback returns allow_with_rule → rule persisted to store") {
        const fs::path p = make_temp_settings();
        auto store = std::make_shared<PermissionStore>(p);

        auto prompt = [](std::string_view tool_name, const Json&) {
            std::string pattern = std::string(tool_name) + "(*)";
            return Decision::allow_with_rule(pattern);
        };

        PermissionGate gate(store, PermissionMode::Default, prompt);
        auto ctx = make_ctx(PermissionMode::Default);

        auto d = gate.ask("Bash", bash_args("npm test"), ctx);
        CHECK(d.kind == Decision::Kind::Allow);

        // The store should now contain the persisted allow rule
        const auto& allow = store->allow_rules();
        bool found = false;
        for (const auto& r : allow) {
            if (r == "Bash(*)") { found = true; break; }
        }
        CHECK(found);

        fs::remove_all(p.parent_path());
    }

    TEST_CASE("Callback returns deny_with_rule → rule persisted to store") {
        const fs::path p = make_temp_settings();
        auto store = std::make_shared<PermissionStore>(p);

        auto prompt = [](std::string_view, const Json&) {
            return Decision::deny_with_rule("Bash(rm -rf *)");
        };

        PermissionGate gate(store, PermissionMode::Default, prompt);
        auto ctx = make_ctx(PermissionMode::Default);

        auto d = gate.ask("Bash", bash_args("rm -rf /tmp/old"), ctx);
        CHECK(d.kind == Decision::Kind::Deny);

        const auto& deny = store->deny_rules();
        bool found = false;
        for (const auto& r : deny) {
            if (r == "Bash(rm -rf *)") { found = true; break; }
        }
        CHECK(found);

        fs::remove_all(p.parent_path());
    }

    TEST_CASE("Persisted rule is honoured on next ask() call") {
        const fs::path p = make_temp_settings();
        auto store = std::make_shared<PermissionStore>(p);

        // First call: prompt returns allow_with_rule
        int prompt_count = 0;
        auto prompt = [&](std::string_view, const Json&) -> Decision {
            ++prompt_count;
            return Decision::allow_with_rule("Bash(git *)");
        };

        PermissionGate gate(store, PermissionMode::Default, prompt);
        auto ctx = make_ctx(PermissionMode::Default);

        // First ask: no rule exists → prompt called → rule persisted
        auto d1 = gate.ask("Bash", bash_args("git status"), ctx);
        CHECK(d1.kind == Decision::Kind::Allow);
        CHECK(prompt_count == 1);

        // Second ask: rule now exists → should Allow without calling prompt
        auto d2 = gate.ask("Bash", bash_args("git log"), ctx);
        CHECK(d2.kind == Decision::Kind::Allow);
        // prompt_count should still be 1 (rule matched, no second prompt)
        CHECK(prompt_count == 1);

        fs::remove_all(p.parent_path());
    }
}

// ===========================================================================
// SUITE 10: BATBOX_AUTO_APPROVE_READS (AC8)
// ===========================================================================
TEST_SUITE("PermissionGate — BATBOX_AUTO_APPROVE_READS") {

    TEST_CASE("[AC8] BATBOX_AUTO_APPROVE_READS=true: read-only tools pass without prompt") {
        // This test sets and unsets the env var.
        ::setenv("BATBOX_AUTO_APPROVE_READS", "1", 1);

        bool prompt_called = false;
        auto prompt = [&](std::string_view, const Json&) {
            prompt_called = true;
            return Decision::deny();
        };
        auto gate = make_gate(PermissionMode::Default, prompt);
        auto ctx = make_ctx(PermissionMode::Default);

        // Read is a read-only tool — should be auto-allowed
        auto d = gate.ask("Read", read_args("./README.md"), ctx);
        CHECK(d.kind == Decision::Kind::Allow);
        CHECK(!prompt_called);

        ::unsetenv("BATBOX_AUTO_APPROVE_READS");
    }

    TEST_CASE("BATBOX_AUTO_APPROVE_READS=true: write tools still go to prompt") {
        ::setenv("BATBOX_AUTO_APPROVE_READS", "1", 1);

        bool prompt_called = false;
        auto prompt = [&](std::string_view, const Json&) {
            prompt_called = true;
            return Decision::allow();
        };
        auto gate = make_gate(PermissionMode::Default, prompt);
        auto ctx = make_ctx(PermissionMode::Default);

        // Bash is NOT read-only — env var doesn't help it
        gate.ask("Bash", bash_args("echo test"), ctx);
        CHECK(prompt_called);

        ::unsetenv("BATBOX_AUTO_APPROVE_READS");
    }

    TEST_CASE("BATBOX_AUTO_APPROVE_READS unset: read tools fall through to prompt") {
        // Ensure env var is not set
        ::unsetenv("BATBOX_AUTO_APPROVE_READS");

        bool prompt_called = false;
        auto prompt = [&](std::string_view, const Json&) {
            prompt_called = true;
            return Decision::allow();
        };
        auto gate = make_gate(PermissionMode::Default, prompt);
        auto ctx = make_ctx(PermissionMode::Default);

        gate.ask("Read", read_args("./README.md"), ctx);
        CHECK(prompt_called);
    }
}

// ===========================================================================
// SUITE 11: Mode combinations × rule combinations
// ===========================================================================
TEST_SUITE("PermissionGate — mode × rule matrix") {

    TEST_CASE("Nuclear + deny rule: Nuclear short-circuits, returns Allow") {
        const fs::path p = make_temp_settings();
        auto store = std::make_shared<PermissionStore>(p);
        REQUIRE(store->add_deny_rule("Bash(*)").has_value());

        PermissionGate gate(store, PermissionMode::Default, mock_deny);
        auto ctx = make_ctx(PermissionMode::Nuclear);

        auto d = gate.ask("Bash", bash_args("anything"), ctx);
        CHECK(d.kind == Decision::Kind::Allow);

        fs::remove_all(p.parent_path());
    }

    TEST_CASE("Plan + allow rule for write tool: Plan still Denies (mode > rule)") {
        // Plan mode blocks all non-read-only tools regardless of allow rules.
        const fs::path p = make_temp_settings();
        auto store = std::make_shared<PermissionStore>(p);
        REQUIRE(store->add_allow_rule("Write(*)").has_value());

        PermissionGate gate(store, PermissionMode::Default, mock_allow);
        auto ctx = make_ctx(PermissionMode::Plan);

        auto d = gate.ask("Write", write_args("./output.txt"), ctx);
        CHECK(d.kind == Decision::Kind::Deny);

        fs::remove_all(p.parent_path());
    }

    TEST_CASE("AcceptEdits + deny rule for Edit: AcceptEdits still Allows (mode > rule)") {
        // AcceptEdits auto-allows Edit/Write/MultiEdit regardless of deny rules.
        const fs::path p = make_temp_settings();
        auto store = std::make_shared<PermissionStore>(p);
        REQUIRE(store->add_deny_rule("Edit(*)").has_value());

        bool prompt_called = false;
        auto prompt = [&](std::string_view, const Json&) {
            prompt_called = true;
            return Decision::deny();
        };

        PermissionGate gate(store, PermissionMode::Default, prompt);
        auto ctx = make_ctx(PermissionMode::AcceptEdits);

        auto d = gate.ask("Edit", write_args("./main.cpp"), ctx);
        CHECK(d.kind == Decision::Kind::Allow);
        CHECK(!prompt_called);

        fs::remove_all(p.parent_path());
    }

    TEST_CASE("Default + multiple allow rules: first match wins") {
        const fs::path p = make_temp_settings();
        auto store = std::make_shared<PermissionStore>(p);
        REQUIRE(store->add_allow_rule("Bash(git *)").has_value());
        REQUIRE(store->add_allow_rule("Bash(npm test*)").has_value());

        int prompt_count = 0;
        auto prompt = [&](std::string_view, const Json&) {
            ++prompt_count;
            return Decision::deny();
        };

        PermissionGate gate(store, PermissionMode::Default, prompt);
        auto ctx = make_ctx(PermissionMode::Default);

        auto d1 = gate.ask("Bash", bash_args("git log --oneline"), ctx);
        CHECK(d1.kind == Decision::Kind::Allow);

        auto d2 = gate.ask("Bash", bash_args("npm test -- --watch"), ctx);
        CHECK(d2.kind == Decision::Kind::Allow);

        CHECK(prompt_count == 0);

        fs::remove_all(p.parent_path());
    }

    TEST_CASE("Default + no rules + no prompt → Deny (null callback)") {
        const fs::path p = make_temp_settings();
        auto store = std::make_shared<PermissionStore>(p);
        PermissionGate gate(store, PermissionMode::Default, nullptr);
        auto ctx = make_ctx(PermissionMode::Default);

        auto d = gate.ask("Bash", bash_args("echo hi"), ctx);
        CHECK(d.kind == Decision::Kind::Deny);

        fs::remove_all(p.parent_path());
    }
}

// ===========================================================================
// SUITE 12: store() accessor
// ===========================================================================
TEST_SUITE("PermissionGate — store accessor") {

    TEST_CASE("store() returns the underlying PermissionStore") {
        const fs::path p = make_temp_settings();
        auto store = std::make_shared<PermissionStore>(p);
        REQUIRE(store->add_allow_rule("Read(**)").has_value());

        PermissionGate gate(store, PermissionMode::Default, mock_allow);

        // Access through gate should show the same rules
        CHECK(gate.store().allow_rules().size() == 1);
        CHECK(gate.store().allow_rules()[0] == "Read(**)");

        fs::remove_all(p.parent_path());
    }
}
