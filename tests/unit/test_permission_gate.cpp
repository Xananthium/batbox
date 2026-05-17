// tests/unit/test_permission_gate.cpp
// ---------------------------------------------------------------------------
// doctest unit tests for batbox::permissions::PermissionGate — CPP 12.5.
//
// Covers all acceptance criteria:
//   [1] 4 modes × 5 tools = 20 test cases, all passing
//       Tools: Read, Write, Edit, Bash, WebFetch
//       Modes: Default, Plan, AcceptEdits, Nuclear
//   [2] Mode cycling test verifies state transitions
//   [3] Nuclear mode confirmation modal pathway tested with mock callback
//   [4] allow-once vs allow-always rule precedence
//   [5] Mode switching mid-session preserves session-scoped rules
//   [6] Rule pattern matching (glob, exact, prefix)
//
// Build (standalone, no CMake):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_permission_gate.cpp \
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
//       -o /tmp/test_pg_unit && /tmp/test_pg_unit
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
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace batbox::permissions;
using batbox::Json;
using batbox::tools::ToolContext;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static fs::path make_temp_settings() {
    static std::atomic<int> counter{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const int seq  = ++counter;
    const std::string dir = "test_pg12_5_" + std::to_string(now) + "_" + std::to_string(seq);
    const fs::path tmp = fs::temp_directory_path() / dir;
    fs::create_directories(tmp);
    return tmp / "settings.json";
}

static ToolContext make_ctx(PermissionMode mode = PermissionMode::Default) {
    ToolContext ctx;
    ctx.cwd        = fs::temp_directory_path();
    ctx.mode       = mode;
    ctx.session_id = "unit-test-session";
    ctx.agent_id   = "";
    return ctx;
}

static Decision mock_allow(std::string_view, const Json&) { return Decision::allow(); }
static Decision mock_deny(std::string_view, const Json&)  { return Decision::deny();  }

/// Convenience: build gate with fresh temp store, given mode and callback.
/// If out_path is non-null the settings path is written there so the caller
/// can load a second store to verify persistence.
static PermissionGate make_gate(
    PermissionMode           mode,
    PermissionGate::PromptFn callback,
    fs::path*                out_path    = nullptr,
    std::shared_ptr<PermissionStore>* out_store = nullptr)
{
    const fs::path p = make_temp_settings();
    if (out_path) *out_path = p;
    auto store = std::make_shared<PermissionStore>(p);
    if (out_store) *out_store = store;
    return PermissionGate(store, mode, std::move(callback));
}

/// Convenience: build gate with a pre-constructed shared store.
static PermissionGate make_gate_with_store(
    std::shared_ptr<PermissionStore> store,
    PermissionMode                   mode,
    PermissionGate::PromptFn         callback)
{
    return PermissionGate(std::move(store), mode, std::move(callback));
}

// JSON helpers for each of the 5 representative tool argument shapes.
static Json read_args(const std::string& path)     { return {{"file_path", path}}; }
static Json write_args(const std::string& path)    { return {{"file_path", path}}; }
static Json edit_args(const std::string& path)     { return {{"file_path", path}}; }
static Json bash_args(const std::string& cmd)      { return {{"command", cmd}};    }
static Json webfetch_args(const std::string& url)  { return {{"url", url}};        }

// ===========================================================================
// SUITE 1 — Default mode × 5 tools
//   Default mode has no auto-allow/deny: all decisions route to the callback.
// ===========================================================================
TEST_SUITE("CPP 12.5 — Default mode × 5 tools") {

    // -------- Read -----------------------------------------------------------
    TEST_CASE("Default + Read: routes to callback (allow)") {
        bool called = false;
        auto gate = make_gate(PermissionMode::Default, [&](std::string_view, const Json&) {
            called = true;
            return Decision::allow();
        });
        auto ctx = make_ctx(PermissionMode::Default);
        auto d = gate.ask("Read", read_args("./src/main.cpp"), ctx);
        CHECK(d.kind == Decision::Kind::Allow);
        CHECK(called);
    }

    TEST_CASE("Default + Read: routes to callback (deny)") {
        auto gate = make_gate(PermissionMode::Default, mock_deny);
        auto ctx  = make_ctx(PermissionMode::Default);
        auto d    = gate.ask("Read", read_args("./src/main.cpp"), ctx);
        CHECK(d.kind == Decision::Kind::Deny);
    }

    // -------- Write ----------------------------------------------------------
    TEST_CASE("Default + Write: routes to callback (allow)") {
        auto gate = make_gate(PermissionMode::Default, mock_allow);
        auto ctx  = make_ctx(PermissionMode::Default);
        auto d    = gate.ask("Write", write_args("./output.txt"), ctx);
        CHECK(d.kind == Decision::Kind::Allow);
    }

    TEST_CASE("Default + Write: routes to callback (deny)") {
        auto gate = make_gate(PermissionMode::Default, mock_deny);
        auto ctx  = make_ctx(PermissionMode::Default);
        auto d    = gate.ask("Write", write_args("./output.txt"), ctx);
        CHECK(d.kind == Decision::Kind::Deny);
    }

    // -------- Edit -----------------------------------------------------------
    TEST_CASE("Default + Edit: routes to callback (allow)") {
        auto gate = make_gate(PermissionMode::Default, mock_allow);
        auto ctx  = make_ctx(PermissionMode::Default);
        auto d    = gate.ask("Edit", edit_args("./README.md"), ctx);
        CHECK(d.kind == Decision::Kind::Allow);
    }

    TEST_CASE("Default + Edit: routes to callback (deny)") {
        auto gate = make_gate(PermissionMode::Default, mock_deny);
        auto ctx  = make_ctx(PermissionMode::Default);
        auto d    = gate.ask("Edit", edit_args("./README.md"), ctx);
        CHECK(d.kind == Decision::Kind::Deny);
    }

    // -------- Bash -----------------------------------------------------------
    TEST_CASE("Default + Bash: routes to callback (allow)") {
        auto gate = make_gate(PermissionMode::Default, mock_allow);
        auto ctx  = make_ctx(PermissionMode::Default);
        auto d    = gate.ask("Bash", bash_args("npm install"), ctx);
        CHECK(d.kind == Decision::Kind::Allow);
    }

    TEST_CASE("Default + Bash: routes to callback (deny)") {
        auto gate = make_gate(PermissionMode::Default, mock_deny);
        auto ctx  = make_ctx(PermissionMode::Default);
        auto d    = gate.ask("Bash", bash_args("npm install"), ctx);
        CHECK(d.kind == Decision::Kind::Deny);
    }

    // -------- WebFetch -------------------------------------------------------
    TEST_CASE("Default + WebFetch: routes to callback (allow)") {
        auto gate = make_gate(PermissionMode::Default, mock_allow);
        auto ctx  = make_ctx(PermissionMode::Default);
        auto d    = gate.ask("WebFetch", webfetch_args("https://example.com/api"), ctx);
        CHECK(d.kind == Decision::Kind::Allow);
    }

    TEST_CASE("Default + WebFetch: routes to callback (deny)") {
        auto gate = make_gate(PermissionMode::Default, mock_deny);
        auto ctx  = make_ctx(PermissionMode::Default);
        auto d    = gate.ask("WebFetch", webfetch_args("https://example.com/api"), ctx);
        CHECK(d.kind == Decision::Kind::Deny);
    }
}

// ===========================================================================
// SUITE 2 — Plan mode × 5 tools
//   Read (read-only) → passes through to callback (Allow in these tests).
//   Write / Edit / Bash / WebFetch → Deny without calling callback.
// ===========================================================================
TEST_SUITE("CPP 12.5 — Plan mode × 5 tools") {

    // -------- Read -----------------------------------------------------------
    TEST_CASE("Plan + Read: passes through to callback (read-only tool)") {
        bool called = false;
        auto gate = make_gate(PermissionMode::Default, [&](std::string_view, const Json&) {
            called = true;
            return Decision::allow();
        });
        auto ctx = make_ctx(PermissionMode::Plan);
        auto d   = gate.ask("Read", read_args("./include/foo.hpp"), ctx);
        CHECK(d.kind == Decision::Kind::Allow);
        CHECK(called);  // read-only: passes through to callback
    }

    // -------- Write ----------------------------------------------------------
    TEST_CASE("Plan + Write: Deny without calling callback") {
        bool called = false;
        auto gate = make_gate(PermissionMode::Default, [&](std::string_view, const Json&) {
            called = true;
            return Decision::allow();
        });
        auto ctx = make_ctx(PermissionMode::Plan);
        auto d   = gate.ask("Write", write_args("./output.txt"), ctx);
        CHECK(d.kind == Decision::Kind::Deny);
        CHECK(!called);
    }

    // -------- Edit -----------------------------------------------------------
    TEST_CASE("Plan + Edit: Deny without calling callback") {
        bool called = false;
        auto gate = make_gate(PermissionMode::Default, [&](std::string_view, const Json&) {
            called = true;
            return Decision::allow();
        });
        auto ctx = make_ctx(PermissionMode::Plan);
        auto d   = gate.ask("Edit", edit_args("./src/Foo.cpp"), ctx);
        CHECK(d.kind == Decision::Kind::Deny);
        CHECK(!called);
    }

    // -------- Bash -----------------------------------------------------------
    TEST_CASE("Plan + Bash: Deny without calling callback") {
        bool called = false;
        auto gate = make_gate(PermissionMode::Default, [&](std::string_view, const Json&) {
            called = true;
            return Decision::allow();
        });
        auto ctx = make_ctx(PermissionMode::Plan);
        auto d   = gate.ask("Bash", bash_args("make build"), ctx);
        CHECK(d.kind == Decision::Kind::Deny);
        CHECK(!called);
    }

    // -------- WebFetch -------------------------------------------------------
    TEST_CASE("Plan + WebFetch: Deny without calling callback (not read-only)") {
        bool called = false;
        auto gate = make_gate(PermissionMode::Default, [&](std::string_view, const Json&) {
            called = true;
            return Decision::allow();
        });
        auto ctx = make_ctx(PermissionMode::Plan);
        auto d   = gate.ask("WebFetch", webfetch_args("https://api.example.com"), ctx);
        CHECK(d.kind == Decision::Kind::Deny);
        CHECK(!called);
    }
}

// ===========================================================================
// SUITE 3 — AcceptEdits mode × 5 tools
//   Edit / Write / MultiEdit → Allow without callback.
//   Read / Bash / WebFetch → pass through to callback.
// ===========================================================================
TEST_SUITE("CPP 12.5 — AcceptEdits mode × 5 tools") {

    // -------- Read -----------------------------------------------------------
    TEST_CASE("AcceptEdits + Read: falls through to callback") {
        bool called = false;
        auto gate = make_gate(PermissionMode::Default, [&](std::string_view, const Json&) {
            called = true;
            return Decision::allow();
        });
        auto ctx = make_ctx(PermissionMode::AcceptEdits);
        auto d   = gate.ask("Read", read_args("./src/Foo.hpp"), ctx);
        CHECK(d.kind == Decision::Kind::Allow);
        CHECK(called);  // not in the auto-approve set: goes to callback
    }

    // -------- Write ----------------------------------------------------------
    TEST_CASE("AcceptEdits + Write: Allow without callback") {
        bool called = false;
        auto gate = make_gate(PermissionMode::Default, [&](std::string_view, const Json&) {
            called = true;
            return Decision::deny();  // callback would deny — but should not be reached
        });
        auto ctx = make_ctx(PermissionMode::AcceptEdits);
        auto d   = gate.ask("Write", write_args("./output/result.txt"), ctx);
        CHECK(d.kind == Decision::Kind::Allow);
        CHECK(!called);
    }

    // -------- Edit -----------------------------------------------------------
    TEST_CASE("AcceptEdits + Edit: Allow without callback") {
        bool called = false;
        auto gate = make_gate(PermissionMode::Default, [&](std::string_view, const Json&) {
            called = true;
            return Decision::deny();
        });
        auto ctx = make_ctx(PermissionMode::AcceptEdits);
        auto d   = gate.ask("Edit", edit_args("./src/main.cpp"), ctx);
        CHECK(d.kind == Decision::Kind::Allow);
        CHECK(!called);
    }

    // -------- Bash -----------------------------------------------------------
    TEST_CASE("AcceptEdits + Bash: falls through to callback (Bash not auto-approved)") {
        bool called = false;
        auto gate = make_gate(PermissionMode::Default, [&](std::string_view, const Json&) {
            called = true;
            return Decision::allow();
        });
        auto ctx = make_ctx(PermissionMode::AcceptEdits);
        auto d   = gate.ask("Bash", bash_args("cargo build"), ctx);
        CHECK(d.kind == Decision::Kind::Allow);
        CHECK(called);
    }

    // -------- WebFetch -------------------------------------------------------
    TEST_CASE("AcceptEdits + WebFetch: falls through to callback") {
        bool called = false;
        auto gate = make_gate(PermissionMode::Default, [&](std::string_view, const Json&) {
            called = true;
            return Decision::allow();
        });
        auto ctx = make_ctx(PermissionMode::AcceptEdits);
        auto d   = gate.ask("WebFetch", webfetch_args("https://crates.io/api/v1/crates"), ctx);
        CHECK(d.kind == Decision::Kind::Allow);
        CHECK(called);
    }
}

// ===========================================================================
// SUITE 4 — Nuclear mode × 5 tools
//   Nuclear short-circuits everything: always Allow, callback never called,
//   rules never consulted.
// ===========================================================================
TEST_SUITE("CPP 12.5 — Nuclear mode × 5 tools") {

    // Shared setup: a store with deny rules for all tool patterns + a callback
    // that fails the test if invoked.  Nuclear must bypass both.

    // -------- Read -----------------------------------------------------------
    TEST_CASE("Nuclear + Read: Allow without consulting rules or callback") {
        fs::path p = make_temp_settings();
        auto store = std::make_shared<PermissionStore>(p);
        REQUIRE(store->add_deny_rule("Read(**)").has_value());

        bool called = false;
        PermissionGate gate(store, PermissionMode::Default, [&](std::string_view, const Json&) {
            called = true;
            return Decision::deny();
        });
        auto ctx = make_ctx(PermissionMode::Nuclear);
        auto d   = gate.ask("Read", read_args("./secret_key.txt"), ctx);
        CHECK(d.kind == Decision::Kind::Allow);
        CHECK(!called);
        fs::remove_all(p.parent_path());
    }

    // -------- Write ----------------------------------------------------------
    TEST_CASE("Nuclear + Write: Allow without consulting rules or callback") {
        fs::path p = make_temp_settings();
        auto store = std::make_shared<PermissionStore>(p);
        REQUIRE(store->add_deny_rule("Write(**)").has_value());

        bool called = false;
        PermissionGate gate(store, PermissionMode::Default, [&](std::string_view, const Json&) {
            called = true;
            return Decision::deny();
        });
        auto ctx = make_ctx(PermissionMode::Nuclear);
        auto d   = gate.ask("Write", write_args("/etc/hosts"), ctx);
        CHECK(d.kind == Decision::Kind::Allow);
        CHECK(!called);
        fs::remove_all(p.parent_path());
    }

    // -------- Edit -----------------------------------------------------------
    TEST_CASE("Nuclear + Edit: Allow without consulting rules or callback") {
        fs::path p = make_temp_settings();
        auto store = std::make_shared<PermissionStore>(p);
        REQUIRE(store->add_deny_rule("Edit(**)").has_value());

        bool called = false;
        PermissionGate gate(store, PermissionMode::Default, [&](std::string_view, const Json&) {
            called = true;
            return Decision::deny();
        });
        auto ctx = make_ctx(PermissionMode::Nuclear);
        auto d   = gate.ask("Edit", edit_args("./src/CriticalFile.cpp"), ctx);
        CHECK(d.kind == Decision::Kind::Allow);
        CHECK(!called);
        fs::remove_all(p.parent_path());
    }

    // -------- Bash -----------------------------------------------------------
    TEST_CASE("Nuclear + Bash: Allow without consulting rules or callback") {
        fs::path p = make_temp_settings();
        auto store = std::make_shared<PermissionStore>(p);
        REQUIRE(store->add_deny_rule("Bash(**)").has_value());

        bool called = false;
        PermissionGate gate(store, PermissionMode::Default, [&](std::string_view, const Json&) {
            called = true;
            return Decision::deny();
        });
        auto ctx = make_ctx(PermissionMode::Nuclear);
        auto d   = gate.ask("Bash", bash_args("rm -rf /"), ctx);
        CHECK(d.kind == Decision::Kind::Allow);
        CHECK(!called);
        fs::remove_all(p.parent_path());
    }

    // -------- WebFetch -------------------------------------------------------
    TEST_CASE("Nuclear + WebFetch: Allow without consulting rules or callback") {
        fs::path p = make_temp_settings();
        auto store = std::make_shared<PermissionStore>(p);
        REQUIRE(store->add_deny_rule("WebFetch(**)").has_value());

        bool called = false;
        PermissionGate gate(store, PermissionMode::Default, [&](std::string_view, const Json&) {
            called = true;
            return Decision::deny();
        });
        auto ctx = make_ctx(PermissionMode::Nuclear);
        auto d   = gate.ask("WebFetch", webfetch_args("https://malicious.example.com"), ctx);
        CHECK(d.kind == Decision::Kind::Allow);
        CHECK(!called);
        fs::remove_all(p.parent_path());
    }
}

// ===========================================================================
// SUITE 5 — Mode cycling (Shift+Tab simulation)
//   cycle_next: Default → Plan → AcceptEdits → Nuclear → Default
// ===========================================================================
TEST_SUITE("CPP 12.5 — Mode cycling") {

    TEST_CASE("cycle_next traverses all 4 modes in order") {
        CHECK(cycle_next(PermissionMode::Default)     == PermissionMode::Plan);
        CHECK(cycle_next(PermissionMode::Plan)        == PermissionMode::AcceptEdits);
        CHECK(cycle_next(PermissionMode::AcceptEdits) == PermissionMode::Nuclear);
        CHECK(cycle_next(PermissionMode::Nuclear)     == PermissionMode::Default);
    }

    TEST_CASE("Full cycle returns to starting mode") {
        PermissionMode m = PermissionMode::Default;
        for (int i = 0; i < 4; ++i) { m = cycle_next(m); }
        CHECK(m == PermissionMode::Default);
    }

    TEST_CASE("set_mode() simulates Shift+Tab: gate behaviour changes immediately") {
        // Start in Default → Bash goes to callback → switch to AcceptEdits
        // → Edit is now auto-allowed without callback → switch to Nuclear
        // → Bash is now auto-allowed without callback.
        fs::path p;
        std::shared_ptr<PermissionStore> store;
        int prompt_count = 0;
        auto gate = make_gate(PermissionMode::Default,
                              [&](std::string_view, const Json&) {
                                  ++prompt_count;
                                  return Decision::allow();
                              },
                              &p, &store);

        // Default: Bash → callback
        auto d1 = gate.ask("Bash", bash_args("echo a"), make_ctx(PermissionMode::Default));
        CHECK(d1.kind == Decision::Kind::Allow);
        CHECK(prompt_count == 1);

        // Switch to AcceptEdits: Edit → auto-allow, no callback
        gate.set_mode(PermissionMode::AcceptEdits);
        CHECK(gate.current_mode() == PermissionMode::AcceptEdits);
        auto d2 = gate.ask("Edit", edit_args("./foo.cpp"), make_ctx(PermissionMode::AcceptEdits));
        CHECK(d2.kind == Decision::Kind::Allow);
        CHECK(prompt_count == 1);  // callback not called again

        // Switch to Nuclear: Bash → auto-allow, no callback
        gate.set_mode(PermissionMode::Nuclear);
        CHECK(gate.current_mode() == PermissionMode::Nuclear);
        auto d3 = gate.ask("Bash", bash_args("rm -rf /tmp/x"), make_ctx(PermissionMode::Nuclear));
        CHECK(d3.kind == Decision::Kind::Allow);
        CHECK(prompt_count == 1);  // callback still not called

        // Switch back to Default: Bash → callback again
        gate.set_mode(PermissionMode::Default);
        CHECK(gate.current_mode() == PermissionMode::Default);
        auto d4 = gate.ask("Bash", bash_args("echo b"), make_ctx(PermissionMode::Default));
        CHECK(d4.kind == Decision::Kind::Allow);
        CHECK(prompt_count == 2);  // callback called again

        fs::remove_all(p.parent_path());
    }

    TEST_CASE("Mode switching mid-session preserves session-scoped rules") {
        // Add a session allow rule while in Default mode.
        // Switch to Plan mode (rules are still in the store).
        // Switch back to Default mode — the allow rule is still effective.
        fs::path p = make_temp_settings();
        auto store = std::make_shared<PermissionStore>(p);
        REQUIRE(store->add_allow_rule("Bash(git *)").has_value());

        int prompt_count = 0;
        PermissionGate gate(store, PermissionMode::Default,
                            [&](std::string_view, const Json&) {
                                ++prompt_count;
                                return Decision::deny();
                            });

        // Default: Bash(git status) matches allow rule → Allow, no prompt
        auto d1 = gate.ask("Bash", bash_args("git status"), make_ctx(PermissionMode::Default));
        CHECK(d1.kind == Decision::Kind::Allow);
        CHECK(prompt_count == 0);

        // Switch to Plan: Bash is blocked (write-like), but the rule is still stored
        gate.set_mode(PermissionMode::Plan);
        auto d2 = gate.ask("Bash", bash_args("git status"), make_ctx(PermissionMode::Plan));
        CHECK(d2.kind == Decision::Kind::Deny);   // Plan mode blocks non-read-only

        // Switch back to Default: allow rule is still present → Allow, no prompt
        gate.set_mode(PermissionMode::Default);
        auto d3 = gate.ask("Bash", bash_args("git log --oneline"), make_ctx(PermissionMode::Default));
        CHECK(d3.kind == Decision::Kind::Allow);
        CHECK(prompt_count == 0);  // no prompt: rule matched

        fs::remove_all(p.parent_path());
    }
}

// ===========================================================================
// SUITE 6 — Nuclear confirmation modal pathway (mock callback)
//   Nuclear bypasses everything.  But when using a mock callback that would
//   return Nuclear-level permissions, it should NOT be called in Nuclear mode.
//   This suite validates the "nuclear confirmation modal" pattern: the TUI
//   callback is set to return allow_with_rule, but in Nuclear mode the gate
//   should short-circuit before ever invoking it.
// ===========================================================================
TEST_SUITE("CPP 12.5 — Nuclear confirmation modal pathway") {

    TEST_CASE("Nuclear: TUI confirmation callback is never called for any tool") {
        int tui_calls = 0;
        // Simulate a TUI that would confirm Nuclear-level operations
        auto tui_callback = [&](std::string_view tn, const Json&) {
            ++tui_calls;
            // In Nuclear mode the TUI would return "always allow"
            return Decision::allow_with_rule(std::string(tn) + "(**)");
        };
        auto gate = make_gate(PermissionMode::Default, tui_callback);
        auto nuclear_ctx = make_ctx(PermissionMode::Nuclear);

        // Invoke all 5 representative tools in Nuclear mode
        (void)gate.ask("Read",     read_args("./src/**"),                 nuclear_ctx);
        (void)gate.ask("Write",    write_args("./out.txt"),               nuclear_ctx);
        (void)gate.ask("Edit",     edit_args("./CMakeLists.txt"),         nuclear_ctx);
        (void)gate.ask("Bash",     bash_args("sudo rm -rf /var/log/*"),   nuclear_ctx);
        (void)gate.ask("WebFetch", webfetch_args("https://secure.api/"), nuclear_ctx);

        // Nuclear mode must never invoke the TUI callback
        CHECK(tui_calls == 0);
    }

    TEST_CASE("Nuclear: bypasses deny rules AND allow rules AND callback") {
        fs::path p = make_temp_settings();
        auto store = std::make_shared<PermissionStore>(p);
        REQUIRE(store->add_deny_rule("Bash(**)").has_value());
        REQUIRE(store->add_deny_rule("Write(**)").has_value());

        bool called = false;
        PermissionGate gate(store, PermissionMode::Default, [&](std::string_view, const Json&) {
            called = true;
            return Decision::deny();
        });

        auto ctx = make_ctx(PermissionMode::Nuclear);
        CHECK(gate.ask("Bash",  bash_args("dangerous"),         ctx).kind == Decision::Kind::Allow);
        CHECK(gate.ask("Write", write_args("/etc/important"),   ctx).kind == Decision::Kind::Allow);
        CHECK(!called);

        fs::remove_all(p.parent_path());
    }

    TEST_CASE("Nuclear then Default: after switching back, rules and callback are active") {
        // Demonstrate that switching out of Nuclear re-enables the full pipeline.
        fs::path p = make_temp_settings();
        auto store = std::make_shared<PermissionStore>(p);
        REQUIRE(store->add_deny_rule("Bash(rm -rf **)").has_value());

        bool called = false;
        PermissionGate gate(store, PermissionMode::Default, [&](std::string_view, const Json&) {
            called = true;
            return Decision::deny();
        });

        // Nuclear: deny rule and callback both skipped → Allow
        auto d1 = gate.ask("Bash", bash_args("rm -rf /tmp/old"), make_ctx(PermissionMode::Nuclear));
        CHECK(d1.kind == Decision::Kind::Allow);
        CHECK(!called);

        // Switch to Default: deny rule kicks in → Deny without callback
        gate.set_mode(PermissionMode::Default);
        auto d2 = gate.ask("Bash", bash_args("rm -rf /tmp/old"), make_ctx(PermissionMode::Default));
        CHECK(d2.kind == Decision::Kind::Deny);
        CHECK(!called);  // deny rule short-circuits before callback

        fs::remove_all(p.parent_path());
    }
}

// ===========================================================================
// SUITE 7 — allow-once vs allow-always rule precedence
//   "allow-once" = callback returns Decision::allow()     (no persist_rule)
//   "allow-always" = callback returns Decision::allow_with_rule(...)
//   After allow-once, the next ask() hits the callback again.
//   After allow-always, the next ask() matches the persisted rule (no callback).
// ===========================================================================
TEST_SUITE("CPP 12.5 — allow-once vs allow-always") {

    TEST_CASE("allow-once: callback called again on second ask") {
        int call_count = 0;
        auto callback = [&](std::string_view, const Json&) -> Decision {
            ++call_count;
            return Decision::allow();  // one-shot: no persist_rule
        };
        auto gate = make_gate(PermissionMode::Default, callback);
        auto ctx  = make_ctx(PermissionMode::Default);

        // First ask: no rule, callback called
        auto d1 = gate.ask("Bash", bash_args("git status"), ctx);
        CHECK(d1.kind == Decision::Kind::Allow);
        CHECK(call_count == 1);

        // Second ask: still no persisted rule, callback called again
        auto d2 = gate.ask("Bash", bash_args("git log"), ctx);
        CHECK(d2.kind == Decision::Kind::Allow);
        CHECK(call_count == 2);  // called twice — no persistence
    }

    TEST_CASE("allow-always: after first ask, rule is persisted; second ask skips callback") {
        int call_count = 0;
        auto callback = [&](std::string_view, const Json&) -> Decision {
            ++call_count;
            // "Always allow" — persists a rule matching any Bash command
            return Decision::allow_with_rule("Bash(git *)");
        };
        fs::path p;
        auto gate = make_gate(PermissionMode::Default, callback, &p);
        auto ctx  = make_ctx(PermissionMode::Default);

        // First ask: callback fires, persists "Bash(git *)"
        auto d1 = gate.ask("Bash", bash_args("git status"), ctx);
        CHECK(d1.kind == Decision::Kind::Allow);
        CHECK(call_count == 1);

        // Second ask: "Bash(git log)" matches the persisted allow rule → no callback
        auto d2 = gate.ask("Bash", bash_args("git log --oneline"), ctx);
        CHECK(d2.kind == Decision::Kind::Allow);
        CHECK(call_count == 1);  // callback NOT called a second time

        fs::remove_all(p.parent_path());
    }

    TEST_CASE("deny-once: callback called again on second ask") {
        int call_count = 0;
        auto callback = [&](std::string_view, const Json&) -> Decision {
            ++call_count;
            return Decision::deny();  // one-shot deny, no persist_rule
        };
        auto gate = make_gate(PermissionMode::Default, callback);
        auto ctx  = make_ctx(PermissionMode::Default);

        auto d1 = gate.ask("Bash", bash_args("npm publish"), ctx);
        CHECK(d1.kind == Decision::Kind::Deny);
        CHECK(call_count == 1);

        auto d2 = gate.ask("Bash", bash_args("npm publish --access public"), ctx);
        CHECK(d2.kind == Decision::Kind::Deny);
        CHECK(call_count == 2);
    }

    TEST_CASE("deny-always: after first ask, deny rule persisted; second ask short-circuits") {
        int call_count = 0;
        auto callback = [&](std::string_view, const Json&) -> Decision {
            ++call_count;
            return Decision::deny_with_rule("Bash(npm publish*)");
        };
        fs::path p;
        auto gate = make_gate(PermissionMode::Default, callback, &p);
        auto ctx  = make_ctx(PermissionMode::Default);

        // First ask: callback fires, persists deny rule
        auto d1 = gate.ask("Bash", bash_args("npm publish"), ctx);
        CHECK(d1.kind == Decision::Kind::Deny);
        CHECK(call_count == 1);

        // Second ask: matches persisted deny rule → Deny without callback
        auto d2 = gate.ask("Bash", bash_args("npm publish --tag beta"), ctx);
        CHECK(d2.kind == Decision::Kind::Deny);
        CHECK(call_count == 1);

        fs::remove_all(p.parent_path());
    }

    TEST_CASE("allow-once for Bash does NOT persist a rule for Read") {
        // allow-once grants only the current invocation — does not expand to
        // other tool names or patterns.
        int call_count = 0;
        auto callback = [&](std::string_view, const Json&) -> Decision {
            ++call_count;
            return Decision::allow();  // one-shot, no persistence
        };
        auto gate = make_gate(PermissionMode::Default, callback);
        auto ctx  = make_ctx(PermissionMode::Default);

        // Allow Bash once
        (void)gate.ask("Bash", bash_args("git status"), ctx);
        CHECK(call_count == 1);

        // Read invocation should still hit callback (no rule persisted for Read)
        (void)gate.ask("Read", read_args("./src/foo.cpp"), ctx);
        CHECK(call_count == 2);
    }
}

// ===========================================================================
// SUITE 8 — Rule pattern matching: glob, exact, prefix
// ===========================================================================
TEST_SUITE("CPP 12.5 — Rule pattern matching") {

    // ---- Exact match --------------------------------------------------------
    TEST_CASE("Exact Bash command rule matches only that exact command") {
        fs::path p = make_temp_settings();
        auto store = std::make_shared<PermissionStore>(p);
        // Using a literal pattern that matches only "git status" exactly
        REQUIRE(store->add_allow_rule("Bash(git status)").has_value());

        PermissionGate gate(store, PermissionMode::Default, mock_deny);
        auto ctx = make_ctx(PermissionMode::Default);

        // Exact match → Allow
        auto d1 = gate.ask("Bash", bash_args("git status"), ctx);
        CHECK(d1.kind == Decision::Kind::Allow);

        // Different command → no match → falls to callback (mock_deny → Deny)
        auto d2 = gate.ask("Bash", bash_args("git status --short"), ctx);
        CHECK(d2.kind == Decision::Kind::Deny);

        fs::remove_all(p.parent_path());
    }

    // ---- Glob * (single component) ----------------------------------------
    TEST_CASE("Bash(git *) allows all git commands without / in the argument") {
        fs::path p = make_temp_settings();
        auto store = std::make_shared<PermissionStore>(p);
        REQUIRE(store->add_allow_rule("Bash(git *)").has_value());

        PermissionGate gate(store, PermissionMode::Default, mock_deny);
        auto ctx = make_ctx(PermissionMode::Default);

        CHECK(gate.ask("Bash", bash_args("git status"),       ctx).kind == Decision::Kind::Allow);
        CHECK(gate.ask("Bash", bash_args("git log"),          ctx).kind == Decision::Kind::Allow);
        CHECK(gate.ask("Bash", bash_args("git pull"),         ctx).kind == Decision::Kind::Allow);
        // Different prefix → Deny
        CHECK(gate.ask("Bash", bash_args("npm install"),      ctx).kind == Decision::Kind::Deny);

        fs::remove_all(p.parent_path());
    }

    // ---- Glob ** (path-recursive) -----------------------------------------
    TEST_CASE("Read(./src/**) allows any file under ./src/ recursively") {
        fs::path p = make_temp_settings();
        auto store = std::make_shared<PermissionStore>(p);
        REQUIRE(store->add_allow_rule("Read(./src/**)").has_value());

        PermissionGate gate(store, PermissionMode::Default, mock_deny);
        auto ctx = make_ctx(PermissionMode::Default);

        // Files under ./src/ (multi-level) → Allow
        CHECK(gate.ask("Read", read_args("./src/main.cpp"),                    ctx).kind == Decision::Kind::Allow);
        CHECK(gate.ask("Read", read_args("./src/permissions/PermissionGate.cpp"), ctx).kind == Decision::Kind::Allow);
        CHECK(gate.ask("Read", read_args("./src/core/utils/Helper.hpp"),       ctx).kind == Decision::Kind::Allow);

        // Outside ./src/ → Deny
        CHECK(gate.ask("Read", read_args("./include/batbox/core/Result.hpp"),  ctx).kind == Decision::Kind::Deny);
        CHECK(gate.ask("Read", read_args("./tests/unit/test_foo.cpp"),         ctx).kind == Decision::Kind::Deny);

        fs::remove_all(p.parent_path());
    }

    // ---- Prefix glob * (single level) ------------------------------------
    TEST_CASE("Write(./build/*) allows files directly under ./build/ only") {
        fs::path p = make_temp_settings();
        auto store = std::make_shared<PermissionStore>(p);
        REQUIRE(store->add_allow_rule("Write(./build/*)").has_value());

        PermissionGate gate(store, PermissionMode::Default, mock_deny);
        auto ctx = make_ctx(PermissionMode::Default);

        // Direct child of ./build/ → Allow
        CHECK(gate.ask("Write", write_args("./build/output.a"),    ctx).kind == Decision::Kind::Allow);
        CHECK(gate.ask("Write", write_args("./build/batbox"),      ctx).kind == Decision::Kind::Allow);

        // Nested → Deny (* does not cross /)
        CHECK(gate.ask("Write", write_args("./build/src/foo.o"),   ctx).kind == Decision::Kind::Deny);
        // Outside build → Deny
        CHECK(gate.ask("Write", write_args("./src/main.cpp"),      ctx).kind == Decision::Kind::Deny);

        fs::remove_all(p.parent_path());
    }

    // ---- WebFetch URL glob -------------------------------------------------
    TEST_CASE("WebFetch(https://*.github.com/**) matches GitHub subdomain URLs") {
        fs::path p = make_temp_settings();
        auto store = std::make_shared<PermissionStore>(p);
        REQUIRE(store->add_allow_rule("WebFetch(https://*.github.com/**)").has_value());

        PermissionGate gate(store, PermissionMode::Default, mock_deny);
        auto ctx = make_ctx(PermissionMode::Default);

        // GitHub subdomain → Allow
        CHECK(gate.ask("WebFetch", webfetch_args("https://api.github.com/repos/foo/bar"),          ctx).kind == Decision::Kind::Allow);
        CHECK(gate.ask("WebFetch", webfetch_args("https://raw.github.com/user/repo/main/file.txt"),ctx).kind == Decision::Kind::Allow);

        // Wrong scheme → Deny
        CHECK(gate.ask("WebFetch", webfetch_args("http://api.github.com/repos"),                   ctx).kind == Decision::Kind::Deny);
        // No subdomain → Deny
        CHECK(gate.ask("WebFetch", webfetch_args("https://github.com/foo"),                        ctx).kind == Decision::Kind::Deny);
        // Different domain → Deny
        CHECK(gate.ask("WebFetch", webfetch_args("https://gitlab.com/user"),                       ctx).kind == Decision::Kind::Deny);

        fs::remove_all(p.parent_path());
    }

    // ---- Deny rule priority ------------------------------------------------
    TEST_CASE("Deny rule takes priority over allow rule for same pattern") {
        fs::path p = make_temp_settings();
        auto store = std::make_shared<PermissionStore>(p);
        // Both allow and deny rules match "Bash(git *)" — deny must win
        REQUIRE(store->add_allow_rule("Bash(git *)").has_value());
        REQUIRE(store->add_deny_rule("Bash(git *)").has_value());

        PermissionGate gate(store, PermissionMode::Default, mock_allow);
        auto ctx = make_ctx(PermissionMode::Default);

        auto d = gate.ask("Bash", bash_args("git push --force"), ctx);
        CHECK(d.kind == Decision::Kind::Deny);

        fs::remove_all(p.parent_path());
    }

    // ---- Multiple deny rules, first match wins ------------------------------
    TEST_CASE("First matching deny rule wins — subsequent deny rules not evaluated") {
        fs::path p = make_temp_settings();
        auto store = std::make_shared<PermissionStore>(p);
        REQUIRE(store->add_deny_rule("Bash(rm -rf **)").has_value());
        REQUIRE(store->add_deny_rule("Bash(*)").has_value());

        PermissionGate gate(store, PermissionMode::Default, mock_allow);
        auto ctx = make_ctx(PermissionMode::Default);

        // Matches first deny rule (rm -rf **) → Deny (no prompt)
        auto d = gate.ask("Bash", bash_args("rm -rf /tmp/foo/bar"), ctx);
        CHECK(d.kind == Decision::Kind::Deny);

        fs::remove_all(p.parent_path());
    }

    // ---- No match falls through to callback --------------------------------
    TEST_CASE("No rule match: gate falls through to callback") {
        fs::path p = make_temp_settings();
        auto store = std::make_shared<PermissionStore>(p);
        REQUIRE(store->add_allow_rule("Read(./src/**)").has_value());

        bool called = false;
        PermissionGate gate(store, PermissionMode::Default, [&](std::string_view, const Json&) {
            called = true;
            return Decision::deny();
        });
        auto ctx = make_ctx(PermissionMode::Default);

        // ./tests/** does not match ./src/** → callback fires
        auto d = gate.ask("Read", read_args("./tests/unit/test_foo.cpp"), ctx);
        CHECK(d.kind == Decision::Kind::Deny);
        CHECK(called);

        fs::remove_all(p.parent_path());
    }

    // ---- Edit rule matching -------------------------------------------------
    TEST_CASE("Edit rule matches file_path argument correctly") {
        fs::path p = make_temp_settings();
        auto store = std::make_shared<PermissionStore>(p);
        REQUIRE(store->add_deny_rule("Edit(./src/core/**)").has_value());

        PermissionGate gate(store, PermissionMode::Default, mock_allow);
        auto ctx = make_ctx(PermissionMode::Default);

        // Match → Deny
        auto d1 = gate.ask("Edit", edit_args("./src/core/Result.hpp"), ctx);
        CHECK(d1.kind == Decision::Kind::Deny);

        // No match → Allow (callback)
        auto d2 = gate.ask("Edit", edit_args("./src/permissions/PermissionGate.cpp"), ctx);
        CHECK(d2.kind == Decision::Kind::Allow);

        fs::remove_all(p.parent_path());
    }
}

// ===========================================================================
// SUITE 9 — Acceptance criteria summary
//   Consolidates the 20 (4 modes × 5 tools) assertions in one place for a
//   clean CI output against the spec.
// ===========================================================================
TEST_SUITE("CPP 12.5 — 4 modes × 5 tools matrix (acceptance criteria)") {

    // Single test covering all 20 cells of the matrix.
    TEST_CASE("4 modes x 5 tools = 20 outcomes") {
        // No rules, prompt returns deny by default (so allow results must come
        // from mode-level auto-approve, not the callback).
        auto gate = make_gate(PermissionMode::Default, mock_deny);

        // -- Row: Nuclear (all Allow) -----------------------------------------
        {
            auto ctx = make_ctx(PermissionMode::Nuclear);
            CHECK(gate.ask("Read",     read_args("./x"),      ctx).kind == Decision::Kind::Allow);
            CHECK(gate.ask("Write",    write_args("./x"),     ctx).kind == Decision::Kind::Allow);
            CHECK(gate.ask("Edit",     edit_args("./x"),      ctx).kind == Decision::Kind::Allow);
            CHECK(gate.ask("Bash",     bash_args("cmd"),      ctx).kind == Decision::Kind::Allow);
            CHECK(gate.ask("WebFetch", webfetch_args("http://x"), ctx).kind == Decision::Kind::Allow);
        }

        // -- Row: Plan (Read Allow; Write/Edit/Bash/WebFetch Deny) ------------
        {
            auto ctx = make_ctx(PermissionMode::Plan);
            CHECK(gate.ask("Read",     read_args("./x"),      ctx).kind == Decision::Kind::Deny);  // mock_deny
            CHECK(gate.ask("Write",    write_args("./x"),     ctx).kind == Decision::Kind::Deny);
            CHECK(gate.ask("Edit",     edit_args("./x"),      ctx).kind == Decision::Kind::Deny);
            CHECK(gate.ask("Bash",     bash_args("cmd"),      ctx).kind == Decision::Kind::Deny);
            CHECK(gate.ask("WebFetch", webfetch_args("http://x"), ctx).kind == Decision::Kind::Deny);
        }

        // -- Row: AcceptEdits (Write/Edit Allow; others to callback=deny) -----
        {
            auto ctx = make_ctx(PermissionMode::AcceptEdits);
            CHECK(gate.ask("Read",     read_args("./x"),      ctx).kind == Decision::Kind::Deny);   // callback
            CHECK(gate.ask("Write",    write_args("./x"),     ctx).kind == Decision::Kind::Allow);  // auto
            CHECK(gate.ask("Edit",     edit_args("./x"),      ctx).kind == Decision::Kind::Allow);  // auto
            CHECK(gate.ask("Bash",     bash_args("cmd"),      ctx).kind == Decision::Kind::Deny);   // callback
            CHECK(gate.ask("WebFetch", webfetch_args("http://x"), ctx).kind == Decision::Kind::Deny); // callback
        }

        // -- Row: Default (all to callback=deny) ------------------------------
        {
            auto ctx = make_ctx(PermissionMode::Default);
            CHECK(gate.ask("Read",     read_args("./x"),      ctx).kind == Decision::Kind::Deny);
            CHECK(gate.ask("Write",    write_args("./x"),     ctx).kind == Decision::Kind::Deny);
            CHECK(gate.ask("Edit",     edit_args("./x"),      ctx).kind == Decision::Kind::Deny);
            CHECK(gate.ask("Bash",     bash_args("cmd"),      ctx).kind == Decision::Kind::Deny);
            CHECK(gate.ask("WebFetch", webfetch_args("http://x"), ctx).kind == Decision::Kind::Deny);
        }
    }

    TEST_CASE("Plan mode: Read passes through (mock_allow as callback)") {
        // A separate gate where callback = allow, so Read in Plan mode → Allow
        auto gate = make_gate(PermissionMode::Default, mock_allow);
        auto ctx  = make_ctx(PermissionMode::Plan);
        auto d    = gate.ask("Read", read_args("./src/main.cpp"), ctx);
        CHECK(d.kind == Decision::Kind::Allow);
    }
}
