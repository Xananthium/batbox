// tests/unit/test_settings_loader.cpp
// ---------------------------------------------------------------------------
// doctest suite for batbox::config::Settings + load_settings() / write_settings()
//
// Coverage:
//   1. Missing file -> returns default Settings (no error)
//   2. Well-formed JSON -> all fields parsed correctly
//   3. permissions.allow / deny / ask arrays
//   4. theme field
//   5. plugins.disabled array
//   6. output_style field
//   7. Malformed JSON -> returns error with byte offset
//   8. Root not an object -> returns error
//   9. write_settings -> file created, content is valid JSON parseable back
//  10. write_settings is atomic: tmp file is not left behind on success
//  11. Round-trip: write then read back -> identical struct
//  12. Missing optional keys -> defaults (empty strings / empty vectors)
//  13. Extra/unknown JSON keys are silently ignored (forward-compat)
//  14. write_settings creates parent directory if absent
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/config/SettingsLoader.hpp>
#include <batbox/core/Json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using batbox::config::Settings;
using batbox::config::load_settings;
using batbox::config::write_settings;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
namespace {

fs::path make_tmp_dir()
{
    const fs::path base = fs::temp_directory_path() / "batbox_test_settings";
    std::error_code ec;
    fs::create_directories(base, ec);
    return base;
}

void write_raw(const fs::path& p, const std::string& content)
{
    std::ofstream ofs(p, std::ios::trunc);
    ofs << content;
}

std::string read_raw(const fs::path& p)
{
    std::ifstream ifs(p);
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

} // anonymous namespace

// ============================================================================
// SUITE 1 - load_settings
// ============================================================================
TEST_SUITE("load_settings") {

    TEST_CASE("missing file returns default Settings - not an error") {
        const fs::path p = make_tmp_dir() / "nonexistent_settings.json";
        fs::remove(p); // ensure absent

        const auto result = load_settings(p);
        REQUIRE(result.has_value());
        const auto& s = result.value();
        CHECK(s.permissions_allow.empty());
        CHECK(s.permissions_deny.empty());
        CHECK(s.permissions_ask.empty());
        CHECK(s.theme.empty());
        CHECK(s.plugins_disabled.empty());
        CHECK(s.output_style.empty());
    }

    TEST_CASE("well-formed JSON - all fields parsed") {
        const fs::path p = make_tmp_dir() / "full_settings.json";
        // Use named delimiter to avoid *)  triggering end of raw string
        write_raw(p, R"json({
  "permissions": {
    "allow": ["Bash(git *)", "Read(./src/**)"],
    "deny":  ["Bash(rm -rf *)"],
    "ask":   ["Bash(npm *)"]
  },
  "theme": "stock-exchange",
  "plugins": {
    "disabled": ["legacy-plugin", "broken-plugin"]
  },
  "output_style": "compact"
})json");

        const auto result = load_settings(p);
        REQUIRE(result.has_value());
        const auto& s = result.value();

        REQUIRE(s.permissions_allow.size() == 2);
        CHECK(s.permissions_allow[0] == "Bash(git *)");
        CHECK(s.permissions_allow[1] == "Read(./src/**)");

        REQUIRE(s.permissions_deny.size() == 1);
        CHECK(s.permissions_deny[0] == "Bash(rm -rf *)");

        REQUIRE(s.permissions_ask.size() == 1);
        CHECK(s.permissions_ask[0] == "Bash(npm *)");

        CHECK(s.theme == "stock-exchange");

        REQUIRE(s.plugins_disabled.size() == 2);
        CHECK(s.plugins_disabled[0] == "legacy-plugin");
        CHECK(s.plugins_disabled[1] == "broken-plugin");

        CHECK(s.output_style == "compact");
    }

    TEST_CASE("missing optional keys yield defaults") {
        const fs::path p = make_tmp_dir() / "empty_obj.json";
        write_raw(p, "{}");

        const auto result = load_settings(p);
        REQUIRE(result.has_value());
        const auto& s = result.value();
        CHECK(s.permissions_allow.empty());
        CHECK(s.permissions_deny.empty());
        CHECK(s.permissions_ask.empty());
        CHECK(s.theme.empty());
        CHECK(s.plugins_disabled.empty());
        CHECK(s.output_style.empty());
    }

    TEST_CASE("only permissions key present") {
        const fs::path p = make_tmp_dir() / "perms_only.json";
        write_raw(p, R"json({"permissions":{"allow":["Read(**)"]}})json");

        const auto result = load_settings(p);
        REQUIRE(result.has_value());
        const auto& s = result.value();
        REQUIRE(s.permissions_allow.size() == 1);
        CHECK(s.permissions_allow[0] == "Read(**)");
        CHECK(s.permissions_deny.empty());
        CHECK(s.theme.empty());
    }

    TEST_CASE("only theme present") {
        const fs::path p = make_tmp_dir() / "theme_only.json";
        write_raw(p, R"({"theme":"frank-sinatra"})");

        const auto result = load_settings(p);
        REQUIRE(result.has_value());
        CHECK(result.value().theme == "frank-sinatra");
    }

    TEST_CASE("only plugins.disabled present") {
        const fs::path p = make_tmp_dir() / "plugins_only.json";
        write_raw(p, R"({"plugins":{"disabled":["foo"]}})");

        const auto result = load_settings(p);
        REQUIRE(result.has_value());
        REQUIRE(result.value().plugins_disabled.size() == 1);
        CHECK(result.value().plugins_disabled[0] == "foo");
    }

    TEST_CASE("only output_style present") {
        const fs::path p = make_tmp_dir() / "os_only.json";
        write_raw(p, R"({"output_style":"verbose"})");

        const auto result = load_settings(p);
        REQUIRE(result.has_value());
        CHECK(result.value().output_style == "verbose");
    }

    TEST_CASE("extra/unknown keys are silently ignored") {
        const fs::path p = make_tmp_dir() / "extra_keys.json";
        write_raw(p, R"({
  "theme": "monochrome",
  "future_key": {"nested": true},
  "another_new_field": 42
})");

        const auto result = load_settings(p);
        REQUIRE(result.has_value());
        CHECK(result.value().theme == "monochrome");
    }

    TEST_CASE("malformed JSON returns Err with parse info") {
        const fs::path p = make_tmp_dir() / "malformed.json";
        write_raw(p, R"({"permissions": {"allow": [}})");

        const auto result = load_settings(p);
        REQUIRE_FALSE(result.has_value());
        // Error message should mention the file context.
        CHECK(!result.error().empty());
        CHECK(result.error().find("settings") != std::string::npos);
    }

    TEST_CASE("root is JSON array - returns error") {
        const fs::path p = make_tmp_dir() / "root_array.json";
        write_raw(p, R"(["not", "an", "object"])");

        const auto result = load_settings(p);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("object") != std::string::npos);
    }

    TEST_CASE("root is JSON string - returns error") {
        const fs::path p = make_tmp_dir() / "root_string.json";
        write_raw(p, R"("just a string")");

        const auto result = load_settings(p);
        REQUIRE_FALSE(result.has_value());
    }

    TEST_CASE("empty permissions arrays are accepted") {
        const fs::path p = make_tmp_dir() / "empty_perms.json";
        write_raw(p, R"({"permissions":{"allow":[],"deny":[],"ask":[]}})");

        const auto result = load_settings(p);
        REQUIRE(result.has_value());
        CHECK(result.value().permissions_allow.empty());
        CHECK(result.value().permissions_deny.empty());
        CHECK(result.value().permissions_ask.empty());
    }

    TEST_CASE("non-string elements in arrays are silently skipped") {
        const fs::path p = make_tmp_dir() / "mixed_array.json";
        write_raw(p, R"json({
  "permissions": {
    "allow": ["Bash(git *)", 42, null, true, "Read(**)"]
  }
})json");

        const auto result = load_settings(p);
        REQUIRE(result.has_value());
        // 42, null, true are skipped - only string elements survive.
        REQUIRE(result.value().permissions_allow.size() == 2);
        CHECK(result.value().permissions_allow[0] == "Bash(git *)");
        CHECK(result.value().permissions_allow[1] == "Read(**)");
    }
}

// ============================================================================
// SUITE 2 - write_settings
// ============================================================================
TEST_SUITE("write_settings") {

    TEST_CASE("basic write produces a valid JSON file") {
        const fs::path p = make_tmp_dir() / "written_settings.json";
        fs::remove(p);

        Settings s;
        s.permissions_allow = {"Bash(git *)", "Read(**)"};
        s.permissions_deny  = {"Bash(rm -rf *)"};
        s.theme             = "miss-kittin";
        s.plugins_disabled  = {"bad-plugin"};
        s.output_style      = "default";

        const auto result = write_settings(p, s);
        REQUIRE(result.has_value());

        // File must now exist.
        CHECK(fs::exists(p));

        // Content must be parseable JSON.
        const std::string raw = read_raw(p);
        batbox::Json j;
        bool parse_ok = true;
        try {
            j = batbox::Json::parse(raw);
        } catch (...) {
            parse_ok = false;
        }
        CHECK(parse_ok);
        CHECK(j.is_object());
    }

    TEST_CASE("tmp file is not left behind after successful write") {
        const fs::path p   = make_tmp_dir() / "atomic_test_settings.json";
        const fs::path tmp = fs::path(p.string() + ".tmp");
        fs::remove(p);
        fs::remove(tmp);

        const auto result = write_settings(p, Settings{});
        REQUIRE(result.has_value());

        CHECK(fs::exists(p));
        CHECK_FALSE(fs::exists(tmp));
    }

    TEST_CASE("round-trip: write then read back gives identical struct") {
        const fs::path p = make_tmp_dir() / "roundtrip_settings.json";
        fs::remove(p);

        Settings orig;
        orig.permissions_allow  = {"Bash(git *)", "Read(./src/**)"};
        orig.permissions_deny   = {"Bash(rm -rf *)"};
        orig.permissions_ask    = {"Bash(npm *)"};
        orig.theme              = "stock-exchange";
        orig.plugins_disabled   = {"legacy-plugin"};
        orig.output_style       = "compact";

        const auto wr = write_settings(p, orig);
        REQUIRE(wr.has_value());

        const auto lr = load_settings(p);
        REQUIRE(lr.has_value());
        const Settings& back = lr.value();

        CHECK(back.permissions_allow == orig.permissions_allow);
        CHECK(back.permissions_deny  == orig.permissions_deny);
        CHECK(back.permissions_ask   == orig.permissions_ask);
        CHECK(back.theme             == orig.theme);
        CHECK(back.plugins_disabled  == orig.plugins_disabled);
        CHECK(back.output_style      == orig.output_style);
    }

    TEST_CASE("round-trip with empty Settings (all defaults)") {
        const fs::path p = make_tmp_dir() / "roundtrip_empty.json";
        fs::remove(p);

        Settings orig; // all defaults

        const auto wr = write_settings(p, orig);
        REQUIRE(wr.has_value());

        const auto lr = load_settings(p);
        REQUIRE(lr.has_value());
        const Settings& back = lr.value();

        CHECK(back.permissions_allow.empty());
        CHECK(back.permissions_deny.empty());
        CHECK(back.permissions_ask.empty());
        CHECK(back.theme.empty());
        CHECK(back.plugins_disabled.empty());
        CHECK(back.output_style.empty());
    }

    TEST_CASE("write creates parent directory if absent") {
        const fs::path dir = make_tmp_dir() / "subdir_that_does_not_exist";
        const fs::path p   = dir / "settings.json";
        fs::remove_all(dir); // ensure absent

        Settings s;
        s.theme = "classic";

        const auto result = write_settings(p, s);
        REQUIRE(result.has_value());
        CHECK(fs::exists(p));

        // Cleanup.
        fs::remove_all(dir);
    }

    TEST_CASE("overwrite existing file - round-trip with updated values") {
        const fs::path p = make_tmp_dir() / "overwrite_settings.json";

        // First write.
        Settings s1;
        s1.theme = "monochrome";
        REQUIRE(write_settings(p, s1).has_value());

        // Second write overwrites.
        Settings s2;
        s2.theme        = "frank-sinatra";
        s2.output_style = "verbose";
        REQUIRE(write_settings(p, s2).has_value());

        const auto lr = load_settings(p);
        REQUIRE(lr.has_value());
        CHECK(lr.value().theme        == "frank-sinatra");
        CHECK(lr.value().output_style == "verbose");
    }

    TEST_CASE("written file ends with a newline") {
        const fs::path p = make_tmp_dir() / "newline_check.json";
        const auto result = write_settings(p, Settings{});
        REQUIRE(result.has_value());

        const std::string raw = read_raw(p);
        REQUIRE_FALSE(raw.empty());
        CHECK(raw.back() == '\n');
    }

    TEST_CASE("permissions structure always present in output JSON") {
        // Even with an empty Settings, the permissions key must be emitted.
        const fs::path p = make_tmp_dir() / "perms_always.json";
        REQUIRE(write_settings(p, Settings{}).has_value());

        const std::string raw = read_raw(p);
        const batbox::Json j  = batbox::Json::parse(raw);
        CHECK(j.contains("permissions"));
        CHECK(j["permissions"].is_object());
        CHECK(j["permissions"].contains("allow"));
        CHECK(j["permissions"].contains("deny"));
        CHECK(j["permissions"].contains("ask"));
    }
}

// ============================================================================
// SUITE 3 - Semantic contract
// ============================================================================
TEST_SUITE("Settings semantic contract") {

    TEST_CASE("theme values round-trip cleanly") {
        const std::vector<std::string> themes = {
            "miss-kittin", "stock-exchange", "frank-sinatra", "monochrome", "classic"
        };

        for (const auto& theme : themes) {
            const fs::path p = make_tmp_dir() / ("theme_" + theme + ".json");
            Settings s;
            s.theme = theme;
            REQUIRE(write_settings(p, s).has_value());
            const auto lr = load_settings(p);
            REQUIRE(lr.has_value());
            CHECK(lr.value().theme == theme);
        }
    }

    TEST_CASE("output_style values round-trip cleanly") {
        for (const auto* style : {"default", "compact", "verbose"}) {
            const fs::path p = make_tmp_dir() / (std::string("ost_") + style + ".json");
            Settings s;
            s.output_style = style;
            REQUIRE(write_settings(p, s).has_value());
            const auto lr = load_settings(p);
            REQUIRE(lr.has_value());
            CHECK(lr.value().output_style == std::string(style));
        }
    }

    TEST_CASE("large permission lists round-trip without truncation") {
        const fs::path p = make_tmp_dir() / "large_perms.json";
        Settings s;
        for (int i = 0; i < 50; ++i) {
            s.permissions_allow.push_back("Bash(cmd" + std::to_string(i) + " *)");
            s.permissions_deny.push_back("Read(/etc/secret" + std::to_string(i) + ")");
        }

        REQUIRE(write_settings(p, s).has_value());
        const auto lr = load_settings(p);
        REQUIRE(lr.has_value());
        CHECK(lr.value().permissions_allow.size() == 50);
        CHECK(lr.value().permissions_deny.size()  == 50);
    }
}
