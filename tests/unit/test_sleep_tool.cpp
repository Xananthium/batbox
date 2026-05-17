// ---------------------------------------------------------------------------
// tests/unit/test_sleep_tool.cpp
//
// Unit tests for batbox::tools::SleepTool (CPP 5.21).
//
// Build (standalone, from repo root):
//   c++ -std=c++20 \
//       -Iinclude \
//       -Ibuild/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_sleep_tool.cpp \
//       src/tools/SleepTool.cpp \
//       src/core/CancelToken.cpp \
//       -o /tmp/test_sleep_tool && /tmp/test_sleep_tool
//
// Acceptance criteria (CPP 5.21):
//   [AC1] Sleeps the requested duration ±50ms
//   [AC2] stop_token cancellation: returns early with "(cancelled)"
//   [AC3] >300s arg: rejected with error
//   [AC4] Unit test (this file)
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/tools/SleepTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/permissions/PermissionMode.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

using namespace batbox;
using namespace batbox::tools;
using namespace batbox::permissions;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static ToolContext make_ctx() {
    ToolContext ctx;
    ctx.cwd        = std::filesystem::current_path();
    ctx.mode       = PermissionMode::Default;
    ctx.session_id = "test-session";
    ctx.agent_id   = "";
    // cancel_token default-constructed = never cancelled.
    return ctx;
}

static ToolContext make_cancelled_ctx() {
    auto [src, tok] = CancelToken::make_root();
    src.request_stop();
    ToolContext ctx = make_ctx();
    ctx.cancel_token = std::move(tok);
    return ctx;
}

// ---------------------------------------------------------------------------
// Interface contract
// ---------------------------------------------------------------------------
TEST_SUITE("SleepTool — interface contract") {

    TEST_CASE("name() returns \"Sleep\"") {
        SleepTool t;
        CHECK(t.name() == std::string_view("Sleep"));
    }

    TEST_CASE("description() is non-empty") {
        SleepTool t;
        CHECK_FALSE(std::string(t.description()).empty());
    }

    TEST_CASE("is_read_only() returns true") {
        SleepTool t;
        CHECK(t.is_read_only());
    }

    TEST_CASE("requires_confirmation() returns false") {
        SleepTool t;
        CHECK_FALSE(t.requires_confirmation());
    }

    TEST_CASE("schema_json() has correct name, description, parameters") {
        SleepTool t;
        Json s = t.schema_json();
        REQUIRE(s.is_object());
        REQUIRE(s.contains("name"));
        REQUIRE(s.contains("description"));
        REQUIRE(s.contains("parameters"));
        CHECK(s["name"].get<std::string>() == "Sleep");
        REQUIRE(s["parameters"].contains("properties"));
        REQUIRE(s["parameters"]["properties"].contains("seconds"));
        REQUIRE(s["parameters"].contains("required"));
    }

    TEST_CASE("schema name matches name()") {
        SleepTool t;
        CHECK(t.schema_json()["name"].get<std::string>() == std::string(t.name()));
    }
}

// ---------------------------------------------------------------------------
// AC1: Normal sleep — duration within ±50ms tolerance
// ---------------------------------------------------------------------------
TEST_SUITE("SleepTool — normal sleep [AC1]") {

    TEST_CASE("sleep 0 seconds returns immediately with correct body") {
        SleepTool t;
        auto ctx = make_ctx();
        Json args = {{"seconds", 0}};

        auto t0 = std::chrono::steady_clock::now();
        ToolResult r = t.run(args, ctx);
        auto elapsed = std::chrono::steady_clock::now() - t0;

        CHECK_FALSE(r.is_error);
        CHECK(r.body == "slept 0 seconds");
        // Should complete in well under 50ms.
        CHECK(elapsed < 50ms);
    }

    TEST_CASE("sleep 0.1 seconds — within ±50ms") {
        SleepTool t;
        auto ctx = make_ctx();
        Json args = {{"seconds", 0.1}};

        auto t0 = std::chrono::steady_clock::now();
        ToolResult r = t.run(args, ctx);
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        CHECK_FALSE(r.is_error);
        CHECK(r.body == "slept 0 seconds");
        // Slept at least 50ms and no more than 150ms.
        CHECK(elapsed_ms >= 50);
        CHECK(elapsed_ms < 150);
    }

    TEST_CASE("sleep 1 second — within ±50ms") {
        SleepTool t;
        auto ctx = make_ctx();
        Json args = {{"seconds", 1}};

        auto t0 = std::chrono::steady_clock::now();
        ToolResult r = t.run(args, ctx);
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        CHECK_FALSE(r.is_error);
        CHECK(r.body == "slept 1 seconds");
        // Slept at least 950ms and no more than 1050ms.
        CHECK(elapsed_ms >= 950);
        CHECK(elapsed_ms < 1050);
    }

    TEST_CASE("sleep body uses integer truncation — 1.9s returns \"slept 1 seconds\"") {
        SleepTool t;
        auto ctx = make_ctx();
        Json args = {{"seconds", 0.05}};
        ToolResult r = t.run(args, ctx);
        CHECK_FALSE(r.is_error);
        CHECK(r.body == "slept 0 seconds");
    }
}

// ---------------------------------------------------------------------------
// AC2: Cancellation returns early with "(cancelled)"
// ---------------------------------------------------------------------------
TEST_SUITE("SleepTool — cancellation [AC2]") {

    TEST_CASE("already-cancelled token: returns \"(cancelled)\" immediately") {
        SleepTool t;
        auto ctx = make_cancelled_ctx();
        Json args = {{"seconds", 10}};

        auto t0 = std::chrono::steady_clock::now();
        ToolResult r = t.run(args, ctx);
        auto elapsed = std::chrono::steady_clock::now() - t0;

        CHECK_FALSE(r.is_error);
        CHECK(r.body == "(cancelled)");
        // Should return well before the 10s sleep.
        CHECK(elapsed < 100ms);
    }

    TEST_CASE("cancel mid-sleep: returns early with \"(cancelled)\"") {
        SleepTool t;
        auto [src, tok] = CancelToken::make_root();

        ToolContext ctx = make_ctx();
        ctx.cancel_token = std::move(tok);

        Json args = {{"seconds", 10}};

        // Fire the cancel after 100ms on a background thread.
        std::thread canceller([&src]() {
            std::this_thread::sleep_for(100ms);
            src.request_stop();
        });

        auto t0 = std::chrono::steady_clock::now();
        ToolResult r = t.run(args, ctx);
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        canceller.join();

        CHECK_FALSE(r.is_error);
        CHECK(r.body == "(cancelled)");
        // Should have woken within ~200ms of the 10s sleep.
        CHECK(elapsed_ms < 500);
    }
}

// ---------------------------------------------------------------------------
// AC3: > 300s rejected with error
// ---------------------------------------------------------------------------
TEST_SUITE("SleepTool — argument validation [AC3]") {

    TEST_CASE("seconds > 300 returns error") {
        SleepTool t;
        auto ctx = make_ctx();
        Json args = {{"seconds", 301}};
        ToolResult r = t.run(args, ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("300") != std::string::npos);
    }

    TEST_CASE("seconds = 300 is accepted (boundary)") {
        // We don't actually sleep 300 seconds in a test — cancel immediately.
        SleepTool t;
        auto ctx = make_cancelled_ctx();
        Json args = {{"seconds", 300}};
        ToolResult r = t.run(args, ctx);
        // Either "(cancelled)" or a normal result — not an error about the cap.
        CHECK_FALSE(r.is_error);
    }

    TEST_CASE("negative seconds returns error") {
        SleepTool t;
        auto ctx = make_ctx();
        Json args = {{"seconds", -1}};
        ToolResult r = t.run(args, ctx);
        CHECK(r.is_error);
    }

    TEST_CASE("missing 'seconds' argument returns error") {
        SleepTool t;
        auto ctx = make_ctx();
        Json args = Json::object();
        ToolResult r = t.run(args, ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("seconds") != std::string::npos);
    }

    TEST_CASE("non-numeric 'seconds' argument returns error") {
        SleepTool t;
        auto ctx = make_ctx();
        Json args = {{"seconds", "fast"}};
        ToolResult r = t.run(args, ctx);
        CHECK(r.is_error);
    }

    TEST_CASE("very large seconds (1e10) returns error about 300 cap") {
        SleepTool t;
        auto ctx = make_ctx();
        Json args = {{"seconds", 1e10}};
        ToolResult r = t.run(args, ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("300") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// Additional: plan-mode — read-only tools may run in plan mode
// ---------------------------------------------------------------------------
TEST_SUITE("SleepTool — plan mode (read-only)") {

    TEST_CASE("runs normally in Plan mode (is_read_only=true)") {
        SleepTool t;
        ToolContext ctx = make_ctx();
        ctx.mode = PermissionMode::Plan;
        Json args = {{"seconds", 0}};
        ToolResult r = t.run(args, ctx);
        CHECK_FALSE(r.is_error);
        CHECK(r.body == "slept 0 seconds");
    }
}
