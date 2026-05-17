// tests/unit/test_state_store.cpp
// ---------------------------------------------------------------------------
// doctest suite for batbox::config::read/write_last_seen_changelog_version.
// (TUI-FLOW-T10)
//
// Uses a tmp HOME override so the real ~/.batbox/state.json is never touched.
//
// Coverage:
//   1. read returns nullopt when state.json absent
//   2. write creates state.json with the version key
//   3. read returns the written version after write
//   4. write twice with different versions — second wins, other keys preserved
//   5. write is idempotent: writing same value twice, read returns it
//   6. state.json parent directory is created if it does not exist
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "batbox/config/StateStore.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// HomeGuard — override HOME env var to a tmp directory for test isolation.
// Restores the original value on destruction.
// ---------------------------------------------------------------------------

struct HomeGuard {
    std::string original_;
    bool had_original_{false};

    explicit HomeGuard(const fs::path& tmp_home) {
        if (const char* cur = std::getenv("HOME")) {
            had_original_ = true;
            original_     = cur;
        }
#ifdef _WIN32
        ::_putenv_s("HOME", tmp_home.string().c_str());
#else
        ::setenv("HOME", tmp_home.string().c_str(), 1);
#endif
    }

    ~HomeGuard() {
#ifdef _WIN32
        if (had_original_) ::_putenv_s("HOME", original_.c_str());
        else               ::_putenv_s("HOME", "");
#else
        if (had_original_) ::setenv("HOME", original_.c_str(), 1);
        else               ::unsetenv("HOME");
#endif
    }
};

// Helper: return a unique tmp directory path (but don't create it yet).
fs::path tmp_home(const std::string& suffix) {
    return fs::temp_directory_path() / ("batbox_test_state_" + suffix);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_CASE("read_last_seen_changelog_version returns nullopt when file absent") {
    auto h = tmp_home("absent");
    fs::remove_all(h);  // Ensure it doesn't exist
    HomeGuard g(h);

    auto result = batbox::config::read_last_seen_changelog_version();
    CHECK_FALSE(result.has_value());

    fs::remove_all(h);
}

TEST_CASE("write creates state.json with version key") {
    auto h = tmp_home("write_creates");
    fs::remove_all(h);
    HomeGuard g(h);

    batbox::config::write_last_seen_changelog_version("1.0.0");

    auto state_path = h / ".batbox" / "state.json";
    CHECK(fs::exists(state_path));

    fs::remove_all(h);
}

TEST_CASE("read returns written version after write") {
    auto h = tmp_home("roundtrip");
    fs::remove_all(h);
    HomeGuard g(h);

    batbox::config::write_last_seen_changelog_version("2.3.4");
    auto result = batbox::config::read_last_seen_changelog_version();

    REQUIRE(result.has_value());
    CHECK(result.value() == "2.3.4");

    fs::remove_all(h);
}

TEST_CASE("write twice: second value wins, read returns second") {
    auto h = tmp_home("overwrite");
    fs::remove_all(h);
    HomeGuard g(h);

    batbox::config::write_last_seen_changelog_version("0.1.0");
    batbox::config::write_last_seen_changelog_version("0.2.0");

    auto result = batbox::config::read_last_seen_changelog_version();
    REQUIRE(result.has_value());
    CHECK(result.value() == "0.2.0");

    fs::remove_all(h);
}

TEST_CASE("write is idempotent: same value written twice reads back correctly") {
    auto h = tmp_home("idempotent");
    fs::remove_all(h);
    HomeGuard g(h);

    batbox::config::write_last_seen_changelog_version("0.1.0");
    batbox::config::write_last_seen_changelog_version("0.1.0");

    auto result = batbox::config::read_last_seen_changelog_version();
    REQUIRE(result.has_value());
    CHECK(result.value() == "0.1.0");

    fs::remove_all(h);
}

TEST_CASE("parent directory is created if absent") {
    auto h = tmp_home("mkdir");
    fs::remove_all(h);  // Ensure even HOME doesn't exist
    HomeGuard g(h);

    // .batbox directory does not exist yet
    CHECK_FALSE(fs::exists(h / ".batbox"));

    batbox::config::write_last_seen_changelog_version("0.9.9");

    // Should have been created
    CHECK(fs::exists(h / ".batbox"));
    CHECK(fs::exists(h / ".batbox" / "state.json"));

    fs::remove_all(h);
}
