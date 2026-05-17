// tests/unit/test_glob_tool.cpp
// ---------------------------------------------------------------------------
// doctest suite for batbox::tools::GlobTool.
//
// Covers:
//   - name(), description(), schema_json() interface contract
//   - is_read_only() == true, requires_confirmation() == false
//   - Basic *.ext single-component matching
//   - src/**/*.hpp recursive matching
//   - Results sorted by mtime descending
//   - Empty result set returns "(no matches)"
//   - Structured payload: matches array + count field
//   - Missing/empty pattern returns error
//   - Non-existent base path returns error
//   - Non-directory base path returns error
//   - Plan mode: runs successfully (is_read_only)
//   - Cancellation returns error immediately
//   - path argument overrides ctx.cwd
//
// Build (standalone, no CMake):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_glob_tool.cpp \
//       src/tools/GlobTool.cpp \
//       src/permissions/PatternMatcher.cpp \
//       -o /tmp/test_glob && /tmp/test_glob
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/tools/GlobTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/permissions/PermissionMode.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace batbox;
using namespace batbox::tools;
using namespace batbox::permissions;

// =============================================================================
// Test fixture helpers
// =============================================================================

/// Build a ToolContext pointed at `dir`.
static ToolContext make_ctx(const fs::path& cwd,
                            PermissionMode  mode = PermissionMode::Default) {
    ToolContext ctx;
    ctx.cwd        = cwd;
    ctx.mode       = mode;
    ctx.session_id = "test";
    ctx.agent_id   = "";
    return ctx;
}

/// Create a regular file at `path` with optional content.
static void touch(const fs::path& path, const std::string& content = "x") {
    std::ofstream f(path);
    f << content;
}

/// Temporary directory RAII wrapper.
struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& suffix = "glob_test") {
        path = fs::temp_directory_path() / ("batbox_" + suffix + "_XXXXXX");
        // mkdtemp-style: create uniquely
        path += std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);  // ignore errors on cleanup
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

// =============================================================================
// TEST SUITE 1: Interface contract
// =============================================================================
TEST_SUITE("GlobTool — interface contract") {

    TEST_CASE("name() returns 'Glob'") {
        GlobTool t;
        CHECK(t.name() == std::string_view("Glob"));
    }

    TEST_CASE("description() is non-empty") {
        GlobTool t;
        CHECK(!t.description().empty());
    }

    TEST_CASE("is_read_only() returns true") {
        GlobTool t;
        CHECK(t.is_read_only());
    }

    TEST_CASE("requires_confirmation() returns false") {
        GlobTool t;
        CHECK_FALSE(t.requires_confirmation());
    }

    TEST_CASE("schema_json() contains name, description, parameters") {
        GlobTool t;
        const Json s = t.schema_json();
        REQUIRE(s.is_object());
        REQUIRE(s.contains("name"));
        REQUIRE(s.contains("description"));
        REQUIRE(s.contains("parameters"));
        CHECK(s["name"].get<std::string>() == "Glob");
        // schema name must match name()
        CHECK(s["name"].get<std::string>() == std::string(t.name()));
    }

    TEST_CASE("schema_json() parameters has 'pattern' as required property") {
        GlobTool t;
        const Json params = t.schema_json()["parameters"];
        REQUIRE(params.contains("properties"));
        REQUIRE(params["properties"].contains("pattern"));
        REQUIRE(params.contains("required"));
        bool has_pattern = false;
        for (const auto& r : params["required"]) {
            if (r.get<std::string>() == "pattern") has_pattern = true;
        }
        CHECK(has_pattern);
    }

    TEST_CASE("schema_json() parameters has optional 'path' property") {
        GlobTool t;
        const Json params = t.schema_json()["parameters"];
        REQUIRE(params.contains("properties"));
        CHECK(params["properties"].contains("path"));
    }
}

// =============================================================================
// TEST SUITE 2: Argument validation
// =============================================================================
TEST_SUITE("GlobTool — argument validation") {

    TEST_CASE("missing 'pattern' returns error") {
        TempDir tmp;
        GlobTool t;
        auto ctx = make_ctx(tmp.path);
        Json args = Json::object();  // no pattern
        auto r = t.run(args, ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("pattern") != std::string::npos);
    }

    TEST_CASE("non-string 'pattern' returns error") {
        TempDir tmp;
        GlobTool t;
        auto ctx = make_ctx(tmp.path);
        Json args = {{"pattern", 42}};
        auto r = t.run(args, ctx);
        CHECK(r.is_error);
    }

    TEST_CASE("empty string 'pattern' returns error") {
        TempDir tmp;
        GlobTool t;
        auto ctx = make_ctx(tmp.path);
        Json args = {{"pattern", ""}};
        auto r = t.run(args, ctx);
        CHECK(r.is_error);
    }

    TEST_CASE("non-existent base path returns error") {
        TempDir tmp;
        GlobTool t;
        // Use a cwd that doesn't exist
        ToolContext ctx = make_ctx(tmp.path / "does_not_exist");
        Json args = {{"pattern", "*.cpp"}};
        auto r = t.run(args, ctx);
        CHECK(r.is_error);
    }

    TEST_CASE("base path pointing to a file (not dir) returns error") {
        TempDir tmp;
        const fs::path file = tmp.path / "afile.txt";
        touch(file);
        GlobTool t;
        auto ctx = make_ctx(tmp.path);
        Json args = {{"pattern", "*.cpp"}, {"path", file.string()}};
        auto r = t.run(args, ctx);
        CHECK(r.is_error);
    }
}

// =============================================================================
// TEST SUITE 3: Basic glob matching
// =============================================================================
TEST_SUITE("GlobTool — basic matching") {

    TEST_CASE("*.cpp matches all cpp files at root level") {
        TempDir tmp;
        touch(tmp.path / "main.cpp");
        touch(tmp.path / "util.cpp");
        touch(tmp.path / "readme.txt");

        GlobTool t;
        auto ctx = make_ctx(tmp.path);
        Json args = {{"pattern", "*.cpp"}};
        auto r = t.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        REQUIRE(r.structured_payload.has_value());
        const Json& pay = *r.structured_payload;
        CHECK(pay["count"].get<std::size_t>() == 2);
        // Body should contain both .cpp files
        CHECK(r.body.find("main.cpp") != std::string::npos);
        CHECK(r.body.find("util.cpp") != std::string::npos);
        // readme.txt should NOT appear
        CHECK(r.body.find("readme.txt") == std::string::npos);
    }

    TEST_CASE("no matches returns ok with '(no matches)' body") {
        TempDir tmp;
        touch(tmp.path / "readme.txt");

        GlobTool t;
        auto ctx = make_ctx(tmp.path);
        Json args = {{"pattern", "*.cpp"}};
        auto r = t.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        CHECK(r.body.find("(no matches)") != std::string::npos);
        REQUIRE(r.structured_payload.has_value());
        CHECK((*r.structured_payload)["count"].get<std::size_t>() == 0);
        CHECK((*r.structured_payload)["matches"].empty());
    }

    TEST_CASE("empty directory returns ok with '(no matches)'") {
        TempDir tmp;
        GlobTool t;
        auto ctx = make_ctx(tmp.path);
        Json args = {{"pattern", "**"}};
        auto r = t.run(args, ctx);
        REQUIRE_FALSE(r.is_error);
        CHECK(r.body.find("(no matches)") != std::string::npos);
    }

    TEST_CASE("** pattern finds files in nested subdirectories") {
        TempDir tmp;
        fs::create_directories(tmp.path / "a" / "b");
        touch(tmp.path / "a" / "b" / "deep.hpp");
        touch(tmp.path / "a" / "mid.hpp");
        touch(tmp.path / "top.hpp");
        touch(tmp.path / "other.txt");

        GlobTool t;
        auto ctx = make_ctx(tmp.path);
        Json args = {{"pattern", "**/*.hpp"}};
        auto r = t.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        REQUIRE(r.structured_payload.has_value());
        CHECK((*r.structured_payload)["count"].get<std::size_t>() == 3);
        CHECK(r.body.find("deep.hpp")  != std::string::npos);
        CHECK(r.body.find("mid.hpp")   != std::string::npos);
        CHECK(r.body.find("top.hpp")   != std::string::npos);
        CHECK(r.body.find("other.txt") == std::string::npos);
    }

    TEST_CASE("src/**/*.hpp recursive match") {
        TempDir tmp;
        fs::create_directories(tmp.path / "src" / "core");
        fs::create_directories(tmp.path / "src" / "tools");
        touch(tmp.path / "src" / "core" / "Foo.hpp");
        touch(tmp.path / "src" / "tools" / "Bar.hpp");
        touch(tmp.path / "src" / "Root.hpp");
        touch(tmp.path / "include" / "Baz.hpp");   // should NOT match
        fs::create_directories(tmp.path / "include");
        touch(tmp.path / "include" / "Baz.hpp");

        GlobTool t;
        auto ctx = make_ctx(tmp.path);
        Json args = {{"pattern", "src/**/*.hpp"}};
        auto r = t.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        REQUIRE(r.structured_payload.has_value());
        // Foo.hpp, Bar.hpp, Root.hpp match; Baz.hpp does not
        const std::size_t cnt = (*r.structured_payload)["count"].get<std::size_t>();
        CHECK(cnt == 3);
        CHECK(r.body.find("Foo.hpp")  != std::string::npos);
        CHECK(r.body.find("Bar.hpp")  != std::string::npos);
        CHECK(r.body.find("Root.hpp") != std::string::npos);
        CHECK(r.body.find("Baz.hpp")  == std::string::npos);
    }

    TEST_CASE("* does NOT cross path separator") {
        TempDir tmp;
        fs::create_directories(tmp.path / "sub");
        touch(tmp.path / "sub" / "deep.cpp");
        touch(tmp.path / "top.cpp");

        GlobTool t;
        auto ctx = make_ctx(tmp.path);
        Json args = {{"pattern", "*.cpp"}};  // * should not cross /
        auto r = t.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        REQUIRE(r.structured_payload.has_value());
        // Only top.cpp should match; sub/deep.cpp should not
        CHECK((*r.structured_payload)["count"].get<std::size_t>() == 1);
        CHECK(r.body.find("top.cpp")  != std::string::npos);
        CHECK(r.body.find("deep.cpp") == std::string::npos);
    }
}

// =============================================================================
// TEST SUITE 4: Sorted by mtime descending
// =============================================================================
TEST_SUITE("GlobTool — mtime ordering") {

    TEST_CASE("results are sorted by mtime descending (newest first)") {
        TempDir tmp;

        // Create files with explicit mtime spread.
        // We write them sequentially; filesystem resolution is usually 1s on
        // macOS HFS+ (or immediate on APFS).  To guarantee ordering we
        // manually set last_write_time.
        const fs::path old_file = tmp.path / "old.cpp";
        const fs::path new_file = tmp.path / "new.cpp";
        touch(old_file);
        touch(new_file);

        // Set old_file to well in the past relative to new_file.
        const auto now = fs::file_time_type::clock::now();
        fs::last_write_time(old_file, now - std::chrono::hours(24));
        fs::last_write_time(new_file, now);

        GlobTool t;
        auto ctx = make_ctx(tmp.path);
        Json args = {{"pattern", "*.cpp"}};
        auto r = t.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        REQUIRE(r.structured_payload.has_value());
        const auto& matches = (*r.structured_payload)["matches"];
        REQUIRE(matches.size() == 2);

        // First result should be the newer file.
        const std::string first = matches[0].get<std::string>();
        CHECK(first.find("new.cpp") != std::string::npos);
        // Second should be the older file.
        const std::string second = matches[1].get<std::string>();
        CHECK(second.find("old.cpp") != std::string::npos);
    }
}

// =============================================================================
// TEST SUITE 5: path argument
// =============================================================================
TEST_SUITE("GlobTool — path argument") {

    TEST_CASE("explicit 'path' argument overrides ctx.cwd") {
        TempDir outer;
        TempDir inner;
        touch(outer.path / "outer.cpp");
        touch(inner.path / "inner.cpp");

        GlobTool t;
        // ctx.cwd points to outer, but 'path' arg points to inner
        auto ctx = make_ctx(outer.path);
        Json args = {{"pattern", "*.cpp"}, {"path", inner.path.string()}};
        auto r = t.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        REQUIRE(r.structured_payload.has_value());
        CHECK((*r.structured_payload)["count"].get<std::size_t>() == 1);
        CHECK(r.body.find("inner.cpp") != std::string::npos);
        CHECK(r.body.find("outer.cpp") == std::string::npos);
    }

    TEST_CASE("empty 'path' string falls back to ctx.cwd") {
        TempDir tmp;
        touch(tmp.path / "hello.cpp");

        GlobTool t;
        auto ctx = make_ctx(tmp.path);
        Json args = {{"pattern", "*.cpp"}, {"path", ""}};
        auto r = t.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        CHECK((*r.structured_payload)["count"].get<std::size_t>() == 1);
    }

    TEST_CASE("absent 'path' key falls back to ctx.cwd") {
        TempDir tmp;
        touch(tmp.path / "hello.cpp");

        GlobTool t;
        auto ctx = make_ctx(tmp.path);
        Json args = {{"pattern", "*.cpp"}};  // no 'path' key
        auto r = t.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        CHECK((*r.structured_payload)["count"].get<std::size_t>() == 1);
    }
}

// =============================================================================
// TEST SUITE 6: Plan mode
// =============================================================================
TEST_SUITE("GlobTool — plan mode") {

    TEST_CASE("Glob runs successfully in Plan mode (is_read_only)") {
        TempDir tmp;
        touch(tmp.path / "hello.cpp");

        GlobTool t;
        auto ctx = make_ctx(tmp.path, PermissionMode::Plan);
        Json args = {{"pattern", "*.cpp"}};
        auto r = t.run(args, ctx);

        CHECK_FALSE(r.is_error);
        CHECK((*r.structured_payload)["count"].get<std::size_t>() == 1);
    }
}

// =============================================================================
// TEST SUITE 7: Cancellation
// =============================================================================
TEST_SUITE("GlobTool — cancellation") {

    TEST_CASE("cancelled context returns error immediately") {
        TempDir tmp;
        touch(tmp.path / "foo.cpp");

        GlobTool t;
        auto [src, tok] = CancelToken::make_root();
        src.request_stop();  // cancel before run

        ToolContext ctx = make_ctx(tmp.path);
        ctx.cancel_token = std::move(tok);

        Json args = {{"pattern", "*.cpp"}};
        auto r = t.run(args, ctx);

        CHECK(r.is_error);
        CHECK(r.body.find("cancel") != std::string::npos);
    }
}

// =============================================================================
// TEST SUITE 8: Structured payload contract
// =============================================================================
TEST_SUITE("GlobTool — structured payload") {

    TEST_CASE("payload has 'matches' array and 'count' integer") {
        TempDir tmp;
        touch(tmp.path / "a.cpp");
        touch(tmp.path / "b.cpp");

        GlobTool t;
        auto ctx = make_ctx(tmp.path);
        Json args = {{"pattern", "*.cpp"}};
        auto r = t.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        REQUIRE(r.structured_payload.has_value());
        const Json& pay = *r.structured_payload;
        REQUIRE(pay.contains("matches"));
        REQUIRE(pay.contains("count"));
        CHECK(pay["matches"].is_array());
        CHECK(pay["count"].is_number_unsigned());
        CHECK(pay["matches"].size() == pay["count"].get<std::size_t>());
    }

    TEST_CASE("payload paths are absolute strings") {
        TempDir tmp;
        touch(tmp.path / "main.cpp");

        GlobTool t;
        auto ctx = make_ctx(tmp.path);
        Json args = {{"pattern", "*.cpp"}};
        auto r = t.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        REQUIRE(r.structured_payload.has_value());
        const Json& matches = (*r.structured_payload)["matches"];
        REQUIRE(matches.size() == 1);
        const std::string p = matches[0].get<std::string>();
        // Must start with '/' on Unix (absolute path).
        CHECK(p[0] == '/');
    }

    TEST_CASE("body paths match payload paths") {
        TempDir tmp;
        touch(tmp.path / "foo.hpp");

        GlobTool t;
        auto ctx = make_ctx(tmp.path);
        Json args = {{"pattern", "*.hpp"}};
        auto r = t.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        REQUIRE(r.structured_payload.has_value());
        const std::string p = (*r.structured_payload)["matches"][0].get<std::string>();
        // The body should contain the same path.
        CHECK(r.body.find(p) != std::string::npos);
    }
}
