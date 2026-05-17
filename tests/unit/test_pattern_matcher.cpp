// tests/unit/test_pattern_matcher.cpp
// ---------------------------------------------------------------------------
// doctest suite for batbox::permissions PatternMatcher.
//
// Covers:
//   - ToolPattern::parse: valid rules, malformed rules (all error paths)
//   - glob_match: *, **, ?, [abc], [!abc], ranges, literal, edge cases
//   - matches(): positive + negative for Bash, Read, Write, WebFetch, WebSearch
//   - parse_pattern_list(): normal + malformed entries mixed
//   - Realistic patterns from pmdraft.md F10 spec
//
// Build (standalone, no CMake):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_pattern_matcher.cpp \
//       src/permissions/PatternMatcher.cpp \
//       -o /tmp/test_pm && /tmp/test_pm
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/permissions/PatternMatcher.hpp>
#include <batbox/core/Json.hpp>

#include <string>
#include <vector>

using namespace batbox::permissions;
using batbox::Json;

// ===========================================================================
// SUITE 1: ToolPattern::parse
// ===========================================================================
TEST_SUITE("ToolPattern::parse") {

    TEST_CASE("simple valid rule") {
        auto r = ToolPattern::parse("Bash(npm test:*)");
        REQUIRE(r.has_value());
        CHECK(r->tool_name == "bash");
        CHECK(r->arg_glob == "npm test:*");
    }

    TEST_CASE("tool name is lower-cased") {
        auto r = ToolPattern::parse("READ(./src/**)");
        REQUIRE(r.has_value());
        CHECK(r->tool_name == "read");
        CHECK(r->arg_glob == "./src/**");
    }

    TEST_CASE("empty arg glob is valid") {
        auto r = ToolPattern::parse("Write()");
        REQUIRE(r.has_value());
        CHECK(r->tool_name == "write");
        CHECK(r->arg_glob.empty());
    }

    TEST_CASE("nested parens in arg glob") {
        // The rule body may contain '(' as a literal char in the glob
        auto r = ToolPattern::parse("Bash(make test(all))");
        REQUIRE(r.has_value());
        CHECK(r->tool_name == "bash");
        // body is everything between first '(' and last ')'
        CHECK(r->arg_glob == "make test(all)");
    }

    TEST_CASE("WebFetch pattern with URL") {
        auto r = ToolPattern::parse("WebFetch(https://*.github.com/**)");
        REQUIRE(r.has_value());
        CHECK(r->tool_name == "webfetch");
        CHECK(r->arg_glob == "https://*.github.com/**");
    }

    // --- Malformed rules ---

    TEST_CASE("missing '(' returns error") {
        auto r = ToolPattern::parse("Bashnpm");
        CHECK(!r.has_value());
        CHECK(!r.error().empty());
    }

    TEST_CASE("empty tool name returns error") {
        auto r = ToolPattern::parse("(something)");
        CHECK(!r.has_value());
    }

    TEST_CASE("missing closing ')' returns error") {
        auto r = ToolPattern::parse("Bash(npm test:*");
        CHECK(!r.has_value());
    }

    TEST_CASE("empty string returns error") {
        auto r = ToolPattern::parse("");
        CHECK(!r.has_value());
    }

    TEST_CASE("bare '(' with no close returns error") {
        auto r = ToolPattern::parse("Tool(");
        CHECK(!r.has_value());
    }
}

// ===========================================================================
// SUITE 2: glob_match — * (single-component)
// ===========================================================================
TEST_SUITE("glob_match — * wildcard") {

    TEST_CASE("* matches any chars in a path component") {
        CHECK(glob_match("npm test:*", "npm test:unit"));
        CHECK(glob_match("npm test:*", "npm test:"));
        CHECK(glob_match("npm test:*", "npm test:integration"));
    }

    TEST_CASE("* does NOT cross '/'") {
        CHECK(!glob_match("npm test:*", "npm test:a/b"));
        CHECK(!glob_match("*.cpp", "src/foo.cpp"));
    }

    TEST_CASE("* in the middle") {
        CHECK(glob_match("foo*bar", "fooXYZbar"));
        CHECK(glob_match("foo*bar", "foobar"));
        CHECK(!glob_match("foo*bar", "foo/bar"));
    }

    TEST_CASE("leading * matches prefix-free") {
        CHECK(glob_match("*.cpp", "main.cpp"));
        CHECK(glob_match("*.cpp", ".cpp"));
        CHECK(!glob_match("*.cpp", "src/main.cpp"));
    }

    TEST_CASE("multiple * in pattern") {
        CHECK(glob_match("*foo*bar*", "xfooYYbarZ"));
        CHECK(!glob_match("*foo*bar*", "xfoo/barZ")); // * won't cross /
    }

    TEST_CASE("* matches empty string in component") {
        CHECK(glob_match("npm*test", "npmtest"));
    }

    TEST_CASE("literal pattern: no wildcards") {
        CHECK(glob_match("npm test", "npm test"));
        CHECK(!glob_match("npm test", "npm tests"));
        CHECK(!glob_match("npm test", "npm tes"));
    }

    TEST_CASE("empty pattern matches empty text only") {
        CHECK(glob_match("", ""));
        CHECK(!glob_match("", "x"));
    }
}

// ===========================================================================
// SUITE 3: glob_match — ** (path-recursive)
// ===========================================================================
TEST_SUITE("glob_match — ** wildcard") {

    TEST_CASE("** matches multiple path components") {
        CHECK(glob_match("./src/**", "./src/a/b.cpp"));
        CHECK(glob_match("./src/**", "./src/foo.cpp"));
        CHECK(glob_match("./src/**", "./src/"));
        CHECK(glob_match("./src/**", "./src/"));
    }

    TEST_CASE("** matches empty suffix") {
        CHECK(glob_match("./src/**", "./src/"));
        // pattern ./src/** — the ** after / can match empty string
        CHECK(glob_match("./src/**", "./src/a"));
    }

    TEST_CASE("** does NOT match parent directories") {
        CHECK(!glob_match("./src/**", "./other/foo.cpp"));
        CHECK(!glob_match("./src/**", "./srca/b.cpp"));
    }

    TEST_CASE("** in the middle") {
        CHECK(glob_match("./src/**/test_*.cpp", "./src/unit/test_foo.cpp"));
        CHECK(glob_match("./src/**/test_*.cpp", "./src/a/b/c/test_bar.cpp"));
        CHECK(!glob_match("./src/**/test_*.cpp", "./src/unit/foo.cpp"));
    }

    TEST_CASE("** vs * difference is cross-slash") {
        // * would fail here because it can't cross /
        CHECK(!glob_match("./build/*",  "./build/a/b.o"));
        CHECK(glob_match( "./build/**", "./build/a/b.o"));
    }

    TEST_CASE("** alone matches everything") {
        CHECK(glob_match("**", ""));
        CHECK(glob_match("**", "a"));
        CHECK(glob_match("**", "a/b/c"));
    }

    TEST_CASE("** with URL prefix — WebFetch pattern") {
        CHECK(glob_match("https://*.github.com/**", "https://api.github.com/repos/foo/bar"));
        CHECK(glob_match("https://*.github.com/**", "https://raw.github.com/owner/repo/main/README.md"));
        CHECK(!glob_match("https://*.github.com/**", "http://api.github.com/"));
        CHECK(!glob_match("https://*.github.com/**", "https://api.notgithub.com/"));
    }
}

// ===========================================================================
// SUITE 4: glob_match — ? and bracket expressions
// ===========================================================================
TEST_SUITE("glob_match — ? and bracket expressions") {

    TEST_CASE("? matches exactly one character") {
        CHECK(glob_match("fo?", "foo"));
        CHECK(glob_match("fo?", "foX"));
        CHECK(!glob_match("fo?", "fo"));
        CHECK(!glob_match("fo?", "fooo"));
    }

    TEST_CASE("? crosses '/'") {
        // ? matches any single char including /
        CHECK(glob_match("a?b", "a/b"));
    }

    TEST_CASE("[abc] matches any listed char") {
        CHECK(glob_match("[abc]oo", "aoo"));
        CHECK(glob_match("[abc]oo", "boo"));
        CHECK(glob_match("[abc]oo", "coo"));
        CHECK(!glob_match("[abc]oo", "doo"));
    }

    TEST_CASE("[!abc] matches chars NOT in set") {
        CHECK(glob_match("[!abc]oo", "doo"));
        CHECK(glob_match("[!abc]oo", "zoo"));
        CHECK(!glob_match("[!abc]oo", "aoo"));
        CHECK(!glob_match("[!abc]oo", "coo"));
    }

    TEST_CASE("[a-z] range expression") {
        CHECK(glob_match("[a-z]oo", "aoo"));
        CHECK(glob_match("[a-z]oo", "zoo"));
        CHECK(!glob_match("[a-z]oo", "Aoo"));
        CHECK(!glob_match("[a-z]oo", "1oo"));
    }

    TEST_CASE("[0-9] range expression") {
        CHECK(glob_match("v[0-9].*", "v3.1"));
        CHECK(!glob_match("v[0-9].*", "va.1"));
    }

    TEST_CASE("malformed bracket treated as literal or skips gracefully") {
        // No closing ']' — implementation handles gracefully
        // (either literal '[' match or returns false — must not crash)
        bool result = glob_match("[abc", "[abc");  // just don't crash
        (void)result;  // result is implementation-defined; non-crash is the contract
    }
}

// ===========================================================================
// SUITE 5: matches() — Bash tool
// ===========================================================================
TEST_SUITE("matches() — Bash tool") {

    TEST_CASE("Bash(npm test:*) positive matches") {
        Json args = {{"command", "npm test:unit"}};
        CHECK(matches("Bash(npm test:*)", "Bash", args));

        args["command"] = "npm test:integration";
        CHECK(matches("Bash(npm test:*)", "Bash", args));

        args["command"] = "npm test:";
        CHECK(matches("Bash(npm test:*)", "Bash", args));
    }

    TEST_CASE("Bash(npm test:*) negative — different prefix") {
        Json args = {{"command", "npm build"}};
        CHECK(!matches("Bash(npm test:*)", "Bash", args));

        args["command"] = "yarn test:unit";
        CHECK(!matches("Bash(npm test:*)", "Bash", args));
    }

    TEST_CASE("Bash — tool name case-insensitive") {
        Json args = {{"command", "npm test:unit"}};
        CHECK(matches("Bash(npm test:*)", "bash", args));
        CHECK(matches("BASH(npm test:*)", "Bash", args));
        CHECK(matches("bash(npm test:*)", "BASH", args));
    }

    TEST_CASE("Bash — wrong tool name does not match") {
        Json args = {{"command", "npm test:unit"}};
        CHECK(!matches("Read(npm test:*)", "Bash", args));
    }

    TEST_CASE("Bash — missing command field returns false") {
        Json args = {{"other_key", "npm test:unit"}};
        // empty canonical arg won't match "npm test:*"
        CHECK(!matches("Bash(npm test:*)", "Bash", args));
    }

    TEST_CASE("Bash(make **) — recursive wildcard") {
        Json args = {{"command", "make all/debug"}};
        CHECK(matches("Bash(make **)", "Bash", args));
        args["command"] = "make";
        CHECK(!matches("Bash(make **)", "Bash", args));  // "make" != "make " + anything
    }
}

// ===========================================================================
// SUITE 6: matches() — Read / Write / Edit tools
// ===========================================================================
TEST_SUITE("matches() — file-path tools") {

    TEST_CASE("Read(./src/**) positive") {
        Json args = {{"file_path", "./src/a/b.cpp"}};
        CHECK(matches("Read(./src/**)", "Read", args));

        args["file_path"] = "./src/foo.hpp";
        CHECK(matches("Read(./src/**)", "Read", args));
    }

    TEST_CASE("Read(./src/**) negative — outside src") {
        Json args = {{"file_path", "./include/foo.hpp"}};
        CHECK(!matches("Read(./src/**)", "Read", args));

        args["file_path"] = "./srcfoo.cpp";
        CHECK(!matches("Read(./src/**)", "Read", args));
    }

    TEST_CASE("Write(./build/*) matches direct children only") {
        Json args = {{"file_path", "./build/output.o"}};
        CHECK(matches("Write(./build/*)", "Write", args));

        // * does not cross /
        args["file_path"] = "./build/subdir/output.o";
        CHECK(!matches("Write(./build/*)", "Write", args));
    }

    TEST_CASE("Edit uses file_path field") {
        Json args = {{"file_path", "./src/main.cpp"}};
        CHECK(matches("Edit(./src/**)", "Edit", args));
    }

    TEST_CASE("MultiEdit uses file_path field") {
        Json args = {{"file_path", "./src/core/Foo.cpp"}};
        CHECK(matches("MultiEdit(./src/**)", "MultiEdit", args));
    }

    TEST_CASE("file tool with missing file_path") {
        Json args = {{"other", "value"}};
        CHECK(!matches("Read(./src/**)", "Read", args));
    }
}

// ===========================================================================
// SUITE 7: matches() — WebFetch tool
// ===========================================================================
TEST_SUITE("matches() — WebFetch tool") {

    TEST_CASE("WebFetch(https://*.github.com/**) matches subdomain URLs") {
        Json args = {{"url", "https://api.github.com/repos/foo/bar"}};
        CHECK(matches("WebFetch(https://*.github.com/**)", "WebFetch", args));

        args["url"] = "https://raw.github.com/owner/repo/main/README.md";
        CHECK(matches("WebFetch(https://*.github.com/**)", "WebFetch", args));
    }

    TEST_CASE("WebFetch — wrong scheme rejected") {
        Json args = {{"url", "http://api.github.com/"}};
        CHECK(!matches("WebFetch(https://*.github.com/**)", "WebFetch", args));
    }

    TEST_CASE("WebFetch — different domain rejected") {
        Json args = {{"url", "https://api.notgithub.com/repos"}};
        CHECK(!matches("WebFetch(https://*.github.com/**)", "WebFetch", args));
    }

    TEST_CASE("WebFetch — bare github.com (no subdomain) rejected by *") {
        // The * in *.github.com requires at least one char before the dot
        Json args = {{"url", "https://github.com/foo"}};
        CHECK(!matches("WebFetch(https://*.github.com/**)", "WebFetch", args));
    }


    TEST_CASE("WebFetch(https://**) broad allow -- ** crosses path slashes") {
        // ** crosses /, so https://** matches any https URL including with path
        Json args = {{"url", "https://example.com/page"}};
        CHECK(matches("WebFetch(https://**)", "WebFetch", args));

        args["url"] = "http://example.com/page";
        CHECK(!matches("WebFetch(https://**)", "WebFetch", args));
    }

    TEST_CASE("WebFetch(https://*) single * -- host only, no path slash") {
        // * does NOT cross /, so https://* matches https://example.com but not https://example.com/page
        Json args = {{"url", "https://example.com"}};
        CHECK(matches("WebFetch(https://*)", "WebFetch", args));

        args["url"] = "https://example.com/page";
        CHECK(!matches("WebFetch(https://*)", "WebFetch", args));
    }

}
// ===========================================================================
// SUITE 8: matches() — WebSearch tool
// ===========================================================================
TEST_SUITE("matches() — WebSearch tool") {

    TEST_CASE("WebSearch(rust*) positive") {
        Json args = {{"query", "rust async book"}};
        CHECK(matches("WebSearch(rust*)", "WebSearch", args));
    }

    TEST_CASE("WebSearch — wrong field not used") {
        Json args = {{"command", "rust async book"}};
        // field is 'query', not 'command'
        CHECK(!matches("WebSearch(rust*)", "WebSearch", args));
    }
}

// ===========================================================================
// SUITE 9: matches() — unrecognised tool (JSON fallback)
// ===========================================================================
TEST_SUITE("matches() — unrecognised tool JSON fallback") {

    TEST_CASE("unknown tool: glob matches against JSON dump") {
        Json args = {{"custom_field", "some_value"}};
        // The JSON dump will contain "some_value"; a ** pattern should find it
        CHECK(matches("TodoWrite(**)", "TodoWrite", args));
    }

    TEST_CASE("unknown tool: mismatch") {
        Json args = {{"key", "hello"}};
        CHECK(!matches("MyTool(goodbye*)", "MyTool", args));
    }
}

// ===========================================================================
// SUITE 10: parse_pattern_list
// ===========================================================================
TEST_SUITE("parse_pattern_list") {

    TEST_CASE("all valid rules") {
        std::vector<std::string> raw = {
            "Bash(npm test:*)",
            "Read(./src/**)",
            "WebFetch(https://*.github.com/**)",
        };
        auto result = parse_pattern_list(raw);
        REQUIRE(result.size() == 3);
        CHECK(result[0].tool_name == "bash");
        CHECK(result[0].arg_glob == "npm test:*");
        CHECK(result[1].tool_name == "read");
        CHECK(result[1].arg_glob == "./src/**");
        CHECK(result[2].tool_name == "webfetch");
        CHECK(result[2].arg_glob == "https://*.github.com/**");
    }

    TEST_CASE("malformed entries are silently skipped") {
        std::vector<std::string> raw = {
            "Bash(npm test:*)",
            "not-a-pattern",          // no '('
            "Read(./src/**)",
            "(empty-tool-name)",      // empty tool name
            "Write(./build/*)",
        };
        auto result = parse_pattern_list(raw);
        // Only 3 valid entries should be returned
        REQUIRE(result.size() == 3);
        CHECK(result[0].tool_name == "bash");
        CHECK(result[1].tool_name == "read");
        CHECK(result[2].tool_name == "write");
    }

    TEST_CASE("empty input returns empty vector") {
        std::vector<std::string> raw;
        auto result = parse_pattern_list(raw);
        CHECK(result.empty());
    }

    TEST_CASE("all malformed returns empty vector") {
        std::vector<std::string> raw = {"noparen", "also-nope", ""};
        auto result = parse_pattern_list(raw);
        CHECK(result.empty());
    }
}

// ===========================================================================
// SUITE 11: matches() — malformed rule returns false, not a throw
// ===========================================================================
TEST_SUITE("matches() — malformed rule safety") {

    TEST_CASE("malformed rule returns false silently") {
        Json args = {{"command", "anything"}};
        CHECK(!matches("not-a-rule",    "Bash", args));
        CHECK(!matches("",              "Bash", args));
        CHECK(!matches("(no-tool)",     "Bash", args));
        CHECK(!matches("Bash(no-close", "Bash", args));
    }
}

// ===========================================================================
// SUITE 12: Edge cases and boundary conditions
// ===========================================================================
TEST_SUITE("glob_match — edge cases") {

    TEST_CASE("pattern with only **") {
        CHECK(glob_match("**", ""));
        CHECK(glob_match("**", "anything/at/all"));
    }

    TEST_CASE("empty pattern, empty text") {
        CHECK(glob_match("", ""));
    }

    TEST_CASE("empty pattern, non-empty text") {
        CHECK(!glob_match("", "x"));
    }

    TEST_CASE("non-empty pattern, empty text") {
        CHECK(!glob_match("a", ""));
        CHECK(glob_match("*", ""));   // * matches empty
        CHECK(glob_match("**", "")); // ** matches empty
    }

    TEST_CASE("consecutive ** handled correctly") {
        CHECK(glob_match("a/**/**", "a/b/c"));
        CHECK(glob_match("a/**/**", "a/"));
    }

    TEST_CASE("pattern matches exactly the text (no partial)") {
        CHECK(!glob_match("foo", "foobar"));
        CHECK(!glob_match("foobar", "foo"));
    }

    TEST_CASE("literal dots in URL patterns") {
        CHECK(glob_match("docs.example.com", "docs.example.com"));
        CHECK(!glob_match("docs.example.com", "docsXexampleYcom"));
    }

    TEST_CASE("slash-anchored path pattern") {
        CHECK(glob_match("/usr/local/**", "/usr/local/bin/clang"));
        CHECK(!glob_match("/usr/local/**", "/usr/bin/clang"));
    }
}
