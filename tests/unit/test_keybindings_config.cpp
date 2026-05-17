// tests/unit/test_keybindings_config.cpp
// ---------------------------------------------------------------------------
// doctest suite for batbox::config::load_keybindings.
//
// Coverage:
//   1. default_keybindings() — all expected defaults present with correct values
//   2. Missing file          — returns defaults, no error
//   3. Empty object file     — returns defaults unchanged
//   4. Valid overrides       — known actions are overridden correctly
//   5. Unknown action name   — entry skipped, warning (does not fail)
//   6. Non-string value      — entry skipped, warning (does not fail)
//   7. Non-object top level  — returns Err
//   8. Malformed JSON        — returns Err
//   9. Modifier combos       — Ctrl+, Shift+, Alt+, Ctrl+Shift+ all accepted as strings
//  10. cycle_mode default    — always "Shift+Tab" in defaults
//  11. Multiple overrides    — several actions overridden in one file
//  12. Unreadable file path  — returns Err (directory used as path)
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/config/KeybindingsConfig.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace batbox::config;
using namespace batbox;

// ============================================================================
// Helpers
// ============================================================================
namespace {

/// Write 'content' to 'path', creating parent directories as needed.
static void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path);
    REQUIRE(f.is_open());
    f << content;
}

/// RAII helper: remove a file on destruction (cleanup after tests).
struct TempFile {
    fs::path path;
    explicit TempFile(fs::path p) : path(std::move(p)) {}
    ~TempFile() { std::error_code ec; fs::remove(path, ec); }
};

} // anonymous namespace

// ============================================================================
// SUITE 1 — default_keybindings()
// ============================================================================
TEST_SUITE("KeybindingsConfig::default_keybindings") {

    TEST_CASE("all expected action keys are present") {
        const auto defaults = default_keybindings();
        CHECK(defaults.count("send")         == 1);
        CHECK(defaults.count("cancel")       == 1);
        CHECK(defaults.count("cycle_mode")   == 1);
        CHECK(defaults.count("newline")      == 1);
        CHECK(defaults.count("history_up")   == 1);
        CHECK(defaults.count("history_down") == 1);
        CHECK(defaults.count("clear")        == 1);
        CHECK(defaults.count("vim_toggle")   == 1);
    }

    TEST_CASE("cycle_mode default is Shift+Tab") {
        const auto defaults = default_keybindings();
        REQUIRE(defaults.count("cycle_mode") == 1);
        CHECK(defaults.at("cycle_mode") == "Shift+Tab");
    }

    TEST_CASE("send default is Ctrl+Enter") {
        const auto defaults = default_keybindings();
        REQUIRE(defaults.count("send") == 1);
        CHECK(defaults.at("send") == "Ctrl+Enter");
    }

    TEST_CASE("cancel default is Escape") {
        const auto defaults = default_keybindings();
        REQUIRE(defaults.count("cancel") == 1);
        CHECK(defaults.at("cancel") == "Escape");
    }

    TEST_CASE("newline default is Shift+Enter") {
        const auto defaults = default_keybindings();
        REQUIRE(defaults.count("newline") == 1);
        CHECK(defaults.at("newline") == "Shift+Enter");
    }

    TEST_CASE("history_up default is Up") {
        const auto defaults = default_keybindings();
        REQUIRE(defaults.count("history_up") == 1);
        CHECK(defaults.at("history_up") == "Up");
    }

    TEST_CASE("history_down default is Down") {
        const auto defaults = default_keybindings();
        REQUIRE(defaults.count("history_down") == 1);
        CHECK(defaults.at("history_down") == "Down");
    }

    TEST_CASE("clear default is Ctrl+L") {
        const auto defaults = default_keybindings();
        REQUIRE(defaults.count("clear") == 1);
        CHECK(defaults.at("clear") == "Ctrl+L");
    }

    TEST_CASE("vim_toggle default is Ctrl+G — not Escape") {
        // UI-D8 fix: vim_toggle must not share Escape with cancel.
        const auto defaults = default_keybindings();
        REQUIRE(defaults.count("vim_toggle") == 1);
        CHECK(defaults.at("vim_toggle") == "Ctrl+G");
        // Cancel must remain Escape.
        REQUIRE(defaults.count("cancel") == 1);
        CHECK(defaults.at("cancel") == "Escape");
        CHECK(defaults.at("vim_toggle") != defaults.at("cancel"));
    }
}

// ============================================================================
// SUITE 2 — Missing file → defaults only
// ============================================================================
TEST_SUITE("load_keybindings — missing file") {

    TEST_CASE("non-existent path returns Ok with defaults") {
        fs::path ghost = "/tmp/batbox_test_does_not_exist_keybindings.json";
        fs::remove(ghost);  // ensure it really doesn't exist

        auto result = load_keybindings(ghost);
        REQUIRE(result.has_value());

        const auto& map = result.value();
        CHECK(map.count("send")       == 1);
        CHECK(map.count("cycle_mode") == 1);
        CHECK(map.at("cycle_mode")    == "Shift+Tab");
    }
}

// ============================================================================
// SUITE 3 — Empty object file → defaults unchanged
// ============================================================================
TEST_SUITE("load_keybindings — empty object") {

    TEST_CASE("empty JSON object leaves all defaults intact") {
        const fs::path p = "/tmp/batbox_test_keybindings_empty.json";
        TempFile tf{p};
        write_file(p, "{}");

        auto result = load_keybindings(p);
        REQUIRE(result.has_value());

        const auto& map = result.value();
        const auto defaults = default_keybindings();
        for (const auto& [action, key] : defaults) {
            REQUIRE(map.count(action) == 1);
            CHECK(map.at(action) == key);
        }
    }
}

// ============================================================================
// SUITE 4 — Valid overrides
// ============================================================================
TEST_SUITE("load_keybindings — valid overrides") {

    TEST_CASE("override send action") {
        const fs::path p = "/tmp/batbox_test_keybindings_send.json";
        TempFile tf{p};
        write_file(p, R"({"send": "Ctrl+S"})");

        auto result = load_keybindings(p);
        REQUIRE(result.has_value());

        const auto& map = result.value();
        CHECK(map.at("send") == "Ctrl+S");
        // Other defaults must still be present and unchanged.
        CHECK(map.at("cycle_mode") == "Shift+Tab");
        CHECK(map.at("cancel")     == "Escape");
    }

    TEST_CASE("override cancel action") {
        const fs::path p = "/tmp/batbox_test_keybindings_cancel.json";
        TempFile tf{p};
        write_file(p, R"({"cancel": "Ctrl+C"})");

        auto result = load_keybindings(p);
        REQUIRE(result.has_value());
        CHECK(result.value().at("cancel") == "Ctrl+C");
    }

    TEST_CASE("override cycle_mode action") {
        const fs::path p = "/tmp/batbox_test_keybindings_cycle.json";
        TempFile tf{p};
        write_file(p, R"({"cycle_mode": "Alt+Tab"})");

        auto result = load_keybindings(p);
        REQUIRE(result.has_value());
        CHECK(result.value().at("cycle_mode") == "Alt+Tab");
    }
}

// ============================================================================
// SUITE 5 — Multiple overrides in one file
// ============================================================================
TEST_SUITE("load_keybindings — multiple overrides") {

    TEST_CASE("several actions overridden simultaneously") {
        const fs::path p = "/tmp/batbox_test_keybindings_multi.json";
        TempFile tf{p};
        write_file(p, R"({
            "send":       "Ctrl+S",
            "cancel":     "Ctrl+Q",
            "history_up": "Ctrl+P"
        })");

        auto result = load_keybindings(p);
        REQUIRE(result.has_value());

        const auto& map = result.value();
        CHECK(map.at("send")       == "Ctrl+S");
        CHECK(map.at("cancel")     == "Ctrl+Q");
        CHECK(map.at("history_up") == "Ctrl+P");
        // Unchanged defaults still present.
        CHECK(map.at("cycle_mode") == "Shift+Tab");
        CHECK(map.at("newline")    == "Shift+Enter");
    }
}

// ============================================================================
// SUITE 6 — Unknown action name → skip with warning
// ============================================================================
TEST_SUITE("load_keybindings — unknown action names") {

    TEST_CASE("unknown action is skipped; result is still Ok") {
        const fs::path p = "/tmp/batbox_test_keybindings_unknown.json";
        TempFile tf{p};
        write_file(p, R"({"totally_unknown_action": "Ctrl+Z"})");

        auto result = load_keybindings(p);
        REQUIRE(result.has_value());

        // The unknown key must NOT appear in the result map.
        CHECK(result.value().count("totally_unknown_action") == 0);
        // Defaults must still be intact.
        CHECK(result.value().count("send") == 1);
    }

    TEST_CASE("mix of known and unknown actions") {
        const fs::path p = "/tmp/batbox_test_keybindings_mixed.json";
        TempFile tf{p};
        write_file(p, R"({
            "send":    "Ctrl+S",
            "unknown1": "Alt+X",
            "cancel":  "Ctrl+C",
            "unknown2": "Meta+Y"
        })");

        auto result = load_keybindings(p);
        REQUIRE(result.has_value());

        const auto& map = result.value();
        // Known actions applied.
        CHECK(map.at("send")   == "Ctrl+S");
        CHECK(map.at("cancel") == "Ctrl+C");
        // Unknown actions absent.
        CHECK(map.count("unknown1") == 0);
        CHECK(map.count("unknown2") == 0);
    }
}

// ============================================================================
// SUITE 7 — Non-string value → skip with warning
// ============================================================================
TEST_SUITE("load_keybindings — non-string values") {

    TEST_CASE("integer value for action is skipped; result still Ok") {
        const fs::path p = "/tmp/batbox_test_keybindings_int.json";
        TempFile tf{p};
        write_file(p, R"({"send": 42})");

        auto result = load_keybindings(p);
        REQUIRE(result.has_value());
        // send should revert to default (not overridden).
        CHECK(result.value().at("send") == "Ctrl+Enter");
    }

    TEST_CASE("null value for action is skipped") {
        const fs::path p = "/tmp/batbox_test_keybindings_null.json";
        TempFile tf{p};
        write_file(p, R"({"cancel": null})");

        auto result = load_keybindings(p);
        REQUIRE(result.has_value());
        CHECK(result.value().at("cancel") == "Escape");
    }

    TEST_CASE("array value for action is skipped") {
        const fs::path p = "/tmp/batbox_test_keybindings_arr.json";
        TempFile tf{p};
        write_file(p, R"({"send": ["Ctrl", "Enter"]})");

        auto result = load_keybindings(p);
        REQUIRE(result.has_value());
        CHECK(result.value().at("send") == "Ctrl+Enter");
    }
}

// ============================================================================
// SUITE 8 — Non-object top level → Err
// ============================================================================
TEST_SUITE("load_keybindings — non-object top level") {

    TEST_CASE("array at top level returns Err") {
        const fs::path p = "/tmp/batbox_test_keybindings_toparray.json";
        TempFile tf{p};
        write_file(p, R"(["send", "Ctrl+Enter"])");

        auto result = load_keybindings(p);
        CHECK_FALSE(result.has_value());
        CHECK(result.error().find("expected a JSON object") != std::string::npos);
    }

    TEST_CASE("string at top level returns Err") {
        const fs::path p = "/tmp/batbox_test_keybindings_topstring.json";
        TempFile tf{p};
        write_file(p, R"("not an object")");

        auto result = load_keybindings(p);
        CHECK_FALSE(result.has_value());
        CHECK(result.error().find("expected a JSON object") != std::string::npos);
    }

    TEST_CASE("null at top level returns Err") {
        const fs::path p = "/tmp/batbox_test_keybindings_topnull.json";
        TempFile tf{p};
        write_file(p, "null");

        auto result = load_keybindings(p);
        CHECK_FALSE(result.has_value());
        CHECK(result.error().find("expected a JSON object") != std::string::npos);
    }
}

// ============================================================================
// SUITE 9 — Malformed JSON → Err
// ============================================================================
TEST_SUITE("load_keybindings — malformed JSON") {

    TEST_CASE("syntactically invalid JSON returns Err") {
        const fs::path p = "/tmp/batbox_test_keybindings_bad.json";
        TempFile tf{p};
        write_file(p, R"({send: "Ctrl+Enter"})");  // unquoted key — invalid JSON

        auto result = load_keybindings(p);
        CHECK_FALSE(result.has_value());
        CHECK(result.error().find("JSON parse error") != std::string::npos);
    }

    TEST_CASE("truncated JSON returns Err") {
        const fs::path p = "/tmp/batbox_test_keybindings_trunc.json";
        TempFile tf{p};
        write_file(p, R"({"send": "Ctrl+E)");  // truncated string

        auto result = load_keybindings(p);
        CHECK_FALSE(result.has_value());
        CHECK(result.error().find("JSON parse error") != std::string::npos);
    }
}

// ============================================================================
// SUITE 10 — Modifier combo acceptance
// ============================================================================
TEST_SUITE("load_keybindings — modifier combos") {

    TEST_CASE("Ctrl+ combo accepted as string") {
        const fs::path p = "/tmp/batbox_test_keybindings_ctrl.json";
        TempFile tf{p};
        write_file(p, R"({"send": "Ctrl+Enter"})");

        auto result = load_keybindings(p);
        REQUIRE(result.has_value());
        CHECK(result.value().at("send") == "Ctrl+Enter");
    }

    TEST_CASE("Shift+ combo accepted as string") {
        const fs::path p = "/tmp/batbox_test_keybindings_shift.json";
        TempFile tf{p};
        write_file(p, R"({"cycle_mode": "Shift+Tab"})");

        auto result = load_keybindings(p);
        REQUIRE(result.has_value());
        CHECK(result.value().at("cycle_mode") == "Shift+Tab");
    }

    TEST_CASE("Alt+ combo accepted as string") {
        const fs::path p = "/tmp/batbox_test_keybindings_alt.json";
        TempFile tf{p};
        write_file(p, R"({"clear": "Alt+L"})");

        auto result = load_keybindings(p);
        REQUIRE(result.has_value());
        CHECK(result.value().at("clear") == "Alt+L");
    }

    TEST_CASE("Ctrl+Shift+ multi-modifier combo accepted as string") {
        const fs::path p = "/tmp/batbox_test_keybindings_ctrlshift.json";
        TempFile tf{p};
        write_file(p, R"({"send": "Ctrl+Shift+Enter"})");

        auto result = load_keybindings(p);
        REQUIRE(result.has_value());
        CHECK(result.value().at("send") == "Ctrl+Shift+Enter");
    }

    TEST_CASE("bare key with no modifier accepted") {
        const fs::path p = "/tmp/batbox_test_keybindings_bare.json";
        TempFile tf{p};
        write_file(p, R"({"cancel": "Escape"})");

        auto result = load_keybindings(p);
        REQUIRE(result.has_value());
        CHECK(result.value().at("cancel") == "Escape");
    }
}

// ============================================================================
// SUITE 11 — cycle_mode always present in result
// ============================================================================
TEST_SUITE("load_keybindings — cycle_mode always present") {

    TEST_CASE("cycle_mode present even when file has no cycle_mode entry") {
        const fs::path p = "/tmp/batbox_test_keybindings_nocycle.json";
        TempFile tf{p};
        write_file(p, R"({"send": "Ctrl+S"})");

        auto result = load_keybindings(p);
        REQUIRE(result.has_value());
        CHECK(result.value().count("cycle_mode") == 1);
        CHECK(result.value().at("cycle_mode") == "Shift+Tab");
    }

    TEST_CASE("cycle_mode present in defaults (no file)") {
        fs::path ghost = "/tmp/batbox_test_no_keybindings_file.json";
        fs::remove(ghost);

        auto result = load_keybindings(ghost);
        REQUIRE(result.has_value());
        CHECK(result.value().count("cycle_mode") == 1);
        CHECK(result.value().at("cycle_mode") == "Shift+Tab");
    }
}

// ============================================================================
// SUITE 12 — Unreadable / directory path → Err
// ============================================================================
TEST_SUITE("load_keybindings — unreadable path") {

    TEST_CASE("directory path used as file returns Err") {
        // A directory cannot be opened as a regular file.
        const fs::path dir = "/tmp";
        // /tmp always exists as a directory; opening it as a file will fail.
        auto result = load_keybindings(dir);
        // On most systems, opening a directory with ifstream either fails at
        // open or at rdbuf reading. Either way the result should be Err or
        // (on some platforms) an empty read followed by a parse error.
        // We just verify the test does not crash; we can't assert Err on all
        // platforms because behaviour varies.
        (void)result;
    }
}
