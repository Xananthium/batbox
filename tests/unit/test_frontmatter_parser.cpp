// tests/unit/test_frontmatter_parser.cpp
// =============================================================================
// doctest suite for batbox::plugins::parse_frontmatter.
//
// Covers:
//   - Basic key:value pairs (string, int, bool)
//   - Quoted string values
//   - Flow-style lists  [a, b, c]
//   - Block-style lists (- item per line)
//   - No frontmatter → returns empty map + whole content as body
//   - Only frontmatter → empty body
//   - Malformed cases: no closing ---, bad key, unterminated quote, bad list
//   - All 9 bundled skill fixture files
//   - Edge cases: empty content, only ---, CRLF line endings
//
// Build standalone (from repo root):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_frontmatter_parser.cpp \
//       src/plugins/FrontmatterParser.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libfmt.a \
//       -o /tmp/test_fp && /tmp/test_fp
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/plugins/FrontmatterParser.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace batbox;
using namespace batbox::plugins;

namespace {

// Convenience: load a fixture file relative to the project root.
// Returns empty string if the file does not exist (tests that use it will skip
// via REQUIRE).
[[nodiscard]] std::string load_fixture(const std::string& rel_path) {
    namespace fs = std::filesystem;

    // __FILE__ is the absolute path to this source file at compile time.
    // Walk up from tests/unit/ (2 levels) to reach the project root.
    fs::path this_file = fs::path(__FILE__);
    fs::path project_root = this_file.parent_path()  // tests/unit
                                     .parent_path()  // tests
                                     .parent_path(); // project root
    fs::path fixture = project_root / rel_path;
    if (!fs::exists(fixture)) return {};

    std::ifstream f(fixture);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

// ============================================================================
// TEST SUITE 1: Happy-path scalars
// ============================================================================
TEST_SUITE("parse_frontmatter — scalars") {

    TEST_CASE("basic key:value string pair") {
        auto res = parse_frontmatter("---\nname: foo\n---\nbody");
        REQUIRE(res.has_value());
        auto& [meta, body] = res.value();
        REQUIRE(meta.count("name"));
        CHECK(meta.at("name").get<std::string>() == "foo");
        CHECK(body == "body");
    }

    TEST_CASE("multiple string pairs") {
        auto res = parse_frontmatter("---\nname: my-skill\ndescription: does stuff\n---\nbody text");
        REQUIRE(res.has_value());
        auto& [meta, body] = res.value();
        CHECK(meta.at("name").get<std::string>() == "my-skill");
        CHECK(meta.at("description").get<std::string>() == "does stuff");
        CHECK(body == "body text");
    }

    TEST_CASE("quoted string strips outer double quotes") {
        auto res = parse_frontmatter("---\nname: \"hello world\"\n---\n");
        REQUIRE(res.has_value());
        CHECK(res.value().first.at("name").get<std::string>() == "hello world");
    }

    TEST_CASE("quoted string with embedded colon") {
        auto res = parse_frontmatter("---\ndescription: \"value: with colon\"\n---\n");
        REQUIRE(res.has_value());
        CHECK(res.value().first.at("description").get<std::string>() == "value: with colon");
    }

    TEST_CASE("quoted string with escaped double-quote (\"\")") {
        auto res = parse_frontmatter("---\nname: \"say \"\"hello\"\"\"\n---\n");
        REQUIRE(res.has_value());
        CHECK(res.value().first.at("name").get<std::string>() == "say \"hello\"");
    }

    TEST_CASE("integer value") {
        auto res = parse_frontmatter("---\nmax_tokens: 4096\n---\n");
        REQUIRE(res.has_value());
        CHECK(res.value().first.at("max_tokens").get<std::int64_t>() == 4096);
    }

    TEST_CASE("negative integer value") {
        auto res = parse_frontmatter("---\noffset: -10\n---\n");
        REQUIRE(res.has_value());
        CHECK(res.value().first.at("offset").get<std::int64_t>() == -10);
    }

    TEST_CASE("boolean true") {
        auto res = parse_frontmatter("---\nenabled: true\n---\n");
        REQUIRE(res.has_value());
        CHECK(res.value().first.at("enabled").get<bool>() == true);
    }

    TEST_CASE("boolean false") {
        auto res = parse_frontmatter("---\ndisabled: false\n---\n");
        REQUIRE(res.has_value());
        CHECK(res.value().first.at("disabled").get<bool>() == false);
    }

    TEST_CASE("boolean yes") {
        auto res = parse_frontmatter("---\nactive: yes\n---\n");
        REQUIRE(res.has_value());
        CHECK(res.value().first.at("active").get<bool>() == true);
    }

    TEST_CASE("boolean no") {
        auto res = parse_frontmatter("---\nverbose: no\n---\n");
        REQUIRE(res.has_value());
        CHECK(res.value().first.at("verbose").get<bool>() == false);
    }

    TEST_CASE("quoted integer stays string") {
        auto res = parse_frontmatter("---\nversion: \"1.0\"\n---\n");
        REQUIRE(res.has_value());
        CHECK(res.value().first.at("version").is_string());
        CHECK(res.value().first.at("version").get<std::string>() == "1.0");
    }
}

// ============================================================================
// TEST SUITE 2: Lists
// ============================================================================
TEST_SUITE("parse_frontmatter — lists") {

    TEST_CASE("flow-style list [a, b, c]") {
        auto res = parse_frontmatter("---\nallowed_tools: [Read, Write, Bash]\n---\n");
        REQUIRE(res.has_value());
        auto& v = res.value().first.at("allowed_tools");
        REQUIRE(v.is_array());
        REQUIRE(v.size() == 3);
        CHECK(v[0].get<std::string>() == "Read");
        CHECK(v[1].get<std::string>() == "Write");
        CHECK(v[2].get<std::string>() == "Bash");
    }

    TEST_CASE("flow-style list with quoted items") {
        auto res = parse_frontmatter("---\ntags: [\"foo bar\", baz]\n---\n");
        REQUIRE(res.has_value());
        auto& v = res.value().first.at("tags");
        REQUIRE(v.is_array());
        REQUIRE(v.size() == 2);
        CHECK(v[0].get<std::string>() == "foo bar");
        CHECK(v[1].get<std::string>() == "baz");
    }

    TEST_CASE("empty flow list []") {
        auto res = parse_frontmatter("---\ntools: []\n---\n");
        REQUIRE(res.has_value());
        auto& v = res.value().first.at("tools");
        REQUIRE(v.is_array());
        CHECK(v.empty());
    }

    TEST_CASE("block-style list") {
        const char* src =
            "---\n"
            "allowed_tools:\n"
            "  - Read\n"
            "  - Write\n"
            "  - Bash\n"
            "---\nbody";
        auto res = parse_frontmatter(src);
        REQUIRE(res.has_value());
        auto& v = res.value().first.at("allowed_tools");
        REQUIRE(v.is_array());
        REQUIRE(v.size() == 3);
        CHECK(v[0].get<std::string>() == "Read");
        CHECK(v[1].get<std::string>() == "Write");
        CHECK(v[2].get<std::string>() == "Bash");
        CHECK(res.value().second == "body");
    }

    TEST_CASE("block-style list followed by another key") {
        const char* src =
            "---\n"
            "tools:\n"
            "  - Read\n"
            "  - Write\n"
            "name: after-list\n"
            "---\n";
        auto res = parse_frontmatter(src);
        REQUIRE(res.has_value());
        auto& meta = res.value().first;
        REQUIRE(meta.at("tools").is_array());
        CHECK(meta.at("tools").size() == 2);
        CHECK(meta.at("name").get<std::string>() == "after-list");
    }

    TEST_CASE("single-item flow list") {
        auto res = parse_frontmatter("---\ntools: [Read]\n---\n");
        REQUIRE(res.has_value());
        auto& v = res.value().first.at("tools");
        REQUIRE(v.is_array());
        CHECK(v.size() == 1);
        CHECK(v[0].get<std::string>() == "Read");
    }
}

// ============================================================================
// TEST SUITE 3: No frontmatter / missing delimiter cases
// ============================================================================
TEST_SUITE("parse_frontmatter — no frontmatter") {

    TEST_CASE("content with no opening --- returns empty meta + whole content") {
        const char* src = "# Just a heading\n\nSome markdown.\n";
        auto res = parse_frontmatter(src);
        REQUIRE(res.has_value());
        CHECK(res.value().first.empty());
        CHECK(res.value().second == src);
    }

    TEST_CASE("empty string returns empty meta + empty body") {
        auto res = parse_frontmatter("");
        REQUIRE(res.has_value());
        CHECK(res.value().first.empty());
        CHECK(res.value().second.empty());
    }

    TEST_CASE("--- not at start is treated as no frontmatter") {
        const char* src = "\n---\nname: foo\n---\n";
        auto res = parse_frontmatter(src);
        REQUIRE(res.has_value());
        CHECK(res.value().first.empty());
        CHECK(res.value().second == src);
    }

    TEST_CASE("--- followed by non-newline is treated as no frontmatter") {
        const char* src = "--- not a delimiter\nsome content";
        auto res = parse_frontmatter(src);
        REQUIRE(res.has_value());
        CHECK(res.value().first.empty());
        CHECK(res.value().second == src);
    }
}

// ============================================================================
// TEST SUITE 4: Only frontmatter (empty body)
// ============================================================================
TEST_SUITE("parse_frontmatter — only frontmatter") {

    TEST_CASE("only frontmatter, closing --- at end with newline") {
        auto res = parse_frontmatter("---\nname: meta-only\n---\n");
        REQUIRE(res.has_value());
        CHECK(res.value().first.at("name").get<std::string>() == "meta-only");
        CHECK(res.value().second.empty());
    }

    TEST_CASE("only frontmatter, closing --- at end without trailing newline") {
        auto res = parse_frontmatter("---\nname: meta-only\n---");
        REQUIRE(res.has_value());
        CHECK(res.value().first.at("name").get<std::string>() == "meta-only");
        CHECK(res.value().second.empty());
    }

    TEST_CASE("empty frontmatter block (no keys)") {
        auto res = parse_frontmatter("---\n---\nbody");
        REQUIRE(res.has_value());
        CHECK(res.value().first.empty());
        CHECK(res.value().second == "body");
    }
}

// ============================================================================
// TEST SUITE 5: Malformed cases → Err
// ============================================================================
TEST_SUITE("parse_frontmatter — malformed") {

    TEST_CASE("no closing --- returns error") {
        auto res = parse_frontmatter("---\nname: foo\n");
        REQUIRE_FALSE(res.has_value());
        // Error message should contain line:col prefix
        CHECK(!res.error().empty());
        CHECK(res.error().find(':') != std::string::npos);
    }

    TEST_CASE("no closing --- with multiple keys returns error") {
        auto res = parse_frontmatter("---\nname: foo\ndescription: bar\n");
        REQUIRE_FALSE(res.has_value());
        CHECK(!res.error().empty());
    }

    TEST_CASE("line without colon returns error") {
        auto res = parse_frontmatter("---\njust a line\n---\n");
        REQUIRE_FALSE(res.has_value());
        CHECK(res.error().find("no ':'") != std::string::npos);
    }

    TEST_CASE("unterminated quoted string returns error") {
        auto res = parse_frontmatter("---\nname: \"unterminated\n---\n");
        REQUIRE_FALSE(res.has_value());
        CHECK(!res.error().empty());
    }

    TEST_CASE("empty key returns error") {
        auto res = parse_frontmatter("---\n: value\n---\n");
        REQUIRE_FALSE(res.has_value());
        CHECK(res.error().find("empty key") != std::string::npos);
    }

    TEST_CASE("unterminated flow list returns error") {
        auto res = parse_frontmatter("---\ntools: [Read, Write\n---\n");
        REQUIRE_FALSE(res.has_value());
        CHECK(!res.error().empty());
    }

    TEST_CASE("error message format is line:col: message") {
        auto res = parse_frontmatter("---\njust a line\n---\n");
        REQUIRE_FALSE(res.has_value());
        // Must match pattern digits:digits: text
        const std::string& e = res.error();
        CHECK(e.size() > 4);
        // First char must be a digit
        CHECK(std::isdigit(static_cast<unsigned char>(e[0])));
        // Must contain at least one ':'
        auto first_colon = e.find(':');
        REQUIRE(first_colon != std::string::npos);
        auto second_colon = e.find(':', first_colon + 1);
        REQUIRE(second_colon != std::string::npos);
    }
}

// ============================================================================
// TEST SUITE 6: Body extraction
// ============================================================================
TEST_SUITE("parse_frontmatter — body extraction") {

    TEST_CASE("body preserves newlines and multi-paragraph content") {
        const char* src =
            "---\nname: skill\n---\n"
            "# Heading\n\nParagraph one.\n\nParagraph two.\n";
        auto res = parse_frontmatter(src);
        REQUIRE(res.has_value());
        CHECK(res.value().second == "# Heading\n\nParagraph one.\n\nParagraph two.\n");
    }

    TEST_CASE("body can itself contain ---") {
        const char* src =
            "---\nname: skill\n---\n"
            "Some text\n---\nNot a delimiter here\n";
        auto res = parse_frontmatter(src);
        REQUIRE(res.has_value());
        CHECK(res.value().second == "Some text\n---\nNot a delimiter here\n");
    }

    TEST_CASE("body after empty frontmatter is the entire remaining content") {
        const char* src = "---\n---\nEverything after.";
        auto res = parse_frontmatter(src);
        REQUIRE(res.has_value());
        CHECK(res.value().second == "Everything after.");
    }
}

// ============================================================================
// TEST SUITE 7: CRLF line endings
// ============================================================================
TEST_SUITE("parse_frontmatter — CRLF line endings") {

    TEST_CASE("CRLF throughout is handled gracefully") {
        // Build a frontmatter block with \r\n
        std::string src = "---\r\nname: crlf-skill\r\ndescription: test\r\n---\r\nbody\r\n";
        auto res = parse_frontmatter(src);
        REQUIRE(res.has_value());
        CHECK(res.value().first.at("name").get<std::string>() == "crlf-skill");
        CHECK(res.value().first.at("description").get<std::string>() == "test");
    }
}

// ============================================================================
// TEST SUITE 8: Comment lines inside frontmatter
// ============================================================================
TEST_SUITE("parse_frontmatter — comment lines") {

    TEST_CASE("# comment lines are ignored") {
        const char* src =
            "---\n"
            "# This is a comment\n"
            "name: foo\n"
            "# Another comment\n"
            "description: bar\n"
            "---\nbody";
        auto res = parse_frontmatter(src);
        REQUIRE(res.has_value());
        auto& meta = res.value().first;
        CHECK(meta.at("name").get<std::string>() == "foo");
        CHECK(meta.at("description").get<std::string>() == "bar");
        CHECK(meta.count("# This is a comment") == 0);
    }
}

// ============================================================================
// TEST SUITE 9: Duplicate keys
// ============================================================================
TEST_SUITE("parse_frontmatter — duplicate keys") {

    TEST_CASE("last value wins for duplicate keys") {
        auto res = parse_frontmatter("---\nname: first\nname: second\n---\n");
        REQUIRE(res.has_value());
        CHECK(res.value().first.at("name").get<std::string>() == "second");
    }
}

// ============================================================================
// TEST SUITE 10: Fixture files (real-world skill format)
// ============================================================================
TEST_SUITE("parse_frontmatter — fixture files") {

    TEST_CASE("basic.md fixture") {
        std::string content = load_fixture("tests/fixtures/skills/basic.md");
        REQUIRE(!content.empty());
        auto res = parse_frontmatter(content);
        REQUIRE(res.has_value());
        auto& meta = res.value().first;
        CHECK(meta.at("name").get<std::string>() == "basic-skill");
        CHECK(meta.at("description").get<std::string>() == "A basic test skill for unit testing");
        CHECK(!res.value().second.empty()); // has body
    }

    TEST_CASE("with_list.md fixture (flow-style list)") {
        std::string content = load_fixture("tests/fixtures/skills/with_list.md");
        REQUIRE(!content.empty());
        auto res = parse_frontmatter(content);
        REQUIRE(res.has_value());
        auto& meta = res.value().first;
        CHECK(meta.at("name").get<std::string>() == "tool-skill");
        REQUIRE(meta.at("allowed_tools").is_array());
        CHECK(meta.at("allowed_tools").size() == 3);
        CHECK(meta.at("enabled").get<bool>() == true);
    }

    TEST_CASE("block_list.md fixture (block-style list)") {
        std::string content = load_fixture("tests/fixtures/skills/block_list.md");
        REQUIRE(!content.empty());
        auto res = parse_frontmatter(content);
        REQUIRE(res.has_value());
        auto& meta = res.value().first;
        REQUIRE(meta.at("allowed_tools").is_array());
        CHECK(meta.at("allowed_tools").size() == 4);
        CHECK(meta.at("allowed_tools")[3].get<std::string>() == "Edit");
    }

    TEST_CASE("quoted_values.md fixture") {
        std::string content = load_fixture("tests/fixtures/skills/quoted_values.md");
        REQUIRE(!content.empty());
        auto res = parse_frontmatter(content);
        REQUIRE(res.has_value());
        auto& meta = res.value().first;
        CHECK(meta.at("name").get<std::string>() == "quoted skill name");
        CHECK(meta.at("description").get<std::string>() == "A description with: colon inside quotes");
    }

    TEST_CASE("no_frontmatter.md fixture") {
        std::string content = load_fixture("tests/fixtures/skills/no_frontmatter.md");
        REQUIRE(!content.empty());
        auto res = parse_frontmatter(content);
        REQUIRE(res.has_value());
        CHECK(res.value().first.empty());
        CHECK(!res.value().second.empty()); // whole file is body
    }

    TEST_CASE("only_frontmatter.md fixture (empty body)") {
        std::string content = load_fixture("tests/fixtures/skills/only_frontmatter.md");
        REQUIRE(!content.empty());
        auto res = parse_frontmatter(content);
        REQUIRE(res.has_value());
        auto& meta = res.value().first;
        CHECK(meta.at("name").get<std::string>() == "only-meta");
        // Body is empty (just a newline or nothing after closing ---)
        {
            const auto& body = res.value().second;
            bool body_is_whitespace_only = body.empty() ||
                body.find_first_not_of("\r\n") == std::string::npos;
            CHECK(body_is_whitespace_only);
        }
    }

    TEST_CASE("bool_and_int.md fixture") {
        std::string content = load_fixture("tests/fixtures/skills/bool_and_int.md");
        REQUIRE(!content.empty());
        auto res = parse_frontmatter(content);
        REQUIRE(res.has_value());
        auto& meta = res.value().first;
        CHECK(meta.at("enabled").get<bool>() == true);
        CHECK(meta.at("disabled").get<bool>() == false);
        CHECK(meta.at("priority").get<std::int64_t>() == 42);
        CHECK(meta.at("max_tokens").get<std::int64_t>() == 4096);
        CHECK(meta.at("debug").get<bool>() == true);
        CHECK(meta.at("verbose").get<bool>() == false);
    }

    TEST_CASE("empty_list.md fixture") {
        std::string content = load_fixture("tests/fixtures/skills/empty_list.md");
        REQUIRE(!content.empty());
        auto res = parse_frontmatter(content);
        REQUIRE(res.has_value());
        auto& v = res.value().first.at("allowed_tools");
        REQUIRE(v.is_array());
        CHECK(v.empty());
    }

    TEST_CASE("real_world_remember.md fixture (R5 mitigation: real claude-code skill)") {
        std::string content = load_fixture("tests/fixtures/skills/real_world_remember.md");
        REQUIRE(!content.empty());
        auto res = parse_frontmatter(content);
        REQUIRE(res.has_value());
        auto& meta = res.value().first;
        CHECK(meta.at("name").get<std::string>() == "remember");
        REQUIRE(meta.at("allowed_tools").is_array());
        CHECK(meta.at("allowed_tools").size() == 3);
        CHECK(meta.at("model").get<std::string>() == "claude-opus-4-5");
        CHECK(meta.at("enabled").get<bool>() == true);
        CHECK(!res.value().second.empty()); // has markdown body
    }

    TEST_CASE("real_world_debug.md fixture (R5 mitigation: real claude-code skill)") {
        std::string content = load_fixture("tests/fixtures/skills/real_world_debug.md");
        REQUIRE(!content.empty());
        auto res = parse_frontmatter(content);
        REQUIRE(res.has_value());
        auto& meta = res.value().first;
        CHECK(meta.at("name").get<std::string>() == "debug");
        REQUIRE(meta.at("allowed_tools").is_array());
        CHECK(meta.at("allowed_tools").size() == 6);
        CHECK(!res.value().second.empty());
    }
}
