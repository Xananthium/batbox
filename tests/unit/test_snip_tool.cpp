// tests/unit/test_snip_tool.cpp
//
// doctest suite for batbox::tools::SnipTool.
//
// Tests cover all four actions (save, load, list, delete), validation,
// Plan-mode gating, and cancellation.  The test uses a temporary directory
// rather than the real ~/.batbox/snippets/ directory; the snippet storage
// directory is pointed at a controlled temp path via the BATBOX_SNIPPETS_DIR
// override mechanism — but since SnipTool derives the path from HOME, tests
// temporarily override HOME to a tmp dir so no real user data is touched.
//
// CPP 5.25 acceptance criteria:
//   [x] save creates file, returns path
//   [x] load reads it back
//   [x] list returns names + first-line preview
//   [x] delete removes the file
//   [x] Unknown name: error
//   [x] Unit test

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/tools/SnipTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/permissions/PermissionMode.hpp>
#include <batbox/core/CancelToken.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace batbox;
using namespace batbox::tools;
using namespace batbox::permissions;

// =============================================================================
// Test infrastructure
// =============================================================================

/// RAII wrapper: sets HOME to a temp dir for the test, restores on destruction.
/// This ensures SnipTool writes to a test-controlled location and does not
/// pollute the developer's ~/.batbox/snippets/ directory.
struct ScopedHome {
    std::string original_home;
    fs::path    tmp_dir;

    ScopedHome() {
        // Capture original HOME.
        const char* h = std::getenv("HOME");
        original_home = h ? h : "";

        // Create a unique temp directory.
        tmp_dir = fs::temp_directory_path()
                / ("batbox_snip_test_" + std::to_string(
                       std::hash<std::thread::id>{}(std::this_thread::get_id())));
        fs::create_directories(tmp_dir);

        // Point HOME at the temp dir.
        setenv("HOME", tmp_dir.c_str(), /*overwrite=*/1);
    }

    ~ScopedHome() {
        // Restore HOME.
        if (!original_home.empty()) {
            setenv("HOME", original_home.c_str(), /*overwrite=*/1);
        } else {
            unsetenv("HOME");
        }
        // Remove temp directory tree.
        std::error_code ec;
        fs::remove_all(tmp_dir, ec);
    }

    /// Returns the path that SnipTool will use as snippets dir.
    fs::path snippets_dir() const {
        return tmp_dir / ".batbox" / "snippets";
    }
};

/// Build a minimal ToolContext with the given mode.
static ToolContext make_ctx(PermissionMode mode = PermissionMode::Default) {
    ToolContext ctx;
    ctx.cwd        = fs::current_path();
    ctx.mode       = mode;
    ctx.session_id = "test-session";
    ctx.agent_id   = "";
    return ctx;
}

/// Build a ToolContext with an already-cancelled token.
static ToolContext make_cancelled_ctx() {
    auto [src, tok] = CancelToken::make_root();
    src.request_stop();
    ToolContext ctx = make_ctx();
    ctx.cancel_token = std::move(tok);
    return ctx;
}

// =============================================================================
// TEST SUITE: SnipTool identity
// =============================================================================
TEST_SUITE("SnipTool — identity and schema") {

    TEST_CASE("name() returns \"Snip\"") {
        SnipTool t;
        CHECK(t.name() == std::string_view("Snip"));
    }

    TEST_CASE("description() is non-empty") {
        SnipTool t;
        CHECK(!t.description().empty());
    }

    TEST_CASE("is_read_only() returns false") {
        SnipTool t;
        CHECK_FALSE(t.is_read_only());
    }

    TEST_CASE("requires_confirmation() returns false") {
        SnipTool t;
        CHECK_FALSE(t.requires_confirmation());
    }

    TEST_CASE("schema_json() has required OpenAI tool fields") {
        SnipTool t;
        Json s = t.schema_json();
        REQUIRE(s.is_object());
        REQUIRE(s.contains("name"));
        REQUIRE(s.contains("description"));
        REQUIRE(s.contains("parameters"));
        CHECK(s["name"].get<std::string>() == "Snip");
        CHECK(s["parameters"]["type"].get<std::string>() == "object");
        REQUIRE(s["parameters"].contains("properties"));
        CHECK(s["parameters"]["properties"].contains("action"));
        CHECK(s["parameters"]["properties"].contains("name"));
        CHECK(s["parameters"]["properties"].contains("content"));
    }

    TEST_CASE("schema name matches name()") {
        SnipTool t;
        CHECK(t.schema_json()["name"].get<std::string>()
              == std::string(t.name()));
    }
}

// =============================================================================
// TEST SUITE: argument validation
// =============================================================================
TEST_SUITE("SnipTool — argument validation") {

    TEST_CASE("missing action returns error") {
        SnipTool t;
        auto ctx = make_ctx();
        ToolResult r = t.run(Json::object(), ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("action") != std::string::npos);
    }

    TEST_CASE("non-string action returns error") {
        SnipTool t;
        auto ctx = make_ctx();
        ToolResult r = t.run(Json{{"action", 42}}, ctx);
        CHECK(r.is_error);
    }

    TEST_CASE("unknown action returns error") {
        SnipTool t;
        auto ctx = make_ctx();
        ToolResult r = t.run(Json{{"action", "copy"}}, ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("copy") != std::string::npos);
    }

    TEST_CASE("save without name returns error") {
        SnipTool t;
        auto ctx = make_ctx();
        ToolResult r = t.run(Json{{"action", "save"}, {"content", "hello"}}, ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("name") != std::string::npos);
    }

    TEST_CASE("save without content returns error") {
        ScopedHome guard;
        SnipTool t;
        auto ctx = make_ctx();
        ToolResult r = t.run(Json{{"action", "save"}, {"name", "mysnip"}}, ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("content") != std::string::npos);
    }

    TEST_CASE("load without name returns error") {
        SnipTool t;
        auto ctx = make_ctx();
        ToolResult r = t.run(Json{{"action", "load"}}, ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("name") != std::string::npos);
    }

    TEST_CASE("delete without name returns error") {
        SnipTool t;
        auto ctx = make_ctx();
        ToolResult r = t.run(Json{{"action", "delete"}}, ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("name") != std::string::npos);
    }

    TEST_CASE("name with path separator '/' is rejected") {
        SnipTool t;
        auto ctx = make_ctx();
        ToolResult r = t.run(Json{{"action", "load"}, {"name", "foo/bar"}}, ctx);
        CHECK(r.is_error);
    }

    TEST_CASE("name starting with '.' is rejected") {
        SnipTool t;
        auto ctx = make_ctx();
        ToolResult r = t.run(Json{{"action", "load"}, {"name", ".hidden"}}, ctx);
        CHECK(r.is_error);
    }

    TEST_CASE("name with spaces is rejected") {
        SnipTool t;
        auto ctx = make_ctx();
        ToolResult r = t.run(Json{{"action", "load"}, {"name", "my snip"}}, ctx);
        CHECK(r.is_error);
    }

    TEST_CASE("empty name is rejected") {
        SnipTool t;
        auto ctx = make_ctx();
        ToolResult r = t.run(Json{{"action", "load"}, {"name", ""}}, ctx);
        CHECK(r.is_error);
    }
}

// =============================================================================
// TEST SUITE: Plan-mode gating
// =============================================================================
TEST_SUITE("SnipTool — Plan mode") {

    TEST_CASE("save in Plan mode returns error") {
        ScopedHome guard;
        SnipTool t;
        auto ctx = make_ctx(PermissionMode::Plan);
        ToolResult r = t.run(
            Json{{"action", "save"}, {"name", "snip1"}, {"content", "hello"}}, ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("Plan mode") != std::string::npos);
    }

    TEST_CASE("delete in Plan mode returns error") {
        ScopedHome guard;
        SnipTool t;
        auto ctx = make_ctx(PermissionMode::Plan);
        ToolResult r = t.run(Json{{"action", "delete"}, {"name", "snip1"}}, ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("Plan mode") != std::string::npos);
    }

    TEST_CASE("load in Plan mode succeeds (after save in Default mode)") {
        ScopedHome guard;
        SnipTool t;

        // Save in Default mode first.
        auto save_ctx = make_ctx(PermissionMode::Default);
        ToolResult save_r = t.run(
            Json{{"action", "save"}, {"name", "plantest"}, {"content", "plan content"}},
            save_ctx);
        REQUIRE_FALSE(save_r.is_error);

        // Now load in Plan mode — should succeed.
        auto plan_ctx = make_ctx(PermissionMode::Plan);
        ToolResult load_r = t.run(Json{{"action", "load"}, {"name", "plantest"}}, plan_ctx);
        CHECK_FALSE(load_r.is_error);
        CHECK(load_r.body == "plan content");
    }

    TEST_CASE("list in Plan mode succeeds") {
        ScopedHome guard;
        SnipTool t;
        auto ctx = make_ctx(PermissionMode::Plan);
        ToolResult r = t.run(Json{{"action", "list"}}, ctx);
        CHECK_FALSE(r.is_error);
    }
}

// =============================================================================
// TEST SUITE: save action
// =============================================================================
TEST_SUITE("SnipTool — save action") {

    TEST_CASE("save creates the file and returns its path") {
        ScopedHome guard;
        SnipTool t;
        auto ctx = make_ctx();

        ToolResult r = t.run(
            Json{{"action", "save"}, {"name", "hello"}, {"content", "world content"}},
            ctx);

        REQUIRE_FALSE(r.is_error);

        // Body mentions the path.
        CHECK(r.body.find("hello") != std::string::npos);

        // File must exist.
        const fs::path expected = guard.snippets_dir() / "hello.txt";
        CHECK(fs::exists(expected));

        // File content must match.
        std::ifstream in(expected);
        std::ostringstream buf;
        buf << in.rdbuf();
        CHECK(buf.str() == "world content");
    }

    TEST_CASE("save structured payload contains path and name") {
        ScopedHome guard;
        SnipTool t;
        auto ctx = make_ctx();

        ToolResult r = t.run(
            Json{{"action", "save"}, {"name", "mypkg"}, {"content", "package code"}},
            ctx);

        REQUIRE_FALSE(r.is_error);
        REQUIRE(r.structured_payload.has_value());
        CHECK(r.structured_payload->at("name").get<std::string>() == "mypkg");
        CHECK(!r.structured_payload->at("path").get<std::string>().empty());
    }

    TEST_CASE("save overwrites existing snippet") {
        ScopedHome guard;
        SnipTool t;
        auto ctx = make_ctx();

        (void)t.run(Json{{"action","save"},{"name","overwrite"},{"content","first"}}, ctx);
        ToolResult r = t.run(
            Json{{"action","save"},{"name","overwrite"},{"content","second"}}, ctx);
        REQUIRE_FALSE(r.is_error);

        const fs::path p = guard.snippets_dir() / "overwrite.txt";
        std::ifstream in(p);
        std::ostringstream buf;
        buf << in.rdbuf();
        CHECK(buf.str() == "second");
    }

    TEST_CASE("save creates directory if it does not exist") {
        ScopedHome guard;
        SnipTool t;
        auto ctx = make_ctx();

        // Directory should not exist yet.
        REQUIRE_FALSE(fs::exists(guard.snippets_dir()));

        ToolResult r = t.run(
            Json{{"action","save"},{"name","newdir"},{"content","created"}}, ctx);
        CHECK_FALSE(r.is_error);
        CHECK(fs::exists(guard.snippets_dir()));
    }

    TEST_CASE("save preserves empty content") {
        ScopedHome guard;
        SnipTool t;
        auto ctx = make_ctx();

        ToolResult r = t.run(
            Json{{"action","save"},{"name","empty_snip"},{"content",""}}, ctx);
        REQUIRE_FALSE(r.is_error);

        const fs::path p = guard.snippets_dir() / "empty_snip.txt";
        REQUIRE(fs::exists(p));
        CHECK(fs::file_size(p) == 0);
    }

    TEST_CASE("save with valid name characters succeeds") {
        ScopedHome guard;
        SnipTool t;
        auto ctx = make_ctx();

        // [a-zA-Z0-9_.-] are all valid.
        ToolResult r = t.run(
            Json{{"action","save"},{"name","My_Snip-v1.0"},{"content","ok"}}, ctx);
        CHECK_FALSE(r.is_error);
    }
}

// =============================================================================
// TEST SUITE: load action
// =============================================================================
TEST_SUITE("SnipTool — load action") {

    TEST_CASE("load returns saved content verbatim") {
        ScopedHome guard;
        SnipTool t;
        auto ctx = make_ctx();

        const std::string content = "line 1\nline 2\nline 3\n";
        (void)t.run(Json{{"action","save"},{"name","multi"},{"content",content}}, ctx);

        ToolResult r = t.run(Json{{"action","load"},{"name","multi"}}, ctx);
        REQUIRE_FALSE(r.is_error);
        CHECK(r.body == content);
    }

    TEST_CASE("load structured payload contains name and content") {
        ScopedHome guard;
        SnipTool t;
        auto ctx = make_ctx();

        (void)t.run(Json{{"action","save"},{"name","payload_test"},{"content","abc"}}, ctx);
        ToolResult r = t.run(Json{{"action","load"},{"name","payload_test"}}, ctx);

        REQUIRE_FALSE(r.is_error);
        REQUIRE(r.structured_payload.has_value());
        CHECK(r.structured_payload->at("name").get<std::string>() == "payload_test");
        CHECK(r.structured_payload->at("content").get<std::string>() == "abc");
    }

    TEST_CASE("load of non-existent snippet returns error") {
        ScopedHome guard;
        SnipTool t;
        auto ctx = make_ctx();

        ToolResult r = t.run(Json{{"action","load"},{"name","no_such_snippet"}}, ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("no_such_snippet") != std::string::npos);
    }
}

// =============================================================================
// TEST SUITE: list action
// =============================================================================
TEST_SUITE("SnipTool — list action") {

    TEST_CASE("list on empty directory returns informative message") {
        ScopedHome guard;
        SnipTool t;
        auto ctx = make_ctx();

        ToolResult r = t.run(Json{{"action","list"}}, ctx);
        CHECK_FALSE(r.is_error);
        // Should indicate no snippets.
        CHECK(!r.body.empty());
    }

    TEST_CASE("list returns all saved snippet names") {
        ScopedHome guard;
        SnipTool t;
        auto ctx = make_ctx();

        (void)t.run(Json{{"action","save"},{"name","alpha"},{"content","first line alpha"}}, ctx);
        (void)t.run(Json{{"action","save"},{"name","beta"}, {"content","first line beta"}},  ctx);
        (void)t.run(Json{{"action","save"},{"name","gamma"},{"content","first line gamma"}}, ctx);

        ToolResult r = t.run(Json{{"action","list"}}, ctx);
        REQUIRE_FALSE(r.is_error);

        CHECK(r.body.find("alpha") != std::string::npos);
        CHECK(r.body.find("beta")  != std::string::npos);
        CHECK(r.body.find("gamma") != std::string::npos);
    }

    TEST_CASE("list includes first-line preview of content") {
        ScopedHome guard;
        SnipTool t;
        auto ctx = make_ctx();

        (void)t.run(Json{{"action","save"},{"name","preview_test"},
                   {"content","preview first line\nsecond line"}}, ctx);

        ToolResult r = t.run(Json{{"action","list"}}, ctx);
        REQUIRE_FALSE(r.is_error);
        CHECK(r.body.find("preview first line") != std::string::npos);
        // Second line should not appear in the list output.
        CHECK(r.body.find("second line") == std::string::npos);
    }

    TEST_CASE("list structured payload contains snippets array and count") {
        ScopedHome guard;
        SnipTool t;
        auto ctx = make_ctx();

        (void)t.run(Json{{"action","save"},{"name","s1"},{"content","c1"}}, ctx);
        (void)t.run(Json{{"action","save"},{"name","s2"},{"content","c2"}}, ctx);

        ToolResult r = t.run(Json{{"action","list"}}, ctx);
        REQUIRE_FALSE(r.is_error);
        REQUIRE(r.structured_payload.has_value());
        CHECK(r.structured_payload->at("count").get<std::size_t>() == 2);
        CHECK(r.structured_payload->at("snippets").is_array());
    }
}

// =============================================================================
// TEST SUITE: delete action
// =============================================================================
TEST_SUITE("SnipTool — delete action") {

    TEST_CASE("delete removes the file") {
        ScopedHome guard;
        SnipTool t;
        auto ctx = make_ctx();

        (void)t.run(Json{{"action","save"},{"name","todelete"},{"content","bye"}}, ctx);
        REQUIRE(fs::exists(guard.snippets_dir() / "todelete.txt"));

        ToolResult r = t.run(Json{{"action","delete"},{"name","todelete"}}, ctx);
        CHECK_FALSE(r.is_error);
        CHECK_FALSE(fs::exists(guard.snippets_dir() / "todelete.txt"));
    }

    TEST_CASE("delete of non-existent snippet returns error") {
        ScopedHome guard;
        SnipTool t;
        auto ctx = make_ctx();

        ToolResult r = t.run(Json{{"action","delete"},{"name","ghost"}}, ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("ghost") != std::string::npos);
    }

    TEST_CASE("deleted snippet no longer appears in list") {
        ScopedHome guard;
        SnipTool t;
        auto ctx = make_ctx();

        (void)t.run(Json{{"action","save"},{"name","gone"},{"content","temporary"}}, ctx);
        (void)t.run(Json{{"action","delete"},{"name","gone"}}, ctx);

        ToolResult r = t.run(Json{{"action","list"}}, ctx);
        CHECK_FALSE(r.is_error);
        CHECK(r.body.find("gone") == std::string::npos);
    }

    TEST_CASE("delete does not affect other snippets") {
        ScopedHome guard;
        SnipTool t;
        auto ctx = make_ctx();

        (void)t.run(Json{{"action","save"},{"name","keep"},{"content","keep me"}}, ctx);
        (void)t.run(Json{{"action","save"},{"name","remove"},{"content","remove me"}}, ctx);

        (void)t.run(Json{{"action","delete"},{"name","remove"}}, ctx);

        // "keep" must still be loadable.
        ToolResult r = t.run(Json{{"action","load"},{"name","keep"}}, ctx);
        REQUIRE_FALSE(r.is_error);
        CHECK(r.body == "keep me");
    }
}

// =============================================================================
// TEST SUITE: cancellation
// =============================================================================
TEST_SUITE("SnipTool — cancellation") {

    TEST_CASE("run() with cancelled token returns error immediately") {
        SnipTool t;
        auto ctx = make_cancelled_ctx();

        ToolResult r = t.run(Json{{"action","list"}}, ctx);
        CHECK(r.is_error);
        CHECK(r.body == "cancelled");
    }

    TEST_CASE("load with cancelled token returns error") {
        ScopedHome guard;
        SnipTool t;

        // Pre-create the file so the error is definitely from cancellation,
        // not from a missing file.
        auto save_ctx = make_ctx();
        (void)t.run(Json{{"action","save"},{"name","cancelme"},{"content","data"}}, save_ctx);

        auto ctx = make_cancelled_ctx();
        ToolResult r = t.run(Json{{"action","load"},{"name","cancelme"}}, ctx);
        CHECK(r.is_error);
        CHECK(r.body == "cancelled");
    }
}

// =============================================================================
// TEST SUITE: round-trip
// =============================================================================
TEST_SUITE("SnipTool — round-trip") {

    TEST_CASE("save → load → delete lifecycle") {
        ScopedHome guard;
        SnipTool t;
        auto ctx = make_ctx();

        const std::string name    = "lifecycle";
        const std::string content = "// This is a code snippet\nint main() { return 0; }\n";

        // Save.
        ToolResult sr = t.run(
            Json{{"action","save"},{"name",name},{"content",content}}, ctx);
        REQUIRE_FALSE(sr.is_error);

        // Load — must match.
        ToolResult lr = t.run(Json{{"action","load"},{"name",name}}, ctx);
        REQUIRE_FALSE(lr.is_error);
        CHECK(lr.body == content);

        // List — must contain the name.
        ToolResult listr = t.run(Json{{"action","list"}}, ctx);
        REQUIRE_FALSE(listr.is_error);
        CHECK(listr.body.find(name) != std::string::npos);

        // Delete.
        ToolResult dr = t.run(Json{{"action","delete"},{"name",name}}, ctx);
        REQUIRE_FALSE(dr.is_error);

        // Load after delete — must be error.
        ToolResult lr2 = t.run(Json{{"action","load"},{"name",name}}, ctx);
        CHECK(lr2.is_error);
    }

    TEST_CASE("multiple snippets saved and retrieved independently") {
        ScopedHome guard;
        SnipTool t;
        auto ctx = make_ctx();

        (void)t.run(Json{{"action","save"},{"name","snip_a"},{"content","content A"}}, ctx);
        (void)t.run(Json{{"action","save"},{"name","snip_b"},{"content","content B"}}, ctx);
        (void)t.run(Json{{"action","save"},{"name","snip_c"},{"content","content C"}}, ctx);

        auto check = [&](const std::string& n, const std::string& expected) {
            ToolResult r = t.run(Json{{"action","load"},{"name",n}}, ctx);
            REQUIRE_FALSE(r.is_error);
            CHECK(r.body == expected);
        };

        check("snip_a", "content A");
        check("snip_b", "content B");
        check("snip_c", "content C");
    }
}
