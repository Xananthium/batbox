// tests/unit/test_permission_store.cpp
// ---------------------------------------------------------------------------
// doctest suite for batbox::permissions::PermissionRule + PermissionStore.
//
// Covers:
//   - PermissionRule struct: fields, Kind enum, equality
//   - kind_to_string helper
//   - PermissionStore construction (missing file, empty file, malformed)
//   - add_allow_rule: add, dedup, persist
//   - add_deny_rule: add, dedup, persist
//   - add_ask_rule: add, dedup, persist
//   - remove_rule: removes from all lists, no-op on absent pattern
//   - rules(): merged typed vector in allow→deny→ask order
//   - Cross-list deduplication: same pattern in deny and ask stays independent
//   - Persistence round-trip: reload from disk reflects all mutations
//
// Build (standalone, no CMake):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_permission_store.cpp \
//       src/permissions/PermissionRule.cpp \
//       src/permissions/PermissionStore.cpp \
//       src/config/SettingsLoader.cpp \
//       src/core/Json.cpp \
//       src/core/Paths.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       -o /tmp/test_ps && /tmp/test_ps
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/permissions/PermissionRule.hpp>
#include <batbox/permissions/PermissionStore.hpp>
#include <batbox/config/SettingsLoader.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace batbox::permissions;

// ---------------------------------------------------------------------------
// Helper: create a fresh temp directory path for each test.
// Uses an atomic counter + timestamp for uniqueness.
// ---------------------------------------------------------------------------
static fs::path make_temp_settings() {
    static std::atomic<int> counter{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const int seq = ++counter;
    const std::string dir_name =
        "test_pstore_" + std::to_string(now) + "_" + std::to_string(seq);
    const fs::path tmp = fs::temp_directory_path() / dir_name;
    fs::create_directories(tmp);
    return tmp / "settings.json";
}

// ===========================================================================
// SUITE 1: PermissionRule struct
// ===========================================================================
TEST_SUITE("PermissionRule") {

    TEST_CASE("default construction") {
        PermissionRule r;
        CHECK(r.pattern.empty());
        CHECK(r.kind == PermissionRule::Kind::Allow);
    }

    TEST_CASE("two-arg constructor") {
        PermissionRule r("Bash(git *)", PermissionRule::Kind::Deny);
        CHECK(r.pattern == "Bash(git *)");
        CHECK(r.kind == PermissionRule::Kind::Deny);
    }

    TEST_CASE("equality: same pattern + kind") {
        PermissionRule a("Read(./src/**)", PermissionRule::Kind::Allow);
        PermissionRule b("Read(./src/**)", PermissionRule::Kind::Allow);
        CHECK(a == b);
        CHECK(!(a != b));
    }

    TEST_CASE("inequality: same pattern, different kind") {
        PermissionRule a("Read(./src/**)", PermissionRule::Kind::Allow);
        PermissionRule b("Read(./src/**)", PermissionRule::Kind::Deny);
        CHECK(a != b);
    }

    TEST_CASE("inequality: different pattern, same kind") {
        PermissionRule a("Read(./src/**)", PermissionRule::Kind::Allow);
        PermissionRule b("Write(./src/**)", PermissionRule::Kind::Allow);
        CHECK(a != b);
    }
}

// ===========================================================================
// SUITE 2: kind_to_string
// ===========================================================================
TEST_SUITE("kind_to_string") {

    TEST_CASE("Allow → \"allow\"") {
        CHECK(kind_to_string(PermissionRule::Kind::Allow) == "allow");
    }
    TEST_CASE("Deny → \"deny\"") {
        CHECK(kind_to_string(PermissionRule::Kind::Deny) == "deny");
    }
    TEST_CASE("Ask → \"ask\"") {
        CHECK(kind_to_string(PermissionRule::Kind::Ask) == "ask");
    }
}

// ===========================================================================
// SUITE 3: PermissionStore construction
// ===========================================================================
TEST_SUITE("PermissionStore — construction") {

    TEST_CASE("missing file: starts empty, no load error") {
        const fs::path p = fs::temp_directory_path() / "nonexistent_settings_xyz123.json";
        fs::remove(p);  // ensure absent

        PermissionStore store(p);
        CHECK(store.allow_rules().empty());
        CHECK(store.deny_rules().empty());
        CHECK(store.ask_rules().empty());
        CHECK(store.last_load_error().empty());
    }

    TEST_CASE("malformed JSON: starts empty, records load error") {
        const fs::path p = make_temp_settings();
        // Write garbage JSON
        {
            std::ofstream f(p);
            f << "{ not valid json !!!! }";
        }
        PermissionStore store(p);
        CHECK(store.allow_rules().empty());
        CHECK(store.deny_rules().empty());
        CHECK(store.ask_rules().empty());
        CHECK(!store.last_load_error().empty());

        fs::remove_all(p.parent_path());
    }

    TEST_CASE("valid file with pre-existing rules") {
        const fs::path p = make_temp_settings();
        {
            std::ofstream f(p);
            f << R"JSON({
  "permissions": {
    "allow": ["Bash(git *)", "Read(./src/**)"],
    "deny":  ["Bash(rm -rf *)"],
    "ask":   ["WebFetch(**)"]
  }
})JSON";
        }

        PermissionStore store(p);
        CHECK(store.last_load_error().empty());
        REQUIRE(store.allow_rules().size() == 2);
        CHECK(store.allow_rules()[0] == "Bash(git *)");
        CHECK(store.allow_rules()[1] == "Read(./src/**)");
        REQUIRE(store.deny_rules().size() == 1);
        CHECK(store.deny_rules()[0] == "Bash(rm -rf *)");
        REQUIRE(store.ask_rules().size() == 1);
        CHECK(store.ask_rules()[0] == "WebFetch(**)");

        fs::remove_all(p.parent_path());
    }

    TEST_CASE("settings_path() accessor returns construction path") {
        const fs::path p = make_temp_settings();
        PermissionStore store(p);
        CHECK(store.settings_path() == p);
        fs::remove_all(p.parent_path());
    }
}

// ===========================================================================
// SUITE 4: add_allow_rule
// ===========================================================================
TEST_SUITE("PermissionStore — add_allow_rule") {

    TEST_CASE("adds rule and persists") {
        const fs::path p = make_temp_settings();
        PermissionStore store(p);

        auto r = store.add_allow_rule("Bash(git status)");
        REQUIRE(r.has_value());

        // In-memory check
        REQUIRE(store.allow_rules().size() == 1);
        CHECK(store.allow_rules()[0] == "Bash(git status)");

        // Persistence round-trip: construct a new store from the same path
        PermissionStore store2(p);
        REQUIRE(store2.allow_rules().size() == 1);
        CHECK(store2.allow_rules()[0] == "Bash(git status)");

        fs::remove_all(p.parent_path());
    }

    TEST_CASE("adds multiple distinct rules") {
        const fs::path p = make_temp_settings();
        PermissionStore store(p);

        REQUIRE(store.add_allow_rule("Bash(git *)").has_value());
        REQUIRE(store.add_allow_rule("Read(./src/**)").has_value());
        REQUIRE(store.add_allow_rule("WebFetch(https://*.github.com/**)").has_value());

        REQUIRE(store.allow_rules().size() == 3);
        CHECK(store.allow_rules()[0] == "Bash(git *)");
        CHECK(store.allow_rules()[1] == "Read(./src/**)");
        CHECK(store.allow_rules()[2] == "WebFetch(https://*.github.com/**)");

        fs::remove_all(p.parent_path());
    }

    TEST_CASE("duplicate add is a no-op") {
        const fs::path p = make_temp_settings();
        PermissionStore store(p);

        REQUIRE(store.add_allow_rule("Bash(git status)").has_value());
        REQUIRE(store.add_allow_rule("Bash(git status)").has_value());  // dup

        // Still exactly one entry
        REQUIRE(store.allow_rules().size() == 1);

        // On-disk should also have exactly one entry
        PermissionStore store2(p);
        REQUIRE(store2.allow_rules().size() == 1);

        fs::remove_all(p.parent_path());
    }

    TEST_CASE("does not affect deny or ask lists") {
        const fs::path p = make_temp_settings();
        PermissionStore store(p);

        REQUIRE(store.add_allow_rule("Bash(git *)").has_value());

        CHECK(store.deny_rules().empty());
        CHECK(store.ask_rules().empty());

        fs::remove_all(p.parent_path());
    }
}

// ===========================================================================
// SUITE 5: add_deny_rule
// ===========================================================================
TEST_SUITE("PermissionStore — add_deny_rule") {

    TEST_CASE("adds rule and persists") {
        const fs::path p = make_temp_settings();
        PermissionStore store(p);

        REQUIRE(store.add_deny_rule("Bash(rm -rf *)").has_value());

        REQUIRE(store.deny_rules().size() == 1);
        CHECK(store.deny_rules()[0] == "Bash(rm -rf *)");

        PermissionStore store2(p);
        REQUIRE(store2.deny_rules().size() == 1);
        CHECK(store2.deny_rules()[0] == "Bash(rm -rf *)");

        fs::remove_all(p.parent_path());
    }

    TEST_CASE("duplicate add is a no-op") {
        const fs::path p = make_temp_settings();
        PermissionStore store(p);

        REQUIRE(store.add_deny_rule("Bash(rm -rf *)").has_value());
        REQUIRE(store.add_deny_rule("Bash(rm -rf *)").has_value());

        REQUIRE(store.deny_rules().size() == 1);

        fs::remove_all(p.parent_path());
    }
}

// ===========================================================================
// SUITE 6: add_ask_rule
// ===========================================================================
TEST_SUITE("PermissionStore — add_ask_rule") {

    TEST_CASE("adds rule and persists") {
        const fs::path p = make_temp_settings();
        PermissionStore store(p);

        REQUIRE(store.add_ask_rule("WebFetch(**)").has_value());

        REQUIRE(store.ask_rules().size() == 1);
        CHECK(store.ask_rules()[0] == "WebFetch(**)");

        PermissionStore store2(p);
        REQUIRE(store2.ask_rules().size() == 1);

        fs::remove_all(p.parent_path());
    }

    TEST_CASE("duplicate add is a no-op") {
        const fs::path p = make_temp_settings();
        PermissionStore store(p);

        REQUIRE(store.add_ask_rule("WebFetch(**)").has_value());
        REQUIRE(store.add_ask_rule("WebFetch(**)").has_value());
        REQUIRE(store.ask_rules().size() == 1);

        fs::remove_all(p.parent_path());
    }
}

// ===========================================================================
// SUITE 7: remove_rule
// ===========================================================================
TEST_SUITE("PermissionStore — remove_rule") {

    TEST_CASE("removes from allow list") {
        const fs::path p = make_temp_settings();
        PermissionStore store(p);

        REQUIRE(store.add_allow_rule("Bash(git *)").has_value());
        REQUIRE(store.add_allow_rule("Read(./src/**)").has_value());

        REQUIRE(store.remove_rule("Bash(git *)").has_value());

        REQUIRE(store.allow_rules().size() == 1);
        CHECK(store.allow_rules()[0] == "Read(./src/**)");

        // Persistence round-trip
        PermissionStore store2(p);
        REQUIRE(store2.allow_rules().size() == 1);
        CHECK(store2.allow_rules()[0] == "Read(./src/**)");

        fs::remove_all(p.parent_path());
    }

    TEST_CASE("removes from deny list") {
        const fs::path p = make_temp_settings();
        PermissionStore store(p);

        REQUIRE(store.add_deny_rule("Bash(rm -rf *)").has_value());
        REQUIRE(store.remove_rule("Bash(rm -rf *)").has_value());

        CHECK(store.deny_rules().empty());

        fs::remove_all(p.parent_path());
    }

    TEST_CASE("removes from ask list") {
        const fs::path p = make_temp_settings();
        PermissionStore store(p);

        REQUIRE(store.add_ask_rule("WebFetch(**)").has_value());
        REQUIRE(store.remove_rule("WebFetch(**)").has_value());

        CHECK(store.ask_rules().empty());

        fs::remove_all(p.parent_path());
    }

    TEST_CASE("removes from all three lists simultaneously") {
        const fs::path p = make_temp_settings();
        PermissionStore store(p);

        // Manually write a settings file where the same pattern appears in all
        // three lists (unusual but the store must handle it gracefully).
        {
            std::ofstream f(p);
            f << R"JSON({
  "permissions": {
    "allow": ["Bash(git *)", "duplicate-pattern"],
    "deny":  ["duplicate-pattern", "Bash(rm -rf *)"],
    "ask":   ["duplicate-pattern"]
  }
})JSON";
        }
        PermissionStore store2(p);
        REQUIRE(store2.allow_rules().size() == 2);
        REQUIRE(store2.deny_rules().size() == 2);
        REQUIRE(store2.ask_rules().size() == 1);

        REQUIRE(store2.remove_rule("duplicate-pattern").has_value());

        REQUIRE(store2.allow_rules().size() == 1);
        CHECK(store2.allow_rules()[0] == "Bash(git *)");
        REQUIRE(store2.deny_rules().size() == 1);
        CHECK(store2.deny_rules()[0] == "Bash(rm -rf *)");
        CHECK(store2.ask_rules().empty());

        fs::remove_all(p.parent_path());
    }

    TEST_CASE("remove absent pattern is a no-op (returns Ok)") {
        const fs::path p = make_temp_settings();
        PermissionStore store(p);

        REQUIRE(store.add_allow_rule("Bash(git *)").has_value());
        // Remove a pattern that was never added
        auto r = store.remove_rule("nonexistent-pattern");
        REQUIRE(r.has_value());  // ok, not an error

        // The allow list is unchanged
        REQUIRE(store.allow_rules().size() == 1);
        CHECK(store.allow_rules()[0] == "Bash(git *)");

        fs::remove_all(p.parent_path());
    }
}

// ===========================================================================
// SUITE 8: rules() — merged typed vector
// ===========================================================================
TEST_SUITE("PermissionStore — rules()") {

    TEST_CASE("empty store returns empty vector") {
        const fs::path p = make_temp_settings();
        PermissionStore store(p);
        CHECK(store.rules().empty());
        fs::remove_all(p.parent_path());
    }

    TEST_CASE("merged in allow → deny → ask order with correct Kind") {
        const fs::path p = make_temp_settings();
        PermissionStore store(p);

        REQUIRE(store.add_allow_rule("Bash(git *)").has_value());
        REQUIRE(store.add_deny_rule("Bash(rm -rf *)").has_value());
        REQUIRE(store.add_ask_rule("WebFetch(**)").has_value());

        const auto all = store.rules();
        REQUIRE(all.size() == 3);

        CHECK(all[0].pattern == "Bash(git *)");
        CHECK(all[0].kind == PermissionRule::Kind::Allow);

        CHECK(all[1].pattern == "Bash(rm -rf *)");
        CHECK(all[1].kind == PermissionRule::Kind::Deny);

        CHECK(all[2].pattern == "WebFetch(**)");
        CHECK(all[2].kind == PermissionRule::Kind::Ask);

        fs::remove_all(p.parent_path());
    }

    TEST_CASE("multiple rules per list preserve insertion order") {
        const fs::path p = make_temp_settings();
        PermissionStore store(p);

        REQUIRE(store.add_allow_rule("A1").has_value());
        REQUIRE(store.add_allow_rule("A2").has_value());
        REQUIRE(store.add_deny_rule("D1").has_value());

        const auto all = store.rules();
        REQUIRE(all.size() == 3);
        CHECK(all[0].pattern == "A1");
        CHECK(all[1].pattern == "A2");
        CHECK(all[2].pattern == "D1");

        fs::remove_all(p.parent_path());
    }
}

// ===========================================================================
// SUITE 9: persistence round-trip with pre-existing settings values
// ===========================================================================
TEST_SUITE("PermissionStore — persistence preserves other settings fields") {

    TEST_CASE("theme and output_style survive a rule mutation") {
        // Write a settings file with theme + permissions
        const fs::path p = make_temp_settings();
        {
            std::ofstream f(p);
            f << R"JSON({
  "permissions": {
    "allow": [],
    "deny": [],
    "ask": []
  },
  "theme": "miss-kittin",
  "output_style": "compact"
})JSON";
        }

        PermissionStore store(p);
        REQUIRE(store.add_allow_rule("Bash(git *)").has_value());

        // Reload settings to check that theme and output_style are preserved
        auto res = batbox::config::load_settings(p);
        REQUIRE(res.has_value());
        CHECK(res->theme == "miss-kittin");
        CHECK(res->output_style == "compact");
        REQUIRE(res->permissions_allow.size() == 1);
        CHECK(res->permissions_allow[0] == "Bash(git *)");

        fs::remove_all(p.parent_path());
    }
}

// ===========================================================================
// SUITE 10: acceptance criteria from task specification
// ===========================================================================
TEST_SUITE("PermissionStore — task acceptance criteria") {

    TEST_CASE("add_allow_rule(\"Bash(git status)\") persists to settings.json") {
        const fs::path p = make_temp_settings();
        PermissionStore store(p);

        auto r = store.add_allow_rule("Bash(git status)");
        REQUIRE(r.has_value());

        // Confirm the file actually exists on disk
        CHECK(fs::exists(p));

        // Reload from a completely fresh store instance to verify on-disk state
        PermissionStore fresh(p);
        REQUIRE(fresh.allow_rules().size() == 1);
        CHECK(fresh.allow_rules()[0] == "Bash(git status)");

        fs::remove_all(p.parent_path());
    }

    TEST_CASE("duplicate rule add: deduplicates, no-op") {
        const fs::path p = make_temp_settings();
        PermissionStore store(p);

        REQUIRE(store.add_allow_rule("Bash(npm test:*)").has_value());
        REQUIRE(store.add_allow_rule("Bash(npm test:*)").has_value());
        REQUIRE(store.add_allow_rule("Bash(npm test:*)").has_value());

        // Still exactly one entry
        CHECK(store.allow_rules().size() == 1);

        // On-disk is also exactly one
        PermissionStore fresh(p);
        CHECK(fresh.allow_rules().size() == 1);

        fs::remove_all(p.parent_path());
    }

    TEST_CASE("remove_rule works") {
        const fs::path p = make_temp_settings();
        PermissionStore store(p);

        REQUIRE(store.add_allow_rule("Bash(git *)").has_value());
        REQUIRE(store.add_deny_rule("Bash(rm -rf *)").has_value());

        REQUIRE(store.remove_rule("Bash(git *)").has_value());
        CHECK(store.allow_rules().empty());

        REQUIRE(store.remove_rule("Bash(rm -rf *)").has_value());
        CHECK(store.deny_rules().empty());

        // On-disk is also clean
        PermissionStore fresh(p);
        CHECK(fresh.allow_rules().empty());
        CHECK(fresh.deny_rules().empty());

        fs::remove_all(p.parent_path());
    }

    TEST_CASE("all operations atomic — file is valid JSON after each operation") {
        const fs::path p = make_temp_settings();
        PermissionStore store(p);

        // After each mutation, the settings file must be valid JSON parseable
        // by load_settings.
        const std::vector<std::string> patterns = {
            "Bash(git *)", "Read(./src/**)", "Write(./build/*)",
            "WebFetch(https://*.github.com/**)", "WebSearch(rust*)"
        };

        for (const auto& pat : patterns) {
            REQUIRE(store.add_allow_rule(pat).has_value());

            auto res = batbox::config::load_settings(p);
            REQUIRE(res.has_value());
        }

        // Remove them all one by one
        for (const auto& pat : patterns) {
            REQUIRE(store.remove_rule(pat).has_value());

            auto res = batbox::config::load_settings(p);
            REQUIRE(res.has_value());
        }

        CHECK(store.allow_rules().empty());

        fs::remove_all(p.parent_path());
    }
}
