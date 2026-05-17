// tests/integration/test_edit_tool.cpp
//
// doctest integration tests for batbox::tools::EditTool.
//
// Covers all 4 acceptance-criteria paths from task CPP 5.5:
//   1. Single match + replace: succeeds, diff in body.
//   2. Zero matches: error "old_string not found in <path>".
//   3. Multiple matches without replace_all: error "found N matches; ...".
//   4. replace_all=true: all replaced, count in body.
//
// Also covers:
//   - Plan mode refusal.
//   - Missing required args.
//   - Atomic write (file exists and has new content after success).
//   - Structured payload: path + replacements count.
//   - Cancellation.
//   - Unified diff appears in the successful result body.
//
// Each test uses a temporary directory that is cleaned up after the test.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/tools/EditTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/permissions/PermissionMode.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using namespace batbox;
using namespace batbox::tools;
using namespace batbox::permissions;

namespace fs = std::filesystem;

// =============================================================================
// Helpers
// =============================================================================

/// Write text to a file, creating parent directories as needed.
static void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    REQUIRE(f.is_open());
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
}

/// Read entire file into a string.
static std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    REQUIRE(f.is_open());
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// Build a minimal ToolContext for a given temp directory and permission mode.
static ToolContext make_ctx(const fs::path& cwd,
                            PermissionMode mode = PermissionMode::Default)
{
    ToolContext ctx;
    ctx.cwd        = cwd;
    ctx.mode       = mode;
    ctx.session_id = "test-session";
    ctx.agent_id   = "";
    return ctx;
}

/// Create and return a fresh temporary directory under the system temp root.
/// The caller is responsible for removing it (use RAII guard below).
struct TempDir {
    fs::path path;

    TempDir() {
        path = fs::temp_directory_path() / ("batbox_test_edit_" +
               std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);  // best-effort cleanup
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

// =============================================================================
// TEST SUITE: Acceptance criteria paths
// =============================================================================

TEST_SUITE("EditTool — acceptance criteria") {

    // -------------------------------------------------------------------------
    // AC 1: Single match + replace succeeds
    // -------------------------------------------------------------------------
    TEST_CASE("single match: succeeds, file updated, diff in body") {
        TempDir tmp;
        const fs::path file = tmp.path / "hello.txt";
        write_file(file, "Hello, world!\nThis is a test.\n");

        EditTool tool;
        auto ctx = make_ctx(tmp.path);
        Json args = {
            {"path",       "hello.txt"},
            {"old_string", "Hello, world!"},
            {"new_string", "Hello, batbox!"}
        };

        ToolResult r = tool.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        CHECK(r.body.find("---") != std::string::npos);
        CHECK(r.body.find("+++") != std::string::npos);
        CHECK(r.body.find("-Hello, world!") != std::string::npos);
        CHECK(r.body.find("+Hello, batbox!") != std::string::npos);

        // File must have new content.
        const std::string updated = read_file(file);
        CHECK(updated == "Hello, batbox!\nThis is a test.\n");

        // Structured payload must carry path + replacements=1.
        REQUIRE(r.structured_payload.has_value());
        CHECK(r.structured_payload->at("replacements") == 1);
        CHECK_FALSE(r.structured_payload->at("path").get<std::string>().empty());
    }

    // -------------------------------------------------------------------------
    // AC 2: Zero matches → error
    // -------------------------------------------------------------------------
    TEST_CASE("zero matches: error message contains path") {
        TempDir tmp;
        const fs::path file = tmp.path / "data.txt";
        write_file(file, "line one\nline two\n");

        EditTool tool;
        auto ctx = make_ctx(tmp.path);
        Json args = {
            {"path",       "data.txt"},
            {"old_string", "THIS DOES NOT EXIST"},
            {"new_string", "replacement"}
        };

        ToolResult r = tool.run(args, ctx);

        CHECK(r.is_error);
        CHECK(r.body.find("old_string not found in") != std::string::npos);
        // File must be unchanged.
        CHECK(read_file(file) == "line one\nline two\n");
    }

    // -------------------------------------------------------------------------
    // AC 3: Multiple matches without replace_all → error
    // -------------------------------------------------------------------------
    TEST_CASE("multiple matches without replace_all: error with count") {
        TempDir tmp;
        const fs::path file = tmp.path / "repeat.txt";
        write_file(file, "foo bar\nfoo baz\nfoo qux\n");

        EditTool tool;
        auto ctx = make_ctx(tmp.path);
        Json args = {
            {"path",       "repeat.txt"},
            {"old_string", "foo"},
            {"new_string", "FOO"}
        };

        ToolResult r = tool.run(args, ctx);

        CHECK(r.is_error);
        // Error must mention "found 3 matches" and suggest replace_all.
        CHECK(r.body.find("found 3 matches") != std::string::npos);
        CHECK(r.body.find("replace_all:true") != std::string::npos);
        // File must be unchanged.
        CHECK(read_file(file) == "foo bar\nfoo baz\nfoo qux\n");
    }

    // -------------------------------------------------------------------------
    // AC 4: replace_all=true → all occurrences replaced, count in body
    // -------------------------------------------------------------------------
    TEST_CASE("replace_all=true: all occurrences replaced, count in body") {
        TempDir tmp;
        const fs::path file = tmp.path / "multi.txt";
        write_file(file, "apple orange apple mango apple\n");

        EditTool tool;
        auto ctx = make_ctx(tmp.path);
        Json args = {
            {"path",        "multi.txt"},
            {"old_string",  "apple"},
            {"new_string",  "APPLE"},
            {"replace_all", true}
        };

        ToolResult r = tool.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        // Body should mention 3 replacements.
        CHECK(r.body.find("3") != std::string::npos);

        // All occurrences must be replaced in the file.
        const std::string updated = read_file(file);
        CHECK(updated.find("apple") == std::string::npos);
        CHECK(updated.find("APPLE") != std::string::npos);

        REQUIRE(r.structured_payload.has_value());
        CHECK(r.structured_payload->at("replacements") == 3);
    }

    // -------------------------------------------------------------------------
    // Diff in body (separate assertion)
    // -------------------------------------------------------------------------
    TEST_CASE("diff in body: unified diff headers present") {
        TempDir tmp;
        const fs::path file = tmp.path / "diff_check.txt";
        write_file(file, "line_a\nline_b\nline_c\n");

        EditTool tool;
        auto ctx = make_ctx(tmp.path);
        Json args = {
            {"path",       "diff_check.txt"},
            {"old_string", "line_b"},
            {"new_string", "line_B_edited"}
        };

        ToolResult r = tool.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        // Unified diff format requires --- and +++ header lines.
        CHECK(r.body.find("---") != std::string::npos);
        CHECK(r.body.find("+++") != std::string::npos);
        // Hunk header.
        CHECK(r.body.find("@@") != std::string::npos);
    }

    // -------------------------------------------------------------------------
    // Atomic write: file exists with correct content after success
    // -------------------------------------------------------------------------
    TEST_CASE("atomic write: modified content persists after run") {
        TempDir tmp;
        const fs::path file = tmp.path / "atomic.txt";
        write_file(file, "old content here\n");

        EditTool tool;
        auto ctx = make_ctx(tmp.path);
        Json args = {
            {"path",       "atomic.txt"},
            {"old_string", "old content"},
            {"new_string", "new content"}
        };

        ToolResult r = tool.run(args, ctx);
        REQUIRE_FALSE(r.is_error);
        CHECK(read_file(file) == "new content here\n");
    }
}

// =============================================================================
// TEST SUITE: Plan mode
// =============================================================================

TEST_SUITE("EditTool — plan mode") {

    TEST_CASE("plan mode: run() returns error immediately") {
        TempDir tmp;
        const fs::path file = tmp.path / "plan_test.txt";
        write_file(file, "some content\n");

        EditTool tool;
        auto ctx = make_ctx(tmp.path, PermissionMode::Plan);
        Json args = {
            {"path",       "plan_test.txt"},
            {"old_string", "some"},
            {"new_string", "other"}
        };

        ToolResult r = tool.run(args, ctx);

        CHECK(r.is_error);
        CHECK(r.body.find("plan mode") != std::string::npos);
        // File must be unchanged.
        CHECK(read_file(file) == "some content\n");
    }
}

// =============================================================================
// TEST SUITE: Argument validation
// =============================================================================

TEST_SUITE("EditTool — argument validation") {

    TEST_CASE("missing path arg: error") {
        EditTool tool;
        TempDir tmp;
        auto ctx = make_ctx(tmp.path);
        Json args = {{"old_string", "x"}, {"new_string", "y"}};
        ToolResult r = tool.run(args, ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("path") != std::string::npos);
    }

    TEST_CASE("missing old_string arg: error") {
        EditTool tool;
        TempDir tmp;
        auto ctx = make_ctx(tmp.path);
        Json args = {{"path", "file.txt"}, {"new_string", "y"}};
        ToolResult r = tool.run(args, ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("old_string") != std::string::npos);
    }

    TEST_CASE("missing new_string arg: error") {
        EditTool tool;
        TempDir tmp;
        auto ctx = make_ctx(tmp.path);
        Json args = {{"path", "file.txt"}, {"old_string", "x"}};
        ToolResult r = tool.run(args, ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("new_string") != std::string::npos);
    }

    TEST_CASE("empty path: error") {
        EditTool tool;
        TempDir tmp;
        auto ctx = make_ctx(tmp.path);
        Json args = {{"path", ""}, {"old_string", "x"}, {"new_string", "y"}};
        ToolResult r = tool.run(args, ctx);
        CHECK(r.is_error);
    }

    TEST_CASE("nonexistent file: error") {
        EditTool tool;
        TempDir tmp;
        auto ctx = make_ctx(tmp.path);
        Json args = {
            {"path",       "no_such_file.txt"},
            {"old_string", "x"},
            {"new_string", "y"}
        };
        ToolResult r = tool.run(args, ctx);
        CHECK(r.is_error);
    }
}

// =============================================================================
// TEST SUITE: Cancellation
// =============================================================================

TEST_SUITE("EditTool — cancellation") {

    TEST_CASE("cancelled token: error returned before file I/O") {
        EditTool tool;
        TempDir tmp;

        auto [src, tok] = CancelToken::make_root();
        src.request_stop();

        ToolContext ctx = make_ctx(tmp.path);
        ctx.cancel_token = std::move(tok);

        Json args = {
            {"path",       "whatever.txt"},
            {"old_string", "x"},
            {"new_string", "y"}
        };

        ToolResult r = tool.run(args, ctx);
        CHECK(r.is_error);
        CHECK(r.body == "cancelled");
    }
}

// =============================================================================
// TEST SUITE: Schema contract
// =============================================================================

TEST_SUITE("EditTool — schema and identity") {

    TEST_CASE("name() returns 'Edit'") {
        EditTool tool;
        CHECK(tool.name() == std::string_view("Edit"));
    }

    TEST_CASE("is_read_only() returns false") {
        EditTool tool;
        CHECK_FALSE(tool.is_read_only());
    }

    TEST_CASE("requires_confirmation() returns true") {
        EditTool tool;
        CHECK(tool.requires_confirmation());
    }

    TEST_CASE("schema_json() has correct structure") {
        EditTool tool;
        Json s = tool.schema_json();
        REQUIRE(s.is_object());
        CHECK(s["name"].get<std::string>() == "Edit");
        REQUIRE(s.contains("parameters"));
        const auto& props = s["parameters"]["properties"];
        CHECK(props.contains("path"));
        CHECK(props.contains("old_string"));
        CHECK(props.contains("new_string"));
        CHECK(props.contains("replace_all"));
        // Required fields: path, old_string, new_string
        const auto& req = s["parameters"]["required"];
        CHECK(req.size() == 3);
    }

    TEST_CASE("schema name matches name()") {
        EditTool tool;
        CHECK(tool.schema_json()["name"].get<std::string>() ==
              std::string(tool.name()));
    }
}

// =============================================================================
// TEST SUITE: Edge cases
// =============================================================================

TEST_SUITE("EditTool — edge cases") {

    TEST_CASE("replace_all with exactly one match succeeds (no error)") {
        TempDir tmp;
        const fs::path file = tmp.path / "one.txt";
        write_file(file, "unique token here\n");

        EditTool tool;
        auto ctx = make_ctx(tmp.path);
        Json args = {
            {"path",        "one.txt"},
            {"old_string",  "unique token"},
            {"new_string",  "replaced token"},
            {"replace_all", true}
        };

        ToolResult r = tool.run(args, ctx);
        REQUIRE_FALSE(r.is_error);
        CHECK(read_file(file) == "replaced token here\n");
        REQUIRE(r.structured_payload.has_value());
        CHECK(r.structured_payload->at("replacements") == 1);
    }

    TEST_CASE("multiline old_string replaced correctly") {
        TempDir tmp;
        const fs::path file = tmp.path / "multi_line.txt";
        write_file(file, "line1\nline2\nline3\n");

        EditTool tool;
        auto ctx = make_ctx(tmp.path);
        Json args = {
            {"path",       "multi_line.txt"},
            {"old_string", "line1\nline2"},
            {"new_string", "replaced1\nreplaced2"}
        };

        ToolResult r = tool.run(args, ctx);
        REQUIRE_FALSE(r.is_error);
        CHECK(read_file(file) == "replaced1\nreplaced2\nline3\n");
    }

    TEST_CASE("replace with empty new_string effectively deletes the match") {
        TempDir tmp;
        const fs::path file = tmp.path / "delete_match.txt";
        write_file(file, "remove_me and keep_me\n");

        EditTool tool;
        auto ctx = make_ctx(tmp.path);
        Json args = {
            {"path",       "delete_match.txt"},
            {"old_string", "remove_me "},
            {"new_string", ""}
        };

        ToolResult r = tool.run(args, ctx);
        REQUIRE_FALSE(r.is_error);
        CHECK(read_file(file) == "and keep_me\n");
    }

    TEST_CASE("absolute path is accepted and resolved correctly") {
        TempDir tmp;
        const fs::path file = tmp.path / "abs.txt";
        write_file(file, "absolute path test\n");

        EditTool tool;
        ToolContext ctx;
        ctx.cwd        = std::filesystem::temp_directory_path(); // different cwd
        ctx.mode       = PermissionMode::Default;
        ctx.session_id = "test-session";

        Json args = {
            {"path",       file.string()},  // absolute path
            {"old_string", "absolute"},
            {"new_string", "ABSOLUTE"}
        };

        ToolResult r = tool.run(args, ctx);
        REQUIRE_FALSE(r.is_error);
        CHECK(read_file(file) == "ABSOLUTE path test\n");
    }
}
