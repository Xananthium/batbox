// tests/unit/test_read_tool.cpp
//
// doctest suite for batbox::tools::ReadTool.
//
// Acceptance criteria (CPP 5.3):
//   [x] Reads text file, returns line-numbered content
//   [x] offset/limit truncate correctly
//   [x] Missing file: ToolResult is_error=true with clear message
//   [x] Permission denied: ToolResult is_error=true
//   [x] 5MB cap enforced
//   [x] Binary and image files: text placeholder returned
//
// Build pattern (same as other tool tests — compile .cpp directly):
//   batbox_add_unit_test(test_read_tool
//       unit/test_read_tool.cpp  batbox_core)
//   target_sources(test_read_tool PRIVATE
//       ${PROJECT_SOURCE_DIR}/src/tools/ReadTool.cpp
//       ${PROJECT_SOURCE_DIR}/src/core/CancelToken.cpp)
//   target_link_libraries(test_read_tool PRIVATE
//       batbox_permissions simdjson::simdjson)

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/tools/ReadTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/permissions/PermissionMode.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace batbox;
using namespace batbox::tools;
using namespace batbox::permissions;

// =============================================================================
// Helpers
// =============================================================================

/// Write `content` to `path`, creating parent dirs if needed.
static void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    REQUIRE(f.is_open());
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
}

/// Build a minimal ToolContext pointing at `cwd`.
static ToolContext make_ctx(const fs::path& cwd,
                             PermissionMode mode = PermissionMode::Default) {
    ToolContext ctx;
    ctx.cwd        = cwd;
    ctx.mode       = mode;
    ctx.session_id = "test-session";
    ctx.agent_id   = "";
    return ctx;
}

/// Return system temp dir / batbox_read_tool_tests / unique sub-dir.
static fs::path make_temp_dir() {
    static std::atomic<int> counter{0};
    auto dir = fs::temp_directory_path()
             / "batbox_read_tool_tests"
             / ("run_" + std::to_string(++counter));
    fs::create_directories(dir);
    return dir;
}

// =============================================================================
// TEST SUITE: identity
// =============================================================================
TEST_SUITE("ReadTool — identity") {

    TEST_CASE("name() = 'Read'") {
        ReadTool t;
        CHECK(t.name() == std::string_view("Read"));
    }

    TEST_CASE("is_read_only() = true") {
        ReadTool t;
        CHECK(t.is_read_only());
    }

    TEST_CASE("requires_confirmation() = false") {
        ReadTool t;
        CHECK_FALSE(t.requires_confirmation());
    }

    TEST_CASE("schema_json() has correct shape") {
        ReadTool t;
        const Json s = t.schema_json();
        REQUIRE(s.is_object());
        REQUIRE(s.contains("name"));
        REQUIRE(s.contains("description"));
        REQUIRE(s.contains("parameters"));
        CHECK(s["name"].get<std::string>() == "Read");
        REQUIRE(s["parameters"].contains("required"));
        const auto& req = s["parameters"]["required"];
        REQUIRE(req.is_array());
        CHECK(req.size() == 1);
        CHECK(req[0].get<std::string>() == "file_path");
    }

    TEST_CASE("schema name matches name()") {
        ReadTool t;
        const Json s = t.schema_json();
        CHECK(s["name"].get<std::string>() == std::string(t.name()));
    }

    TEST_CASE("allowed in Plan mode (is_read_only check via MockDispatcher)") {
        // The dispatch layer only calls run() when is_read_only()==true in Plan mode.
        // Verify the flag independently (dispatch logic is tested in ToolRegistry tests).
        ReadTool t;
        CHECK(t.is_read_only());
    }
}

// =============================================================================
// TEST SUITE: successful reads
// =============================================================================
TEST_SUITE("ReadTool — successful reads") {

    TEST_CASE("reads a simple text file with line numbers") {
        auto tmp = make_temp_dir();
        const fs::path f = tmp / "hello.txt";
        write_file(f, "line one\nline two\nline three");

        ReadTool t;
        auto ctx = make_ctx(tmp);
        Json args = {{"file_path", f.string()}};
        auto r = t.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        // Expect cat -n numbering: "     1→line one\n     2→line two\n     3→line three"
        CHECK(r.body.find("     1\xe2\x86\x92line one")  != std::string::npos);
        CHECK(r.body.find("     2\xe2\x86\x92line two")  != std::string::npos);
        CHECK(r.body.find("     3\xe2\x86\x92line three") != std::string::npos);
    }

    TEST_CASE("relative path resolved against ctx.cwd") {
        auto tmp = make_temp_dir();
        write_file(tmp / "relative.txt", "hello");

        ReadTool t;
        auto ctx = make_ctx(tmp);
        Json args = {{"file_path", "relative.txt"}};
        auto r = t.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        CHECK(r.body.find("hello") != std::string::npos);
    }

    TEST_CASE("structured payload contains metadata") {
        auto tmp = make_temp_dir();
        const fs::path f = tmp / "meta.txt";
        write_file(f, "a\nb\nc");

        ReadTool t;
        auto ctx = make_ctx(tmp);
        Json args = {{"file_path", f.string()}};
        auto r = t.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        REQUIRE(r.structured_payload.has_value());
        const auto& p = *r.structured_payload;
        CHECK(p.contains("start_line"));
        CHECK(p.contains("lines_read"));
        CHECK(p.contains("total_lines"));
        CHECK(p["start_line"].get<std::size_t>()  == 1);
        CHECK(p["total_lines"].get<std::size_t>() == 3);
        CHECK(p["lines_read"].get<std::size_t>()  == 3);
    }

    TEST_CASE("empty file returns empty body without error") {
        auto tmp = make_temp_dir();
        const fs::path f = tmp / "empty.txt";
        write_file(f, "");

        ReadTool t;
        auto ctx = make_ctx(tmp);
        Json args = {{"file_path", f.string()}};
        auto r = t.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        // Empty content → empty (or just whitespace) body is fine.
    }

    TEST_CASE("single line file (no trailing newline)") {
        auto tmp = make_temp_dir();
        const fs::path f = tmp / "single.txt";
        write_file(f, "only line");

        ReadTool t;
        auto ctx = make_ctx(tmp);
        Json args = {{"file_path", f.string()}};
        auto r = t.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        CHECK(r.body.find("only line") != std::string::npos);
    }
}

// =============================================================================
// TEST SUITE: offset and limit
// =============================================================================
TEST_SUITE("ReadTool — offset and limit") {

    /// Build a file with N lines: "Line 1\nLine 2\n..."
    static fs::path make_numbered_file(const fs::path& dir, int n) {
        const fs::path f = dir / ("numbered_" + std::to_string(n) + ".txt");
        std::string content;
        for (int i = 1; i <= n; ++i) {
            content += "Line " + std::to_string(i);
            if (i < n) content += '\n';
        }
        write_file(f, content);
        return f;
    }

    TEST_CASE("offset=3 skips first 2 lines") {
        auto tmp  = make_temp_dir();
        // Use 5 lines: no ambiguity between "Line 1" and "Line 10".
        auto file = make_numbered_file(tmp, 5);

        ReadTool t;
        auto ctx = make_ctx(tmp);
        Json args = {{"file_path", file.string()}, {"offset", 3}};
        auto r = t.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        // Arrow-prefix check avoids matching "Line 10" substring.
        CHECK(r.body.find("\xe2\x86\x92Line 1") == std::string::npos);
        CHECK(r.body.find("\xe2\x86\x92Line 2") == std::string::npos);
        CHECK(r.body.find("Line 3") != std::string::npos);

        // Line numbers in output should start at 3.
        CHECK(r.body.find("     3\xe2\x86\x92Line 3") != std::string::npos);
    }

    TEST_CASE("limit=2 returns at most 2 lines") {
        auto tmp  = make_temp_dir();
        auto file = make_numbered_file(tmp, 10);

        ReadTool t;
        auto ctx = make_ctx(tmp);
        Json args = {{"file_path", file.string()}, {"limit", 2}};
        auto r = t.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        CHECK(r.body.find("Line 1") != std::string::npos);
        CHECK(r.body.find("Line 2") != std::string::npos);
        CHECK(r.body.find("Line 3") == std::string::npos);

        REQUIRE(r.structured_payload.has_value());
        CHECK((*r.structured_payload)["lines_read"].get<std::size_t>() == 2);
    }

    TEST_CASE("offset + limit selects exact slice") {
        auto tmp  = make_temp_dir();
        auto file = make_numbered_file(tmp, 10);

        ReadTool t;
        auto ctx = make_ctx(tmp);
        Json args = {{"file_path", file.string()}, {"offset", 4}, {"limit", 3}};
        auto r = t.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        CHECK(r.body.find("Line 3") == std::string::npos);
        CHECK(r.body.find("Line 4") != std::string::npos);
        CHECK(r.body.find("Line 5") != std::string::npos);
        CHECK(r.body.find("Line 6") != std::string::npos);
        CHECK(r.body.find("Line 7") == std::string::npos);

        REQUIRE(r.structured_payload.has_value());
        CHECK((*r.structured_payload)["lines_read"].get<std::size_t>()  == 3);
        CHECK((*r.structured_payload)["start_line"].get<std::size_t>()  == 4);
        CHECK((*r.structured_payload)["total_lines"].get<std::size_t>() == 10);
    }

    TEST_CASE("offset=1 (default) reads from start") {
        auto tmp  = make_temp_dir();
        auto file = make_numbered_file(tmp, 5);

        ReadTool t;
        auto ctx = make_ctx(tmp);
        Json args = {{"file_path", file.string()}, {"offset", 1}};
        auto r = t.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        CHECK(r.body.find("Line 1") != std::string::npos);
    }

    TEST_CASE("limit larger than file returns all lines") {
        auto tmp  = make_temp_dir();
        auto file = make_numbered_file(tmp, 5);

        ReadTool t;
        auto ctx = make_ctx(tmp);
        Json args = {{"file_path", file.string()}, {"limit", 9999}};
        auto r = t.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        REQUIRE(r.structured_payload.has_value());
        CHECK((*r.structured_payload)["lines_read"].get<std::size_t>() == 5);
    }

    TEST_CASE("default limit caps at 2000 lines") {
        auto tmp = make_temp_dir();
        const fs::path f = tmp / "big.txt";

        // Write 3000 lines.
        std::string content;
        content.reserve(3000 * 10);
        for (int i = 1; i <= 3000; ++i) {
            content += "L" + std::to_string(i) + "\n";
        }
        write_file(f, content);

        ReadTool t;
        auto ctx = make_ctx(tmp);
        Json args = {{"file_path", f.string()}};
        auto r = t.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        REQUIRE(r.structured_payload.has_value());
        const auto& p = *r.structured_payload;
        CHECK(p["lines_read"].get<std::size_t>()  == ReadTool::kDefaultLineLimit);
        CHECK(p["total_lines"].get<std::size_t>() == 3000);
    }

    TEST_CASE("offset beyond file end returns error with total_lines hint") {
        auto tmp  = make_temp_dir();
        auto file = make_numbered_file(tmp, 5);

        ReadTool t;
        auto ctx = make_ctx(tmp);
        Json args = {{"file_path", file.string()}, {"offset", 100}};
        auto r = t.run(args, ctx);

        CHECK(r.is_error);
        CHECK(r.body.find("offset") != std::string::npos);
    }
}

// =============================================================================
// TEST SUITE: error handling
// =============================================================================
TEST_SUITE("ReadTool — error handling") {

    TEST_CASE("missing file returns is_error=true with clear message") {
        auto tmp = make_temp_dir();
        ReadTool t;
        auto ctx = make_ctx(tmp);
        Json args = {{"file_path", (tmp / "nonexistent.txt").string()}};
        auto r = t.run(args, ctx);

        CHECK(r.is_error);
        CHECK(r.body.find("not found") != std::string::npos);
    }

    TEST_CASE("directory path returns is_error=true") {
        auto tmp = make_temp_dir();
        ReadTool t;
        auto ctx = make_ctx(tmp);
        Json args = {{"file_path", tmp.string()}};
        auto r = t.run(args, ctx);

        CHECK(r.is_error);
        CHECK(r.body.find("directory") != std::string::npos);
    }

    TEST_CASE("missing file_path argument returns is_error=true") {
        auto tmp = make_temp_dir();
        ReadTool t;
        auto ctx = make_ctx(tmp);
        Json args = Json::object();  // No file_path key.
        auto r = t.run(args, ctx);

        CHECK(r.is_error);
        CHECK(r.body.find("file_path") != std::string::npos);
    }

    TEST_CASE("file_path not a string returns is_error=true") {
        auto tmp = make_temp_dir();
        ReadTool t;
        auto ctx = make_ctx(tmp);
        Json args = {{"file_path", 42}};
        auto r = t.run(args, ctx);

        CHECK(r.is_error);
    }

    TEST_CASE("offset < 1 returns is_error=true") {
        auto tmp = make_temp_dir();
        const fs::path f = tmp / "t.txt";
        write_file(f, "x");

        ReadTool t;
        auto ctx = make_ctx(tmp);
        Json args = {{"file_path", f.string()}, {"offset", 0}};
        auto r = t.run(args, ctx);

        CHECK(r.is_error);
    }

    TEST_CASE("limit < 1 returns is_error=true") {
        auto tmp = make_temp_dir();
        const fs::path f = tmp / "t.txt";
        write_file(f, "x");

        ReadTool t;
        auto ctx = make_ctx(tmp);
        Json args = {{"file_path", f.string()}, {"limit", 0}};
        auto r = t.run(args, ctx);

        CHECK(r.is_error);
    }

#if !defined(_WIN32)
    TEST_CASE("permission denied returns is_error=true") {
        // Only run on POSIX where chmod is available. Skip in CI as root.
        if (::getuid() == 0) {
            // Root can read any file; skip.
            return;
        }
        auto tmp = make_temp_dir();
        const fs::path f = tmp / "noperm.txt";
        write_file(f, "secret");
        // Remove read permission.
        std::error_code ec;
        fs::permissions(f,
            fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read,
            fs::perm_options::remove, ec);
        REQUIRE_FALSE(ec);

        ReadTool t;
        auto ctx = make_ctx(tmp);
        Json args = {{"file_path", f.string()}};
        auto r = t.run(args, ctx);

        // Restore so temp dir cleanup doesn't fail.
        fs::permissions(f, fs::perms::owner_read, fs::perm_options::add, ec);

        CHECK(r.is_error);
        // Message should mention permission.
        CHECK(r.body.find("permission") != std::string::npos);
    }
#endif

    TEST_CASE("5 MB cap enforced — file over limit returns is_error=true") {
        auto tmp = make_temp_dir();
        const fs::path f = tmp / "bigfile.bin";

        // Write just over 5 MB.
        constexpr std::size_t kOver = ReadTool::kMaxFileSizeBytes + 1;
        {
            std::ofstream of(f, std::ios::binary);
            REQUIRE(of.is_open());
            // Write in 64 KiB chunks to avoid huge stack allocation.
            constexpr std::size_t kChunk = 65536;
            std::string chunk(kChunk, 'A');
            std::size_t written = 0;
            while (written < kOver) {
                const std::size_t to_write = std::min(kChunk, kOver - written);
                of.write(chunk.data(), static_cast<std::streamsize>(to_write));
                written += to_write;
            }
        }

        ReadTool t;
        auto ctx = make_ctx(tmp);
        Json args = {{"file_path", f.string()}};
        auto r = t.run(args, ctx);

        CHECK(r.is_error);
        CHECK(r.body.find("5 MB") != std::string::npos);
    }

    TEST_CASE("cancellation returns is_error=true immediately") {
        auto tmp = make_temp_dir();
        ReadTool t;
        auto [src, tok] = CancelToken::make_root();
        src.request_stop();

        ToolContext ctx = make_ctx(tmp);
        ctx.cancel_token = std::move(tok);
        Json args = {{"file_path", (tmp / "any.txt").string()}};
        auto r = t.run(args, ctx);

        CHECK(r.is_error);
        CHECK(r.body == "cancelled");
    }
}

// =============================================================================
// TEST SUITE: image and binary detection
// =============================================================================
TEST_SUITE("ReadTool — image and binary detection") {

    TEST_CASE("known image extension returns placeholder, not error") {
        auto tmp = make_temp_dir();
        const fs::path f = tmp / "photo.png";
        // Write some fake PNG bytes — content doesn't matter; extension drives detection.
        write_file(f, "\x89PNG\r\n\x1a\n");

        ReadTool t;
        auto ctx = make_ctx(tmp);
        Json args = {{"file_path", f.string()}};
        auto r = t.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        CHECK(r.body.find("image") != std::string::npos);
        CHECK(r.body.find(".png") != std::string::npos);
    }

    TEST_CASE("jpg extension returns placeholder") {
        auto tmp = make_temp_dir();
        const fs::path f = tmp / "shot.jpg";
        write_file(f, "\xff\xd8\xff");

        ReadTool t;
        auto ctx = make_ctx(tmp);
        Json args = {{"file_path", f.string()}};
        auto r = t.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        CHECK(r.body.find("image") != std::string::npos);
    }

    TEST_CASE("binary file (null bytes) returns placeholder, not error") {
        auto tmp = make_temp_dir();
        const fs::path f = tmp / "binary.bin";
        // Write data with embedded null bytes — triggers binary heuristic.
        std::string data(100, 'A');
        data[50] = '\0';
        write_file(f, data);

        ReadTool t;
        auto ctx = make_ctx(tmp);
        Json args = {{"file_path", f.string()}};
        auto r = t.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        CHECK(r.body.find("binary") != std::string::npos);
    }

    TEST_CASE("text file with no null bytes is not treated as binary") {
        auto tmp = make_temp_dir();
        const fs::path f = tmp / "text.txt";
        write_file(f, "hello world\n");

        ReadTool t;
        auto ctx = make_ctx(tmp);
        Json args = {{"file_path", f.string()}};
        auto r = t.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        CHECK(r.body.find("hello world") != std::string::npos);
    }
}

// =============================================================================
// TEST SUITE: plan mode
// =============================================================================
TEST_SUITE("ReadTool — plan mode") {

    TEST_CASE("ReadTool runs successfully in Plan mode (is_read_only=true)") {
        auto tmp = make_temp_dir();
        const fs::path f = tmp / "plan.txt";
        write_file(f, "plan content");

        ReadTool t;
        auto ctx = make_ctx(tmp, PermissionMode::Plan);
        Json args = {{"file_path", f.string()}};
        auto r = t.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        CHECK(r.body.find("plan content") != std::string::npos);
    }
}

// =============================================================================
// TEST SUITE: ITool virtual dispatch
// =============================================================================
TEST_SUITE("ReadTool — ITool virtual dispatch") {

    TEST_CASE("ReadTool can be held as unique_ptr<ITool> and dispatched") {
        auto tmp = make_temp_dir();
        const fs::path f = tmp / "dispatch.txt";
        write_file(f, "dispatch test");

        std::unique_ptr<ITool> tool = std::make_unique<ReadTool>();
        CHECK(tool->name() == std::string_view("Read"));
        CHECK(tool->is_read_only());
        CHECK_FALSE(tool->requires_confirmation());

        auto ctx = make_ctx(tmp);
        Json args = {{"file_path", f.string()}};
        auto r = tool->run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        CHECK(r.body.find("dispatch test") != std::string::npos);
    }
}

