// tests/unit/test_notepad_tools.cpp
// =============================================================================
// Unit tests for the notepad ITools (DIS-981, S6).
//
// AC2 coverage: a write ITool (notepad_append) and a read/grep ITool
// (notepad_read), both with correct schema + behaviour. The read tool is
// is_read_only()==true; the write tool is is_read_only()==false. Neither
// requires confirmation.  Round-trip: append jots, read returns it, grep
// filters, and the pad is keyed by ToolContext.session_id.
//
// Build standalone (from repo root, x64-linux triplet):
//   c++ -std=c++20 -I include -I build/vcpkg_installed/x64-linux/include \
//       tests/unit/test_notepad_tools.cpp \
//       src/tools/NotepadAppendTool.cpp src/tools/NotepadReadTool.cpp \
//       src/tools/NotepadStore.cpp src/core/Paths.cpp \
//       src/core/CancelToken.cpp src/core/Json.cpp \
//       build/vcpkg_installed/x64-linux/lib/libsimdjson.a \
//       -o /tmp/test_notepad_tools && /tmp/test_notepad_tools
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/tools/NotepadAppendTool.hpp>
#include <batbox/tools/NotepadReadTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>
#include <batbox/core/Json.hpp>

#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace batbox::tools;
using batbox::Json;

namespace {
struct TempRoot {
    fs::path path;
    explicit TempRoot(const char* tag) {
        path = fs::temp_directory_path() /
               ("batbox_notepad_tool_test_" + std::string(tag));
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path, ec);
    }
    ~TempRoot() { std::error_code ec; fs::remove_all(path, ec); }
};

ToolContext make_ctx(const std::string& session_id) {
    ToolContext ctx;
    ctx.cwd        = fs::current_path();
    ctx.session_id = session_id;
    return ctx;
}
} // namespace

TEST_SUITE("notepad tools") {

    // -----------------------------------------------------------------------
    // AC2: identity + permission-gate flags.
    // -----------------------------------------------------------------------
    TEST_CASE("identity and permission flags") {
        NotepadAppendTool append;
        NotepadReadTool    read;

        CHECK(append.name() == "notepad_append");
        CHECK(read.name()   == "notepad_read");

        // Write tool mutates; read tool does not.
        CHECK_FALSE(append.is_read_only());
        CHECK(read.is_read_only());

        // Neither prompts for confirmation.
        CHECK_FALSE(append.requires_confirmation());
        CHECK_FALSE(read.requires_confirmation());
    }

    // -----------------------------------------------------------------------
    // AC2: schema shape — names match, required fields correct.
    // -----------------------------------------------------------------------
    TEST_CASE("schemas are well-formed") {
        NotepadAppendTool append;
        NotepadReadTool    read;

        const Json as = append.schema_json();
        CHECK(as["name"] == "notepad_append");
        CHECK(as["parameters"]["properties"].contains("note"));
        CHECK(as["parameters"]["properties"].contains("section"));
        CHECK(as["parameters"]["required"] == Json::array({"note"}));

        const Json rs = read.schema_json();
        CHECK(rs["name"] == "notepad_read");
        CHECK(rs["parameters"]["properties"].contains("query"));
        // read has no required args (whole-pad read is valid).
        CHECK(rs["parameters"]["required"].empty());
    }

    // -----------------------------------------------------------------------
    // AC2: append → read round-trip, keyed by session_id.
    // -----------------------------------------------------------------------
    TEST_CASE("append then read round-trips under the session key") {
        TempRoot tr("roundtrip");
        NotepadAppendTool append(tr.path);
        NotepadReadTool    read(tr.path);

        auto ctx = make_ctx("session-XYZ");

        Json append_args = Json{{"note", "the deploy hook lives in MAINTENANCE.md"},
                                {"section", "ops"}};
        ToolResult ar = append.run(append_args, ctx);
        CHECK_FALSE(ar.is_error);

        // Read the whole pad back.
        Json read_args = Json::object();
        ToolResult rr = read.run(read_args, ctx);
        CHECK_FALSE(rr.is_error);
        CHECK(rr.body.find("deploy hook lives in MAINTENANCE.md") != std::string::npos);
        CHECK(rr.body.find("## ops") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // AC2: empty note rejected (LEAST_FORCE).
    // -----------------------------------------------------------------------
    TEST_CASE("append rejects an empty note") {
        TempRoot tr("emptynote");
        NotepadAppendTool append(tr.path);
        auto ctx = make_ctx("s");

        Json args = Json{{"note", ""}};
        ToolResult r = append.run(args, ctx);
        CHECK(r.is_error);
    }

    // -----------------------------------------------------------------------
    // AC2: read with a query greps; non-matching entries excluded.
    // -----------------------------------------------------------------------
    TEST_CASE("read with a query greps the pad") {
        TempRoot tr("grep");
        NotepadAppendTool append(tr.path);
        NotepadReadTool    read(tr.path);
        auto ctx = make_ctx("s-grep");

        { Json a = Json{{"note", "auth uses a bearer token"}};   (void)append.run(a, ctx); }
        { Json a = Json{{"note", "the hero image is 1200x630"}}; (void)append.run(a, ctx); }

        Json q = Json{{"query", "auth"}};
        ToolResult r = read.run(q, ctx);
        CHECK_FALSE(r.is_error);
        CHECK(r.body.find("bearer token") != std::string::npos);
        CHECK(r.body.find("hero image")   == std::string::npos);
    }

    // -----------------------------------------------------------------------
    // AC2: read of an empty pad returns a friendly non-error note.
    // -----------------------------------------------------------------------
    TEST_CASE("read of empty pad is a non-error sentinel") {
        TempRoot tr("emptyread");
        NotepadReadTool read(tr.path);
        auto ctx = make_ctx("s-empty");

        Json args = Json::object();
        ToolResult r = read.run(args, ctx);
        CHECK_FALSE(r.is_error);
        CHECK(r.body.find("empty") != std::string::npos);
    }
}
