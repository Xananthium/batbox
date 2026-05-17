// tests/unit/test_env_loader.cpp
// ---------------------------------------------------------------------------
// doctest suite for batbox::config::load_env_file and helpers.
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/config/EnvLoader.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace batbox::config;

// ---------------------------------------------------------------------------
// Helper: write a temporary .env file and return its path.
// The caller is responsible for removing the file when done.
// ---------------------------------------------------------------------------
static fs::path write_tmp_env(const std::string& contents) {
    const auto path = fs::temp_directory_path() / "batbox_test_env_loader.env";
    std::ofstream f(path, std::ios::trunc);
    f << contents;
    return path;
}

// ---------------------------------------------------------------------------
// TEST SUITE 1: Empty / missing file
// ---------------------------------------------------------------------------
TEST_SUITE("EnvLoader — empty and missing file") {

    TEST_CASE("empty file returns empty map") {
        const auto p = write_tmp_env("");
        const auto r = load_env_file(p);
        REQUIRE(r.has_value());
        CHECK(r.value().empty());
        fs::remove(p);
    }

    TEST_CASE("file with only blank lines and comments returns empty map") {
        const auto p = write_tmp_env("# comment\n   \n\t\n# another comment\n");
        const auto r = load_env_file(p);
        REQUIRE(r.has_value());
        CHECK(r.value().empty());
        fs::remove(p);
    }

    TEST_CASE("missing file returns error Result") {
        const auto r = load_env_file("/tmp/batbox_nonexistent_file_XXXXXX.env");
        CHECK_FALSE(r.has_value());
        CHECK_FALSE(r.error().empty());
    }
}

// ---------------------------------------------------------------------------
// TEST SUITE 2: Simple key=value
// ---------------------------------------------------------------------------
TEST_SUITE("EnvLoader — simple key=value") {

    TEST_CASE("single key=value") {
        const auto p = write_tmp_env("HELLO=world\n");
        const auto r = load_env_file(p);
        REQUIRE(r.has_value());
        CHECK(r.value().at("HELLO") == "world");
        fs::remove(p);
    }

    TEST_CASE("multiple key=value pairs") {
        const auto p = write_tmp_env("FOO=bar\nBAZ=qux\n");
        const auto r = load_env_file(p);
        REQUIRE(r.has_value());
        const auto& m = r.value();
        CHECK(m.at("FOO") == "bar");
        CHECK(m.at("BAZ") == "qux");
        fs::remove(p);
    }

    TEST_CASE("trailing whitespace stripped from unquoted value") {
        const auto p = write_tmp_env("KEY=value   \n");
        const auto r = load_env_file(p);
        REQUIRE(r.has_value());
        CHECK(r.value().at("KEY") == "value");
        fs::remove(p);
    }

    TEST_CASE("key with whitespace around equals") {
        const auto p = write_tmp_env("MY_KEY = hello\n");
        const auto r = load_env_file(p);
        REQUIRE(r.has_value());
        CHECK(r.value().at("MY_KEY") == "hello");
        fs::remove(p);
    }

    TEST_CASE("value can be empty") {
        const auto p = write_tmp_env("EMPTY=\n");
        const auto r = load_env_file(p);
        REQUIRE(r.has_value());
        REQUIRE(r.value().count("EMPTY") == 1);
        CHECK(r.value().at("EMPTY") == "");
        fs::remove(p);
    }

    TEST_CASE("export prefix is stripped") {
        const auto p = write_tmp_env("export MY_VAR=exported\n");
        const auto r = load_env_file(p);
        REQUIRE(r.has_value());
        CHECK(r.value().at("MY_VAR") == "exported");
        fs::remove(p);
    }
}

// ---------------------------------------------------------------------------
// TEST SUITE 3: Quoted values
// ---------------------------------------------------------------------------
TEST_SUITE("EnvLoader — quoted values") {

    TEST_CASE("double-quoted value strips outer quotes") {
        const auto p = write_tmp_env("KEY=\"quoted value\"\n");
        const auto r = load_env_file(p);
        REQUIRE(r.has_value());
        CHECK(r.value().at("KEY") == "quoted value");
        fs::remove(p);
    }

    TEST_CASE("double-quoted value preserves internal spaces") {
        const auto p = write_tmp_env("GREETING=\"hello   world\"\n");
        const auto r = load_env_file(p);
        REQUIRE(r.has_value());
        CHECK(r.value().at("GREETING") == "hello   world");
        fs::remove(p);
    }

    TEST_CASE("single-quoted value strips outer quotes, no escape processing") {
        const auto p = write_tmp_env("RAW='no\\nescaping'\n");
        const auto r = load_env_file(p);
        REQUIRE(r.has_value());
        CHECK(r.value().at("RAW") == "no\\nescaping");
        fs::remove(p);
    }

    TEST_CASE("double-quoted value: \\n escape becomes newline") {
        const auto p = write_tmp_env("MULTI=\"line1\\nline2\"\n");
        const auto r = load_env_file(p);
        REQUIRE(r.has_value());
        CHECK(r.value().at("MULTI") == "line1\nline2");
        fs::remove(p);
    }

    TEST_CASE("double-quoted value: \\t escape becomes tab") {
        const auto p = write_tmp_env("TABBED=\"col1\\tcol2\"\n");
        const auto r = load_env_file(p);
        REQUIRE(r.has_value());
        CHECK(r.value().at("TABBED") == "col1\tcol2");
        fs::remove(p);
    }

    TEST_CASE("double-quoted value: \\\" escape becomes literal quote") {
        const auto p = write_tmp_env("QUOTED=\"say \\\"hello\\\"\"\n");
        const auto r = load_env_file(p);
        REQUIRE(r.has_value());
        CHECK(r.value().at("QUOTED") == "say \"hello\"");
        fs::remove(p);
    }

    TEST_CASE("double-quoted value: \\\\ escape becomes single backslash") {
        const auto p = write_tmp_env("SLASH=\"a\\\\b\"\n");
        const auto r = load_env_file(p);
        REQUIRE(r.has_value());
        CHECK(r.value().at("SLASH") == "a\\b");
        fs::remove(p);
    }

    TEST_CASE("double-quoted value: invalid escape skips line with warning") {
        const auto p = write_tmp_env("BAD=\"value\\q\"\n");
        const auto r = load_env_file(p);
        REQUIRE(r.has_value());
        // Malformed line is skipped; key must not appear in map.
        CHECK(r.value().count("BAD") == 0);
        fs::remove(p);
    }
}

// ---------------------------------------------------------------------------
// TEST SUITE 4: Comments
// ---------------------------------------------------------------------------
TEST_SUITE("EnvLoader — comments") {

    TEST_CASE("full-line comment is skipped") {
        const auto p = write_tmp_env("# this is a comment\nKEY=val\n");
        const auto r = load_env_file(p);
        REQUIRE(r.has_value());
        const auto& m = r.value();
        CHECK(m.size() == 1);
        CHECK(m.at("KEY") == "val");
        fs::remove(p);
    }

    TEST_CASE("inline comment (space+#) is stripped from unquoted value") {
        const auto p = write_tmp_env("KEY=hello # this is inline\n");
        const auto r = load_env_file(p);
        REQUIRE(r.has_value());
        CHECK(r.value().at("KEY") == "hello");
        fs::remove(p);
    }

    TEST_CASE("# inside double-quoted value is NOT treated as comment") {
        const auto p = write_tmp_env("KEY=\"hello # world\"\n");
        const auto r = load_env_file(p);
        REQUIRE(r.has_value());
        CHECK(r.value().at("KEY") == "hello # world");
        fs::remove(p);
    }
}

// ---------------------------------------------------------------------------
// TEST SUITE 5: Variable substitution
// ---------------------------------------------------------------------------
TEST_SUITE("EnvLoader — variable substitution") {

    TEST_CASE("${HOME} is expanded from process env") {
        // HOME is always set in a Unix environment; use it as a stable ref.
        const char* home = std::getenv("HOME");
        REQUIRE(home != nullptr);

        const auto p = write_tmp_env("MY_PATH=${HOME}/subdir\n");
        const auto r = load_env_file(p);
        REQUIRE(r.has_value());
        const std::string expected = std::string(home) + "/subdir";
        CHECK(r.value().at("MY_PATH") == expected);
        fs::remove(p);
    }

    TEST_CASE("unknown variable expands to empty string") {
        const auto p = write_tmp_env("KEY=prefix${_BATBOX_NONEXISTENT_VAR_}suffix\n");
        const auto r = load_env_file(p);
        REQUIRE(r.has_value());
        CHECK(r.value().at("KEY") == "prefixsuffix");
        fs::remove(p);
    }

    TEST_CASE("substitution also works inside double-quoted values") {
        const char* home = std::getenv("HOME");
        REQUIRE(home != nullptr);

        const auto p = write_tmp_env("DQPATH=\"${HOME}/subdir\"\n");
        const auto r = load_env_file(p);
        REQUIRE(r.has_value());
        const std::string expected = std::string(home) + "/subdir";
        CHECK(r.value().at("DQPATH") == expected);
        fs::remove(p);
    }

    TEST_CASE("single-quoted value: no substitution") {
        const auto p = write_tmp_env("SQPATH='${HOME}/literal'\n");
        const auto r = load_env_file(p);
        REQUIRE(r.has_value());
        CHECK(r.value().at("SQPATH") == "${HOME}/literal");
        fs::remove(p);
    }
}

// ---------------------------------------------------------------------------
// TEST SUITE 6: Tilde expansion
// ---------------------------------------------------------------------------
TEST_SUITE("EnvLoader — tilde expansion") {

    TEST_CASE("tilde prefix in unquoted value is expanded") {
        const char* home = std::getenv("HOME");
        REQUIRE(home != nullptr);

        const auto p = write_tmp_env("CONF=~/.batbox/config\n");
        const auto r = load_env_file(p);
        REQUIRE(r.has_value());
        const std::string expected = std::string(home) + "/.batbox/config";
        CHECK(r.value().at("CONF") == expected);
        fs::remove(p);
    }

    TEST_CASE("bare tilde expands to home directory") {
        const char* home = std::getenv("HOME");
        REQUIRE(home != nullptr);

        const auto p = write_tmp_env("HOME_DIR=~\n");
        const auto r = load_env_file(p);
        REQUIRE(r.has_value());
        CHECK(r.value().at("HOME_DIR") == std::string(home));
        fs::remove(p);
    }
}

// ---------------------------------------------------------------------------
// TEST SUITE 7: Malformed lines
// ---------------------------------------------------------------------------
TEST_SUITE("EnvLoader — malformed lines") {

    TEST_CASE("line without '=' is skipped; parsing continues") {
        const auto p = write_tmp_env("NOEQUALSSIGN\nGOOD=value\n");
        const auto r = load_env_file(p);
        REQUIRE(r.has_value());
        const auto& m = r.value();
        // Bad line skipped; good line retained.
        CHECK(m.count("NOEQUALSSIGN") == 0);
        CHECK(m.at("GOOD") == "value");
        fs::remove(p);
    }

    TEST_CASE("multiple malformed lines; only valid pairs survive") {
        const auto p = write_tmp_env("A=1\nBADLINE\nB=2\nBADLINE2\nC=3\n");
        const auto r = load_env_file(p);
        REQUIRE(r.has_value());
        const auto& m = r.value();
        CHECK(m.at("A") == "1");
        CHECK(m.at("B") == "2");
        CHECK(m.at("C") == "3");
        CHECK(m.size() == 3);
        fs::remove(p);
    }
}

// ---------------------------------------------------------------------------
// TEST SUITE 8: BOM handling
// ---------------------------------------------------------------------------
TEST_SUITE("EnvLoader — BOM handling") {

    TEST_CASE("UTF-8 BOM on first line is silently stripped") {
        // Write file with BOM prefix manually.
        const auto path = fs::temp_directory_path() / "batbox_test_bom.env";
        {
            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            // UTF-8 BOM: 0xEF 0xBB 0xBF
            const char bom[3] = {'\xEF', '\xBB', '\xBF'};
            f.write(bom, 3);
            f << "BOM_KEY=bom_value\n";
        }
        const auto r = load_env_file(path);
        REQUIRE(r.has_value());
        CHECK(r.value().at("BOM_KEY") == "bom_value");
        fs::remove(path);
    }
}

// ---------------------------------------------------------------------------
// TEST SUITE 9: get() helper
// ---------------------------------------------------------------------------
TEST_SUITE("EnvLoader — get() helper") {

    TEST_CASE("get() returns value when key exists") {
        const EnvMap m{{"FOO", "bar"}};
        CHECK(get(m, "FOO") == "bar");
    }

    TEST_CASE("get() returns default when key absent") {
        const EnvMap m{{"FOO", "bar"}};
        CHECK(get(m, "MISSING", "default_val") == "default_val");
    }

    TEST_CASE("get() returns empty string when key absent and no default provided") {
        const EnvMap m{};
        CHECK(get(m, "MISSING") == "");
    }
}

// ---------------------------------------------------------------------------
// TEST SUITE 10: merge_with_process_env()
// ---------------------------------------------------------------------------
TEST_SUITE("EnvLoader — merge_with_process_env()") {

    TEST_CASE("process_env_wins=false: file value wins over process env") {
        // Set an env var we control, then confirm file value wins.
        ::setenv("BATBOX_TEST_MERGE_VAR", "from_process", 1);

        EnvMap m;
        m["BATBOX_TEST_MERGE_VAR"] = "from_file";
        merge_with_process_env(m, /*process_env_wins=*/false);

        // File value must survive.
        CHECK(m.at("BATBOX_TEST_MERGE_VAR") == "from_file");

        ::unsetenv("BATBOX_TEST_MERGE_VAR");
    }

    TEST_CASE("process_env_wins=true: process env wins over file value") {
        ::setenv("BATBOX_TEST_MERGE_WIN", "from_process", 1);

        EnvMap m;
        m["BATBOX_TEST_MERGE_WIN"] = "from_file";
        merge_with_process_env(m, /*process_env_wins=*/true);

        CHECK(m.at("BATBOX_TEST_MERGE_WIN") == "from_process");

        ::unsetenv("BATBOX_TEST_MERGE_WIN");
    }

    TEST_CASE("process_env_wins=false: process env keys not in map are added") {
        ::setenv("BATBOX_TEST_MERGE_NEW", "new_val", 1);

        EnvMap m;
        merge_with_process_env(m, /*process_env_wins=*/false);

        CHECK(m.at("BATBOX_TEST_MERGE_NEW") == "new_val");

        ::unsetenv("BATBOX_TEST_MERGE_NEW");
    }
}
