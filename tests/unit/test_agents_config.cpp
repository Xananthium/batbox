// tests/unit/test_agents_config.cpp
// ---------------------------------------------------------------------------
// doctest suite for batbox::config::load_agents_config.
//
// Coverage:
//   1. Missing file → empty map (not an error)
//   2. Empty JSON object → empty map
//   3. Single agent override entry
//   4. Multiple agent override entries
//   5. Malformed JSON → Err
//   6. Top-level array (not an object) → Err
//   7. Top-level string (not an object) → Err
//   8. Mixed valid + non-string value → non-string skipped, string retained
//   9. Unreadable file error message contains path
//  10. Large realistic map (many entries)
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/config/AgentsConfig.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace batbox::config;

// ---------------------------------------------------------------------------
// Helper: write a temporary JSON file and return its path.
// ---------------------------------------------------------------------------
static fs::path write_tmp_json(const std::string& contents,
                               const std::string& suffix = "agents_config.json") {
    const auto path = fs::temp_directory_path() / ("batbox_test_" + suffix);
    std::ofstream f(path, std::ios::trunc);
    f << contents;
    return path;
}

// ---------------------------------------------------------------------------
// TEST SUITE 1: Missing file
// ---------------------------------------------------------------------------
TEST_SUITE("AgentsConfig — missing file") {

    TEST_CASE("missing file returns empty map (not an error)") {
        const fs::path path = "/tmp/batbox_nonexistent_agents_XXXXXX.json";
        fs::remove(path); // ensure it really doesn't exist
        const auto r = load_agents_config(path);
        REQUIRE(r.has_value());
        CHECK(r.value().empty());
    }
}

// ---------------------------------------------------------------------------
// TEST SUITE 2: Empty / minimal valid JSON
// ---------------------------------------------------------------------------
TEST_SUITE("AgentsConfig — empty and minimal valid files") {

    TEST_CASE("empty JSON object returns empty map") {
        const auto p = write_tmp_json("{}");
        const auto r = load_agents_config(p);
        REQUIRE(r.has_value());
        CHECK(r.value().empty());
        fs::remove(p);
    }

    TEST_CASE("JSON object with only whitespace content returns empty map") {
        const auto p = write_tmp_json("  {  }  ");
        const auto r = load_agents_config(p);
        REQUIRE(r.has_value());
        CHECK(r.value().empty());
        fs::remove(p);
    }
}

// ---------------------------------------------------------------------------
// TEST SUITE 3: Single and multiple entries
// ---------------------------------------------------------------------------
TEST_SUITE("AgentsConfig — valid entries") {

    TEST_CASE("single entry: agent → model") {
        const auto p = write_tmp_json(R"({"verify":"gpt-4o-mini"})");
        const auto r = load_agents_config(p);
        REQUIRE(r.has_value());
        const auto& m = r.value();
        REQUIRE(m.size() == 1);
        CHECK(m.at("verify") == "gpt-4o-mini");
        fs::remove(p);
    }

    TEST_CASE("multiple entries: verify + debug + default overrides") {
        const auto p = write_tmp_json(R"({
            "verify": "gpt-4o-mini",
            "debug": "o1-preview",
            "refactor": "gpt-4o"
        })");
        const auto r = load_agents_config(p);
        REQUIRE(r.has_value());
        const auto& m = r.value();
        REQUIRE(m.size() == 3);
        CHECK(m.at("verify")   == "gpt-4o-mini");
        CHECK(m.at("debug")    == "o1-preview");
        CHECK(m.at("refactor") == "gpt-4o");
        fs::remove(p);
    }

    TEST_CASE("agent name with hyphens is accepted as a key") {
        const auto p = write_tmp_json(R"({"code-review":"claude-3-opus-20240229"})");
        const auto r = load_agents_config(p);
        REQUIRE(r.has_value());
        CHECK(r.value().at("code-review") == "claude-3-opus-20240229");
        fs::remove(p);
    }

    TEST_CASE("empty string value is a valid (though unusual) model name") {
        const auto p = write_tmp_json(R"({"agent":""})");
        const auto r = load_agents_config(p);
        REQUIRE(r.has_value());
        CHECK(r.value().at("agent") == "");
        fs::remove(p);
    }
}

// ---------------------------------------------------------------------------
// TEST SUITE 4: Malformed JSON
// ---------------------------------------------------------------------------
TEST_SUITE("AgentsConfig — malformed JSON") {

    TEST_CASE("invalid JSON returns error Result") {
        const auto p = write_tmp_json("{not valid json");
        const auto r = load_agents_config(p);
        CHECK_FALSE(r.has_value());
        CHECK_FALSE(r.error().empty());
        fs::remove(p);
    }

    TEST_CASE("error message contains the file path") {
        const auto p = write_tmp_json("{broken");
        const auto r = load_agents_config(p);
        REQUIRE_FALSE(r.has_value());
        // Error message must reference the file that failed.
        CHECK(r.error().find(p.string()) != std::string::npos);
        fs::remove(p);
    }

    TEST_CASE("empty file (not even valid JSON) returns error Result") {
        const auto p = write_tmp_json("");
        const auto r = load_agents_config(p);
        // nlohmann rejects an empty input as invalid JSON.
        CHECK_FALSE(r.has_value());
        fs::remove(p);
    }
}

// ---------------------------------------------------------------------------
// TEST SUITE 5: Wrong JSON type at top level
// ---------------------------------------------------------------------------
TEST_SUITE("AgentsConfig — wrong top-level type") {

    TEST_CASE("top-level JSON array returns error") {
        const auto p = write_tmp_json(R"(["verify","debug"])");
        const auto r = load_agents_config(p);
        CHECK_FALSE(r.has_value());
        CHECK_FALSE(r.error().empty());
        fs::remove(p);
    }

    TEST_CASE("top-level JSON string returns error") {
        const auto p = write_tmp_json(R"("just a string")");
        const auto r = load_agents_config(p);
        CHECK_FALSE(r.has_value());
        fs::remove(p);
    }

    TEST_CASE("top-level JSON number returns error") {
        const auto p = write_tmp_json("42");
        const auto r = load_agents_config(p);
        CHECK_FALSE(r.has_value());
        fs::remove(p);
    }

    TEST_CASE("top-level JSON null returns error") {
        const auto p = write_tmp_json("null");
        const auto r = load_agents_config(p);
        CHECK_FALSE(r.has_value());
        fs::remove(p);
    }
}

// ---------------------------------------------------------------------------
// TEST SUITE 6: Non-string values (mixed entries)
// ---------------------------------------------------------------------------
TEST_SUITE("AgentsConfig — non-string value entries skipped") {

    TEST_CASE("integer value entry is skipped; string entries are retained") {
        const auto p = write_tmp_json(R"({
            "good_agent": "gpt-4o",
            "bad_agent": 42
        })");
        const auto r = load_agents_config(p);
        REQUIRE(r.has_value());
        const auto& m = r.value();
        CHECK(m.size() == 1);
        CHECK(m.at("good_agent") == "gpt-4o");
        CHECK(m.count("bad_agent") == 0);
        fs::remove(p);
    }

    TEST_CASE("boolean value entry is skipped; other entries retained") {
        const auto p = write_tmp_json(R"({
            "verify": "gpt-4o-mini",
            "enabled": true,
            "debug": "o1-preview"
        })");
        const auto r = load_agents_config(p);
        REQUIRE(r.has_value());
        const auto& m = r.value();
        CHECK(m.size() == 2);
        CHECK(m.at("verify") == "gpt-4o-mini");
        CHECK(m.at("debug")  == "o1-preview");
        CHECK(m.count("enabled") == 0);
        fs::remove(p);
    }

    TEST_CASE("nested object value is skipped; string entries retained") {
        const auto p = write_tmp_json(R"({
            "simple": "gpt-4o",
            "nested": {"model": "something"}
        })");
        const auto r = load_agents_config(p);
        REQUIRE(r.has_value());
        const auto& m = r.value();
        CHECK(m.size() == 1);
        CHECK(m.at("simple") == "gpt-4o");
        CHECK(m.count("nested") == 0);
        fs::remove(p);
    }

    TEST_CASE("all non-string values → empty map (still Ok, not Err)") {
        const auto p = write_tmp_json(R"({"a": 1, "b": false, "c": null})");
        const auto r = load_agents_config(p);
        REQUIRE(r.has_value());
        CHECK(r.value().empty());
        fs::remove(p);
    }
}

// ---------------------------------------------------------------------------
// TEST SUITE 7: Large realistic map
// ---------------------------------------------------------------------------
TEST_SUITE("AgentsConfig — large realistic map") {

    TEST_CASE("ten agent overrides all parse correctly") {
        const auto p = write_tmp_json(R"({
            "verify":    "gpt-4o-mini",
            "debug":     "o1-preview",
            "refactor":  "gpt-4o",
            "review":    "claude-3-opus-20240229",
            "docstring": "gpt-3.5-turbo",
            "test":      "gpt-4o",
            "plan":      "o1",
            "search":    "gpt-4o-mini",
            "embed":     "text-embedding-3-small",
            "summarize": "gpt-4o-mini"
        })");
        const auto r = load_agents_config(p);
        REQUIRE(r.has_value());
        const auto& m = r.value();
        CHECK(m.size() == 10);
        CHECK(m.at("verify")    == "gpt-4o-mini");
        CHECK(m.at("debug")     == "o1-preview");
        CHECK(m.at("plan")      == "o1");
        CHECK(m.at("summarize") == "gpt-4o-mini");
        fs::remove(p);
    }
}
