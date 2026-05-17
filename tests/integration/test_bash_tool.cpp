// tests/integration/test_bash_tool.cpp
//
// Integration tests for BashRunner (CPP 5.8 + CPP 5.9).
//
// Tests:
//   1.  echo hello → body "hello\n[exit=0, ...]", is_error=false
//   2.  false      → is_error=true, exit_code=1
//   3.  sleep 5 with 1s timeout → SIGTERM at ~1s
//   4.  output cap → truncation notice appended
//   5.  secret env scrub → BATBOX_API_KEY not present in child
//   6.  cancel_token → SIGINT sent to child
//   7.  plan mode → immediate error, no child spawned
//   8.  multiline output preserved
//   9.  ANSI escapes stripped from output body
//
//   CPP 5.9 — Pipes backend forced (BATBOX_BASH_BACKEND=pipes):
//   10. pipes: echo hello → same result shape as pty
//   11. pipes: output cap with truncation notice
//   12. pipes: cancel_token fires SIGINT and terminates child
//   13. pipes: BATBOX_API_KEY scrubbed from child environment
//   14. pipes: timeout fires watchdog and kills child
//
// Each test is self-contained and does not require a running network or GUI.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/tools/bash/BashRunner.hpp>
#include <batbox/core/CancelToken.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

using namespace batbox;
using namespace batbox::tools::bash;

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static const fs::path kCwd = fs::temp_directory_path();
static const std::vector<std::string> kDefaultAllowlist = {};
static constexpr int         kNoTimeout = 0;
static constexpr std::size_t kNoLimit   = 0;

/// Run a command with default settings and a fresh never-cancelled token.
static BashResult simple_run(const std::string& cmd,
                              int timeout_sec = kNoTimeout,
                              std::size_t max_bytes = kNoLimit)
{
    auto [src, tok] = CancelToken::make_root();
    BashRunner runner;
    return runner.run(cmd, kCwd, kDefaultAllowlist, timeout_sec, max_bytes, tok, false);
}

/// Run with BATBOX_BASH_BACKEND forced to a specific value.
static BashResult run_with_backend(const std::string& backend,
                                    const std::string& cmd,
                                    int timeout_sec = kNoTimeout,
                                    std::size_t max_bytes = kNoLimit)
{
    ::setenv("BATBOX_BASH_BACKEND", backend.c_str(), 1);
    auto [src, tok] = CancelToken::make_root();
    BashRunner runner;
    BashResult r = runner.run(cmd, kCwd, kDefaultAllowlist, timeout_sec, max_bytes, tok, false);
    ::unsetenv("BATBOX_BASH_BACKEND");
    return r;
}

// ---------------------------------------------------------------------------
// Test 1 — echo hello
// ---------------------------------------------------------------------------
TEST_CASE("BashRunner: echo hello returns exit 0 with body") {
    BashResult r = simple_run("echo hello");

    CHECK(r.is_error == false);
    CHECK(r.exit_code == 0);

    CHECK(r.body.find("hello") != std::string::npos);
    CHECK(r.body.find("[exit=0") != std::string::npos);
    CHECK(r.body.find("duration=") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Test 2 — false: exit code 1
// ---------------------------------------------------------------------------
TEST_CASE("BashRunner: false → is_error=true, exit_code=1") {
    BashResult r = simple_run("false");

    CHECK(r.is_error == true);
    CHECK(r.exit_code == 1);
    CHECK(r.body.find("[exit=1") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Test 3 — timeout: sleep 5 with 1s limit
// ---------------------------------------------------------------------------
TEST_CASE("BashRunner: sleep 5 with 1s timeout fires SIGTERM") {
    const auto before = std::chrono::steady_clock::now();
    BashResult r = simple_run("sleep 5", /*timeout_sec=*/1);
    const auto after  = std::chrono::steady_clock::now();

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          after - before).count();

    // Should finish in ≤ 4 s (1s timeout + 2s kill grace + margin).
    CHECK(elapsed_ms < 4000);

    // is_error=true because signal-killed.
    CHECK(r.is_error == true);

    CHECK(r.body.find("duration=") != std::string::npos);
    CHECK(r.body.find("[exit=") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Test 4 — output cap: generate >1024 bytes, cap at 512 bytes
// ---------------------------------------------------------------------------
TEST_CASE("BashRunner: output capped at max_output_bytes with truncation notice") {
    BashResult r = simple_run(
        "python3 -c \"print('A' * 4096)\"",
        kNoTimeout,
        /*max_output_bytes=*/512
    );

    CHECK(r.body.find("truncated") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Test 5 — secret env scrub
// ---------------------------------------------------------------------------
TEST_CASE("BashRunner: BATBOX_API_KEY scrubbed from child environment") {
    ::setenv("BATBOX_API_KEY", "super_secret_12345", 1);

    BashResult r = simple_run("echo ${BATBOX_API_KEY:-NOT_SET}");

    ::unsetenv("BATBOX_API_KEY");

    CHECK(r.body.find("super_secret_12345") == std::string::npos);
    CHECK((r.body.find("NOT_SET") != std::string::npos
           || r.body.find("super_secret") == std::string::npos));
}

// ---------------------------------------------------------------------------
// Test 6 — cancel_token cancels a running process
// ---------------------------------------------------------------------------
TEST_CASE("BashRunner: cancel_token fires SIGINT and terminates child") {
    auto [src, tok] = CancelToken::make_root();

    std::thread cancel_thread([&src]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        src.request_stop();
    });

    BashRunner runner;
    const auto before = std::chrono::steady_clock::now();
    BashResult r = runner.run(
        "sleep 10",
        kCwd,
        kDefaultAllowlist,
        kNoTimeout,
        kNoLimit,
        tok,
        false
    );
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - before).count();

    cancel_thread.join();

    CHECK(elapsed_ms < 3500);
    CHECK(r.is_error == true);
    CHECK(r.body.find("[exit=") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Test 7 — plan mode: refuses to execute
// ---------------------------------------------------------------------------
TEST_CASE("BashRunner: plan mode returns error without spawning a child") {
    auto [src, tok] = CancelToken::make_root();
    BashRunner runner;

    BashResult r = runner.run(
        "echo should_not_run",
        kCwd,
        kDefaultAllowlist,
        kNoTimeout,
        kNoLimit,
        tok,
        /*plan_mode=*/true
    );

    CHECK(r.is_error == true);
    CHECK(r.body.find("plan mode") != std::string::npos);
    CHECK(r.body.find("should_not_run") == std::string::npos);
}

// ---------------------------------------------------------------------------
// Test 8 — multiline output preserved
// ---------------------------------------------------------------------------
TEST_CASE("BashRunner: multiline output is captured intact") {
    BashResult r = simple_run("printf 'line1\\nline2\\nline3\\n'");

    CHECK(r.is_error == false);
    CHECK(r.body.find("line1") != std::string::npos);
    CHECK(r.body.find("line2") != std::string::npos);
    CHECK(r.body.find("line3") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Test 9 — ANSI escapes stripped from output body
// ---------------------------------------------------------------------------
TEST_CASE("BashRunner: ANSI escape sequences stripped from output") {
    BashResult r = simple_run("printf '\\033[31mhello\\033[0m\\n'");

    CHECK(r.is_error == false);
    CHECK(r.body.find("hello") != std::string::npos);
    CHECK(r.body.find('\x1b') == std::string::npos);
}

// ===========================================================================
// CPP 5.9 — Pipes backend tests (BATBOX_BASH_BACKEND=pipes forced)
// ===========================================================================

// ---------------------------------------------------------------------------
// Test 10 — pipes backend: echo hello produces same result shape
// ---------------------------------------------------------------------------
TEST_CASE("PipesBackend: echo hello returns exit 0 with body") {
    BashResult r = run_with_backend("pipes", "echo hello");

    CHECK(r.is_error == false);
    CHECK(r.exit_code == 0);
    CHECK(r.body.find("hello") != std::string::npos);
    CHECK(r.body.find("[exit=0") != std::string::npos);
    CHECK(r.body.find("duration=") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Test 11 — pipes backend: output cap with truncation notice
// ---------------------------------------------------------------------------
TEST_CASE("PipesBackend: output capped at max_output_bytes with truncation notice") {
    ::setenv("BATBOX_BASH_BACKEND", "pipes", 1);
    auto [src, tok] = CancelToken::make_root();
    BashRunner runner;
    BashResult r = runner.run(
        "python3 -c \"print('B' * 4096)\"",
        kCwd,
        kDefaultAllowlist,
        kNoTimeout,
        /*max_output_bytes=*/512,
        tok,
        false
    );
    ::unsetenv("BATBOX_BASH_BACKEND");

    CHECK(r.body.find("truncated") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Test 12 — pipes backend: cancel_token fires SIGINT and terminates child
// ---------------------------------------------------------------------------
TEST_CASE("PipesBackend: cancel_token fires SIGINT and terminates child") {
    ::setenv("BATBOX_BASH_BACKEND", "pipes", 1);

    auto [src, tok] = CancelToken::make_root();

    std::thread cancel_thread([&src]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        src.request_stop();
    });

    BashRunner runner;
    const auto before = std::chrono::steady_clock::now();
    BashResult r = runner.run(
        "sleep 10",
        kCwd,
        kDefaultAllowlist,
        kNoTimeout,
        kNoLimit,
        tok,
        false
    );
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - before).count();

    cancel_thread.join();
    ::unsetenv("BATBOX_BASH_BACKEND");

    // Should complete well within the SIGKILL grace window.
    CHECK(elapsed_ms < 3500);
    CHECK(r.is_error == true);
    CHECK(r.body.find("[exit=") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Test 13 — pipes backend: BATBOX_API_KEY scrubbed from child environment
// ---------------------------------------------------------------------------
TEST_CASE("PipesBackend: BATBOX_API_KEY scrubbed from child environment") {
    ::setenv("BATBOX_API_KEY", "pipes_secret_xyz", 1);

    BashResult r = run_with_backend("pipes", "echo ${BATBOX_API_KEY:-NOT_SET}");

    ::unsetenv("BATBOX_API_KEY");

    CHECK(r.body.find("pipes_secret_xyz") == std::string::npos);
    CHECK((r.body.find("NOT_SET") != std::string::npos
           || r.body.find("pipes_secret") == std::string::npos));
}

// ---------------------------------------------------------------------------
// Test 14 — pipes backend: timeout fires watchdog and kills child
// ---------------------------------------------------------------------------
TEST_CASE("PipesBackend: timeout watchdog fires and terminates child") {
    ::setenv("BATBOX_BASH_BACKEND", "pipes", 1);

    const auto before = std::chrono::steady_clock::now();
    auto [src, tok] = CancelToken::make_root();
    BashRunner runner;
    BashResult r = runner.run(
        "sleep 5",
        kCwd,
        kDefaultAllowlist,
        /*timeout_sec=*/1,
        kNoLimit,
        tok,
        false
    );
    const auto after = std::chrono::steady_clock::now();

    ::unsetenv("BATBOX_BASH_BACKEND");

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          after - before).count();

    CHECK(elapsed_ms < 4000);
    CHECK(r.is_error == true);
    CHECK(r.body.find("[exit=") != std::string::npos);
}
