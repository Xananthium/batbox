// =============================================================================
// tests/unit/test_logging.cpp — Unit tests for batbox::log (Logging.hpp/cpp)
//
// Build + run (standalone, no CMake needed):
//   c++ -std=c++20 \
//       -Iinclude \
//       -Ibuild/vcpkg_installed/arm64-osx/include \
//       -Lbuild/vcpkg_installed/arm64-osx/lib \
//       tests/unit/test_logging.cpp src/core/Logging.cpp \
//       -lspdlog -lfmt \
//       -o /tmp/test_logging && /tmp/test_logging
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "batbox/core/Logging.hpp"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
namespace {

// RAII guard: set an environment variable for the duration of a test, then
// restore (or unset) it when the guard goes out of scope.
struct EnvGuard {
    std::string key;
    std::string prev;
    bool had_prev;

    EnvGuard(const char* k, const char* v) : key(k) {
        const char* existing = std::getenv(k);
        had_prev = (existing != nullptr);
        prev = had_prev ? existing : "";
        ::setenv(k, v, 1);
    }
    ~EnvGuard() {
        if (had_prev) ::setenv(key.c_str(), prev.c_str(), 1);
        else          ::unsetenv(key.c_str());
    }
};

// Reset the spdlog registry so init_logging() runs fresh each test.
void reset_logging() {
    spdlog::drop_all();
    // Reset the internal initialized flag by re-initialising with a
    // suppressed-file sentinel so the flag is cleared via re-registration.
    // We do this by calling init through the public API with a no-file config.
    // The atomic guard inside init_logging uses double-checked locking,
    // but spdlog::drop_all() removes "batbox" from the registry, which lets
    // us re-run init_logging on the next call (the flag itself is reset
    // by the trick below: after drop_all the root logger lookup returns null,
    // so the factory branch runs again and sets g_initialized=true again —
    // which is the desired state for subsequent test cases).
    //
    // For true isolation we expose a test-only reset helper via a compile flag.
}

// Read entire file content into a string.
static std::string slurp(const std::string& path) {
    std::ifstream f(path);
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

} // anonymous namespace

// =============================================================================
// TEST SUITE 1 — redact_secret (no spdlog dependency)
// =============================================================================
TEST_SUITE("log::redact_secret") {

    TEST_CASE("empty string returns ***") {
        CHECK(batbox::log::redact_secret("") == "***");
    }

    TEST_CASE("1-char string returns ***") {
        CHECK(batbox::log::redact_secret("x") == "***");
    }

    TEST_CASE("2-char string returns ***") {
        CHECK(batbox::log::redact_secret("xy") == "***");
    }

    TEST_CASE("3-char string returns ***") {
        // Exactly 3 chars: size <= keep, so fully redacted.
        CHECK(batbox::log::redact_secret("abc") == "***");
    }

    TEST_CASE("4-char string preserves first 3, redacts rest") {
        CHECK(batbox::log::redact_secret("abcd") == "abc***");
    }

    TEST_CASE("sk-abc123 key preserves sk- and redacts rest") {
        CHECK(batbox::log::redact_secret("sk-abc123") == "sk-***");
    }

    TEST_CASE("longer key preserves exactly 3 chars") {
        std::string key = "sk-" + std::string(50, 'x');
        std::string result = batbox::log::redact_secret(key);
        CHECK(result == "sk-***");
        CHECK(result.size() == 6u); // "sk-" + "***"
    }

    TEST_CASE("preserves first 3 regardless of content") {
        CHECK(batbox::log::redact_secret("ABCDEF") == "ABC***");
        CHECK(batbox::log::redact_secret("1234567890") == "123***");
    }
}

// =============================================================================
// TEST SUITE 2 — get() before init_logging (no crash, returns valid logger)
// =============================================================================
TEST_SUITE("log::get — pre-init safety") {

    TEST_CASE("get() before init returns non-null logger") {
        spdlog::drop_all(); // ensure clean state
        auto lg = batbox::log::get("pre_init_module");
        CHECK(lg != nullptr);
        // Should not crash when logging at warn level.
        REQUIRE_NOTHROW(lg->warn("pre-init log from module"));
    }

    TEST_CASE("BATBOX_LOG_WARN macro does not crash before init") {
        spdlog::drop_all();
        REQUIRE_NOTHROW(BATBOX_LOG_WARN("pre-init macro warn"));
    }

    TEST_CASE("BATBOX_LOG_ERROR macro does not crash before init") {
        spdlog::drop_all();
        REQUIRE_NOTHROW(BATBOX_LOG_ERROR("pre-init macro error"));
    }
}

// =============================================================================
// TEST SUITE 3 — init_logging() produces a working logger
// =============================================================================
TEST_SUITE("log::init_logging — basic init") {

    TEST_CASE("init with info level — root logger exists and accepts info messages") {
        spdlog::drop_all();
        batbox::log::LogConfig cfg;
        cfg.level    = "info";
        cfg.log_file = std::string(1, '\x00'); // suppress file output
        batbox::log::init_logging(cfg);

        auto root = spdlog::get("batbox");
        REQUIRE(root != nullptr);
        CHECK(root->level() == spdlog::level::info);
        REQUIRE_NOTHROW(root->info("init_logging smoke test"));
    }

    TEST_CASE("init with debug level — root logger level is debug") {
        spdlog::drop_all();
        batbox::log::LogConfig cfg;
        cfg.level    = "debug";
        cfg.log_file = std::string(1, '\x00');
        batbox::log::init_logging(cfg);

        auto root = spdlog::get("batbox");
        REQUIRE(root != nullptr);
        CHECK(root->level() == spdlog::level::debug);
    }

    TEST_CASE("init with warn level — logger level is warn") {
        spdlog::drop_all();
        batbox::log::LogConfig cfg;
        cfg.level    = "warn";
        cfg.log_file = std::string(1, '\x00');
        batbox::log::init_logging(cfg);

        auto root = spdlog::get("batbox");
        REQUIRE(root != nullptr);
        CHECK(root->level() == spdlog::level::warn);
    }

    TEST_CASE("re-calling init_logging is a no-op (idempotent)") {
        spdlog::drop_all();
        batbox::log::LogConfig cfg;
        cfg.level    = "info";
        cfg.log_file = std::string(1, '\x00');
        batbox::log::init_logging(cfg);
        // Second call must not throw or crash.
        REQUIRE_NOTHROW(batbox::log::init_logging(cfg));
    }
}

// =============================================================================
// TEST SUITE 4 — level filtering
// =============================================================================
TEST_SUITE("log::level filtering") {

    TEST_CASE("info level suppresses debug messages") {
        spdlog::drop_all();
        batbox::log::LogConfig cfg;
        cfg.level    = "info";
        cfg.log_file = std::string(1, '\x00');
        batbox::log::init_logging(cfg);

        auto root = spdlog::get("batbox");
        REQUIRE(root != nullptr);
        CHECK_FALSE(root->should_log(spdlog::level::debug));
        CHECK_FALSE(root->should_log(spdlog::level::trace));
        CHECK(root->should_log(spdlog::level::info));
        CHECK(root->should_log(spdlog::level::warn));
        CHECK(root->should_log(spdlog::level::err));
    }

    TEST_CASE("debug level passes debug and above") {
        spdlog::drop_all();
        batbox::log::LogConfig cfg;
        cfg.level    = "debug";
        cfg.log_file = std::string(1, '\x00');
        batbox::log::init_logging(cfg);

        auto root = spdlog::get("batbox");
        REQUIRE(root != nullptr);
        CHECK(root->should_log(spdlog::level::debug));
        CHECK(root->should_log(spdlog::level::info));
        CHECK_FALSE(root->should_log(spdlog::level::trace));
    }

    TEST_CASE("error level suppresses info and debug") {
        spdlog::drop_all();
        batbox::log::LogConfig cfg;
        cfg.level    = "error";
        cfg.log_file = std::string(1, '\x00');
        batbox::log::init_logging(cfg);

        auto root = spdlog::get("batbox");
        REQUIRE(root != nullptr);
        CHECK_FALSE(root->should_log(spdlog::level::debug));
        CHECK_FALSE(root->should_log(spdlog::level::info));
        CHECK_FALSE(root->should_log(spdlog::level::warn));
        CHECK(root->should_log(spdlog::level::err));
        CHECK(root->should_log(spdlog::level::critical));
    }
}

// =============================================================================
// TEST SUITE 5 — file output
// =============================================================================
TEST_SUITE("log::file output") {

    TEST_CASE("logs appear in the specified file after init") {
        spdlog::drop_all();

        const std::string tmp_log = "/tmp/batbox_test_logging.log";
        // Remove stale file from a previous run.
        std::filesystem::remove(tmp_log);

        batbox::log::LogConfig cfg;
        cfg.level    = "info";
        cfg.log_file = tmp_log;
        batbox::log::init_logging(cfg);

        auto root = spdlog::get("batbox");
        REQUIRE(root != nullptr);
        root->info("hello from file output test sentinel_abc123");
        root->flush();

        CHECK(std::filesystem::exists(tmp_log));
        const std::string content = slurp(tmp_log);
        CHECK(content.find("sentinel_abc123") != std::string::npos);

        // Cleanup.
        std::filesystem::remove(tmp_log);
    }

    TEST_CASE("BATBOX_LOG_INFO macro writes to file") {
        spdlog::drop_all();

        const std::string tmp_log = "/tmp/batbox_test_macro_log.log";
        std::filesystem::remove(tmp_log);

        batbox::log::LogConfig cfg;
        cfg.level    = "info";
        cfg.log_file = tmp_log;
        batbox::log::init_logging(cfg);

        BATBOX_LOG_INFO("macro_sentinel_xyz789");
        batbox::log::get("batbox")->flush();

        CHECK(std::filesystem::exists(tmp_log));
        const std::string content = slurp(tmp_log);
        CHECK(content.find("macro_sentinel_xyz789") != std::string::npos);

        std::filesystem::remove(tmp_log);
    }
}

// =============================================================================
// TEST SUITE 6 — per-module loggers
// =============================================================================
TEST_SUITE("log::get — per-module loggers") {

    TEST_CASE("get returns non-null after init") {
        spdlog::drop_all();
        batbox::log::LogConfig cfg;
        cfg.level    = "info";
        cfg.log_file = std::string(1, '\x00');
        batbox::log::init_logging(cfg);

        auto lg = batbox::log::get("inference");
        CHECK(lg != nullptr);
    }

    TEST_CASE("get with same name returns the same logger") {
        spdlog::drop_all();
        batbox::log::LogConfig cfg;
        cfg.level    = "info";
        cfg.log_file = std::string(1, '\x00');
        batbox::log::init_logging(cfg);

        auto a = batbox::log::get("session");
        auto b = batbox::log::get("session");
        CHECK(a.get() == b.get());
    }

    TEST_CASE("different module names return different loggers") {
        spdlog::drop_all();
        batbox::log::LogConfig cfg;
        cfg.level    = "info";
        cfg.log_file = std::string(1, '\x00');
        batbox::log::init_logging(cfg);

        auto a = batbox::log::get("mcp");
        auto b = batbox::log::get("sidecar");
        CHECK(a.get() != b.get());
        CHECK(a->name() == "mcp");
        CHECK(b->name() == "sidecar");
    }

    TEST_CASE("module loggers inherit root level") {
        spdlog::drop_all();
        batbox::log::LogConfig cfg;
        cfg.level    = "warn";
        cfg.log_file = std::string(1, '\x00');
        batbox::log::init_logging(cfg);

        auto lg = batbox::log::get("tools");
        REQUIRE(lg != nullptr);
        CHECK(lg->level() == spdlog::level::warn);
    }

    TEST_CASE("module logger does not crash when logging") {
        spdlog::drop_all();
        batbox::log::LogConfig cfg;
        cfg.level    = "info";
        cfg.log_file = std::string(1, '\x00');
        batbox::log::init_logging(cfg);

        auto lg = batbox::log::get("inference");
        REQUIRE_NOTHROW(lg->info("inference module test message"));
        REQUIRE_NOTHROW(lg->warn("inference module warn"));
        REQUIRE_NOTHROW(lg->error("inference module error"));
    }

    TEST_CASE("pre-init module logger does not crash") {
        spdlog::drop_all();
        // Deliberately do NOT call init_logging.
        auto lg = batbox::log::get("pre_init_check");
        REQUIRE(lg != nullptr);
        REQUIRE_NOTHROW(lg->warn("pre-init module warn"));
        REQUIRE_NOTHROW(lg->error("pre-init module error"));
    }
}

// =============================================================================
// TEST SUITE 7 — env var driven configuration
// =============================================================================
TEST_SUITE("log::env var configuration") {

    TEST_CASE("BATBOX_LOG_LEVEL=debug via env sets debug level") {
        spdlog::drop_all();
        EnvGuard g("BATBOX_LOG_LEVEL", "debug");
        // Suppress file output for this test.
        batbox::log::LogConfig cfg;
        cfg.log_file = std::string(1, '\x00');
        batbox::log::init_logging(cfg);

        auto root = spdlog::get("batbox");
        REQUIRE(root != nullptr);
        CHECK(root->level() == spdlog::level::debug);
    }

    TEST_CASE("BATBOX_LOG_LEVEL=info via env sets info level") {
        spdlog::drop_all();
        EnvGuard g("BATBOX_LOG_LEVEL", "info");
        batbox::log::LogConfig cfg;
        cfg.log_file = std::string(1, '\x00');
        batbox::log::init_logging(cfg);

        auto root = spdlog::get("batbox");
        REQUIRE(root != nullptr);
        CHECK(root->level() == spdlog::level::info);
    }
}
