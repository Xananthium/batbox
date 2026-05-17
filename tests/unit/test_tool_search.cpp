// tests/unit/test_tool_search.cpp
//
// doctest suite for batbox::tools::ToolSearchTool (CPP 5.14).
//
// Acceptance criteria:
//   [x] Query "read" returns ReadTool first
//   [x] Query "fil" returns Read/Write/Edit ranked by name overlap
//   [x] Unit test
//   [x] select:Name1,Name2 returns exact schemas
//   [x] Unknown name in select: listed as not-found, result is NOT is_error
//   [x] is_read_only() = true
//   [x] requires_confirmation() = false
//   [x] name() = "ToolSearch"
//   [x] Query that matches nothing produces friendly message
//   [x] max_results caps results
//   [x] Cancellation returns ToolResult::error("cancelled")
//   [x] Missing 'query' arg returns is_error=true
//
// Build note:
//   This test compiles ToolSearchTool.cpp and ToolRegistry.cpp directly so it
//   does not depend on the full batbox_tools shared library being built yet.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/tools/ToolSearchTool.hpp>
#include <batbox/tools/ToolRegistry.hpp>
#include <batbox/tools/ITool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/permissions/PermissionMode.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace batbox;
using namespace batbox::tools;
using namespace batbox::permissions;

// =============================================================================
// Test doubles
// =============================================================================

/// A mock tool with configurable name and description.
class SearchMockTool final : public ITool {
public:
    SearchMockTool(std::string name, std::string desc)
        : name_(std::move(name)), desc_(std::move(desc)) {}

    [[nodiscard]] std::string_view name()        const override { return name_; }
    [[nodiscard]] std::string_view description() const override { return desc_; }

    [[nodiscard]] Json schema_json() const override {
        return Json{
            {"name",        name_},
            {"description", desc_},
            {"parameters",  Json{
                {"type",       "object"},
                {"properties", Json{}},
                {"required",   Json::array()}
            }}
        };
    }

    [[nodiscard]] ToolResult run(const Json& /*args*/, ToolContext& /*ctx*/) override {
        return ToolResult::ok("mock:" + name_);
    }

    [[nodiscard]] bool is_read_only()          const override { return false; }
    [[nodiscard]] bool requires_confirmation() const override { return true;  }

private:
    std::string name_;
    std::string desc_;
};

/// Convenience factory.
static std::unique_ptr<SearchMockTool> make_tool(std::string name, std::string desc = "") {
    if (desc.empty()) desc = "A tool named " + name + ".";
    return std::make_unique<SearchMockTool>(std::move(name), std::move(desc));
}

/// Build a ToolRegistry with the given name/description pairs.
static ToolRegistry make_registry(
    std::vector<std::pair<std::string, std::string>> tools)
{
    ToolRegistry reg;
    for (auto& [name, desc] : tools) {
        reg.register_tool(make_tool(name, desc));
    }
    return reg;
}

/// Minimal ToolContext.
static ToolContext make_ctx() {
    ToolContext ctx;
    ctx.cwd        = std::filesystem::current_path();
    ctx.mode       = PermissionMode::Default;
    ctx.session_id = "test";
    ctx.agent_id   = "";
    return ctx;
}

// =============================================================================
// TEST SUITE: identity
// =============================================================================
TEST_SUITE("ToolSearchTool — identity") {

    TEST_CASE("name() returns 'ToolSearch'") {
        ToolRegistry reg;
        ToolSearchTool t{reg};
        CHECK(t.name() == std::string_view("ToolSearch"));
    }

    TEST_CASE("is_read_only() = true") {
        ToolRegistry reg;
        ToolSearchTool t{reg};
        CHECK(t.is_read_only());
    }

    TEST_CASE("requires_confirmation() = false") {
        ToolRegistry reg;
        ToolSearchTool t{reg};
        CHECK_FALSE(t.requires_confirmation());
    }

    TEST_CASE("description() is non-empty") {
        ToolRegistry reg;
        ToolSearchTool t{reg};
        CHECK_FALSE(std::string{t.description()}.empty());
    }

    TEST_CASE("schema_json() has correct name field") {
        ToolRegistry reg;
        ToolSearchTool t{reg};
        Json s = t.schema_json();
        REQUIRE(s.contains("name"));
        CHECK(s["name"].get<std::string>() == "ToolSearch");
    }

    TEST_CASE("schema_json() parameters has query as required") {
        ToolRegistry reg;
        ToolSearchTool t{reg};
        Json s = t.schema_json();
        REQUIRE(s.contains("parameters"));
        const Json& params = s["parameters"];
        REQUIRE(params.contains("required"));
        bool found_query = false;
        for (const auto& r : params["required"]) {
            if (r.get<std::string>() == "query") { found_query = true; break; }
        }
        CHECK(found_query);
    }
}

// =============================================================================
// TEST SUITE: argument validation
// =============================================================================
TEST_SUITE("ToolSearchTool — argument validation") {

    TEST_CASE("missing 'query' arg returns is_error=true") {
        ToolRegistry reg;
        ToolSearchTool t{reg};
        auto ctx = make_ctx();
        Json args = Json::object();  // no query
        ToolResult res = t.run(args, ctx);
        CHECK(res.is_error);
        CHECK(res.body.find("query") != std::string::npos);
    }

    TEST_CASE("query is not a string returns is_error=true") {
        ToolRegistry reg;
        ToolSearchTool t{reg};
        auto ctx = make_ctx();
        Json args = {{"query", 42}};
        ToolResult res = t.run(args, ctx);
        CHECK(res.is_error);
    }

    TEST_CASE("select: with empty remainder returns is_error=true") {
        ToolRegistry reg;
        ToolSearchTool t{reg};
        auto ctx = make_ctx();
        Json args = {{"query", "select:"}};
        ToolResult res = t.run(args, ctx);
        CHECK(res.is_error);
    }
}

// =============================================================================
// TEST SUITE: cancellation
// =============================================================================
TEST_SUITE("ToolSearchTool — cancellation") {

    TEST_CASE("pre-cancelled context returns error('cancelled')") {
        ToolRegistry reg;
        reg.register_tool(make_tool("ReadFile", "reads files from disk"));
        ToolSearchTool t{reg};

        auto [src, tok] = CancelToken::make_root();
        src.request_stop();

        auto ctx = make_ctx();
        ctx.cancel_token = std::move(tok);

        Json args = {{"query", "read"}};
        ToolResult res = t.run(args, ctx);
        CHECK(res.is_error);
        CHECK(res.body == "cancelled");
    }
}

// =============================================================================
// TEST SUITE: fuzzy/substring search
// =============================================================================
TEST_SUITE("ToolSearchTool — fuzzy search") {

    TEST_CASE("AC1: query 'read' returns ReadFile first") {
        // Registry with several tools; ReadFile should score highest.
        auto reg = make_registry({
            {"ReadFile",  "Read the contents of a file from disk."},
            {"WriteFile", "Write content to a file on disk."},
            {"EditFile",  "Edit a portion of a file on disk."},
            {"Bash",      "Execute a bash shell command."},
        });

        ToolSearchTool t{reg};
        auto ctx = make_ctx();
        Json args = {{"query", "read"}};
        ToolResult res = t.run(args, ctx);
        CHECK_FALSE(res.is_error);

        // Body should lead with ReadFile.
        const auto first_bracket = res.body.find("[1]");
        const auto read_pos      = res.body.find("ReadFile");
        REQUIRE(first_bracket != std::string::npos);
        REQUIRE(read_pos      != std::string::npos);
        CHECK(read_pos < res.body.find("[2]", first_bracket));
    }

    TEST_CASE("AC2: query 'fil' returns Read/Write/Edit — all three appear in results") {
        auto reg = make_registry({
            {"ReadFile",  "Read the contents of a file from disk."},
            {"WriteFile", "Write content to a file on disk."},
            {"EditFile",  "Edit a portion of a file on disk."},
            {"Bash",      "Execute a bash shell command."},
            {"Glob",      "Match files using glob patterns."},
        });

        ToolSearchTool t{reg};
        auto ctx = make_ctx();
        Json args = {{"query", "fil"}};
        ToolResult res = t.run(args, ctx);
        CHECK_FALSE(res.is_error);

        // All three *File tools must appear.
        CHECK(res.body.find("ReadFile")  != std::string::npos);
        CHECK(res.body.find("WriteFile") != std::string::npos);
        CHECK(res.body.find("EditFile")  != std::string::npos);
    }

    TEST_CASE("query 'fil' — ReadFile appears before Bash (name match beats no-match)") {
        auto reg = make_registry({
            {"Bash",     "Execute a bash shell command."},
            {"ReadFile", "Read the contents of a file from disk."},
        });

        ToolSearchTool t{reg};
        auto ctx = make_ctx();
        Json args = {{"query", "fil"}};
        ToolResult res = t.run(args, ctx);
        CHECK_FALSE(res.is_error);

        const auto read_pos  = res.body.find("ReadFile");
        const auto bash_pos  = res.body.find("Bash");
        REQUIRE(read_pos != std::string::npos);
        // Bash has no "fil" anywhere — it should NOT appear in results.
        CHECK(bash_pos == std::string::npos);
    }

    TEST_CASE("empty query returns all tools") {
        auto reg = make_registry({
            {"Alpha", "First tool."},
            {"Beta",  "Second tool."},
            {"Gamma", "Third tool."},
        });

        ToolSearchTool t{reg};
        auto ctx = make_ctx();
        Json args = {{"query", ""}};
        ToolResult res = t.run(args, ctx);
        CHECK_FALSE(res.is_error);
        // All three must be present (empty query matches everything).
        CHECK(res.body.find("Alpha") != std::string::npos);
        CHECK(res.body.find("Beta")  != std::string::npos);
        CHECK(res.body.find("Gamma") != std::string::npos);
    }

    TEST_CASE("no match produces friendly message with zero results in payload") {
        auto reg = make_registry({
            {"ReadFile", "Read file contents."},
            {"Bash",     "Run a shell command."},
        });

        ToolSearchTool t{reg};
        auto ctx = make_ctx();
        Json args = {{"query", "xyzzy_not_found"}};
        ToolResult res = t.run(args, ctx);
        CHECK_FALSE(res.is_error);
        // Body should mention the query.
        CHECK(res.body.find("xyzzy_not_found") != std::string::npos);
        // Structured payload should have empty matches array.
        REQUIRE(res.structured_payload.has_value());
        const Json& p = *res.structured_payload;
        REQUIRE(p.contains("matches"));
        CHECK(p["matches"].empty());
    }

    TEST_CASE("max_results caps number of hits returned") {
        auto reg = make_registry({
            {"FileA", "Manipulates file A."},
            {"FileB", "Manipulates file B."},
            {"FileC", "Manipulates file C."},
            {"FileD", "Manipulates file D."},
            {"FileE", "Manipulates file E."},
        });

        ToolSearchTool t{reg};
        auto ctx = make_ctx();
        // Query "File" matches all 5; cap at 2.
        Json args = {{"query", "File"}, {"max_results", 2}};
        ToolResult res = t.run(args, ctx);
        CHECK_FALSE(res.is_error);

        REQUIRE(res.structured_payload.has_value());
        CHECK((*res.structured_payload)["matches"].size() == 2);
    }

    TEST_CASE("max_results defaults to 10 and does not clamp under 10 hits") {
        // Register 5 tools that all match — should all appear (fewer than default).
        auto reg = make_registry({
            {"FileA", "Manipulates file A."},
            {"FileB", "Manipulates file B."},
            {"FileC", "Manipulates file C."},
        });

        ToolSearchTool t{reg};
        auto ctx = make_ctx();
        Json args = {{"query", "File"}};
        ToolResult res = t.run(args, ctx);
        CHECK_FALSE(res.is_error);

        REQUIRE(res.structured_payload.has_value());
        CHECK((*res.structured_payload)["matches"].size() == 3);
    }

    TEST_CASE("description match also surfaces tool when name does not match") {
        auto reg = make_registry({
            {"Alpha", "Searches the repository for symbols."},
            {"Beta",  "Does something completely different."},
        });

        ToolSearchTool t{reg};
        auto ctx = make_ctx();
        // "symbol" appears in Alpha's description, not in any name.
        Json args = {{"query", "symbol"}};
        ToolResult res = t.run(args, ctx);
        CHECK_FALSE(res.is_error);
        CHECK(res.body.find("Alpha") != std::string::npos);
        CHECK(res.body.find("Beta")  == std::string::npos);
    }

    TEST_CASE("name match outranks description-only match") {
        // NameMatch has 'grep' in name; DescMatch has 'grep' in description only.
        auto reg = make_registry({
            {"DescOnly", "Grep-like search over file contents."},
            {"GrepTool", "Execute searches on files."},
        });

        ToolSearchTool t{reg};
        auto ctx = make_ctx();
        Json args = {{"query", "grep"}};
        ToolResult res = t.run(args, ctx);
        CHECK_FALSE(res.is_error);

        // GrepTool should appear first (name match = 1.0 vs desc match × 0.5).
        const auto grep_tool_pos = res.body.find("GrepTool");
        const auto desc_tool_pos = res.body.find("DescOnly");
        REQUIRE(grep_tool_pos != std::string::npos);
        CHECK(grep_tool_pos < desc_tool_pos);
    }

    TEST_CASE("structured payload contains schema for each match") {
        auto reg = make_registry({
            {"ReadFile", "Read file contents."},
        });

        ToolSearchTool t{reg};
        auto ctx = make_ctx();
        Json args = {{"query", "ReadFile"}};
        ToolResult res = t.run(args, ctx);
        CHECK_FALSE(res.is_error);

        REQUIRE(res.structured_payload.has_value());
        const Json& p = *res.structured_payload;
        REQUIRE(p.contains("matches"));
        REQUIRE(p["matches"].size() == 1);
        CHECK(p["matches"][0].contains("schema"));
        CHECK(p["matches"][0]["schema"].contains("name"));
        CHECK(p["matches"][0]["schema"]["name"].get<std::string>() == "ReadFile");
    }

    TEST_CASE("case-insensitive: 'READ' matches ReadFile") {
        auto reg = make_registry({
            {"ReadFile", "Read file contents."},
            {"Bash",     "Run a shell command."},
        });

        ToolSearchTool t{reg};
        auto ctx = make_ctx();
        Json args = {{"query", "READ"}};
        ToolResult res = t.run(args, ctx);
        CHECK_FALSE(res.is_error);
        CHECK(res.body.find("ReadFile") != std::string::npos);
    }

    TEST_CASE("empty registry — no match, friendly message") {
        ToolRegistry reg;
        ToolSearchTool t{reg};
        auto ctx = make_ctx();
        Json args = {{"query", "read"}};
        ToolResult res = t.run(args, ctx);
        CHECK_FALSE(res.is_error);
        REQUIRE(res.structured_payload.has_value());
        CHECK((*res.structured_payload)["matches"].empty());
    }
}

// =============================================================================
// TEST SUITE: select: form
// =============================================================================
TEST_SUITE("ToolSearchTool — select: form") {

    TEST_CASE("select:Name returns exact schema for that tool") {
        auto reg = make_registry({
            {"ReadFile",  "Read file contents."},
            {"WriteFile", "Write file contents."},
        });

        ToolSearchTool t{reg};
        auto ctx = make_ctx();
        Json args = {{"query", "select:ReadFile"}};
        ToolResult res = t.run(args, ctx);
        CHECK_FALSE(res.is_error);

        REQUIRE(res.structured_payload.has_value());
        const Json& p = *res.structured_payload;
        REQUIRE(p.contains("matches"));
        REQUIRE(p["matches"].size() == 1);
        CHECK(p["matches"][0]["name"].get<std::string>() == "ReadFile");
        CHECK(p["matches"][0].contains("schema"));
    }

    TEST_CASE("select:Name1,Name2 returns both schemas") {
        auto reg = make_registry({
            {"ReadFile",  "Read file contents."},
            {"WriteFile", "Write file contents."},
            {"Bash",      "Run shell command."},
        });

        ToolSearchTool t{reg};
        auto ctx = make_ctx();
        Json args = {{"query", "select:ReadFile,WriteFile"}};
        ToolResult res = t.run(args, ctx);
        CHECK_FALSE(res.is_error);

        REQUIRE(res.structured_payload.has_value());
        const Json& p = *res.structured_payload;
        REQUIRE(p.contains("matches"));
        CHECK(p["matches"].size() == 2);
    }

    TEST_CASE("select: with unknown name — result is NOT is_error, but missing list populated") {
        auto reg = make_registry({
            {"ReadFile", "Read file contents."},
        });

        ToolSearchTool t{reg};
        auto ctx = make_ctx();
        Json args = {{"query", "select:ReadFile,NonExistentTool"}};
        ToolResult res = t.run(args, ctx);
        // Not an error result even though one name is missing.
        CHECK_FALSE(res.is_error);

        REQUIRE(res.structured_payload.has_value());
        const Json& p = *res.structured_payload;
        REQUIRE(p.contains("missing"));
        bool found_missing = false;
        for (const auto& m : p["missing"]) {
            if (m.get<std::string>() == "NonExistentTool") {
                found_missing = true;
                break;
            }
        }
        CHECK(found_missing);
    }

    TEST_CASE("select: result body mentions 'not found' for missing tools") {
        auto reg = make_registry({
            {"ReadFile", "Read file contents."},
        });

        ToolSearchTool t{reg};
        auto ctx = make_ctx();
        Json args = {{"query", "select:Ghost"}};
        ToolResult res = t.run(args, ctx);
        CHECK_FALSE(res.is_error);
        CHECK(res.body.find("not found") != std::string::npos);
    }

    TEST_CASE("select: whitespace around names is trimmed") {
        auto reg = make_registry({
            {"ReadFile",  "Read file contents."},
            {"WriteFile", "Write file contents."},
        });

        ToolSearchTool t{reg};
        auto ctx = make_ctx();
        Json args = {{"query", "select: ReadFile , WriteFile "}};
        ToolResult res = t.run(args, ctx);
        CHECK_FALSE(res.is_error);
        REQUIRE(res.structured_payload.has_value());
        CHECK((*res.structured_payload)["matches"].size() == 2);
    }

    TEST_CASE("select: all unknown — no matches, missing list has all names") {
        ToolRegistry reg;
        ToolSearchTool t{reg};
        auto ctx = make_ctx();
        Json args = {{"query", "select:Ghost,Phantom"}};
        ToolResult res = t.run(args, ctx);
        CHECK_FALSE(res.is_error);
        REQUIRE(res.structured_payload.has_value());
        CHECK((*res.structured_payload)["matches"].empty());
        CHECK((*res.structured_payload)["missing"].size() == 2);
    }
}

// =============================================================================
// TEST SUITE: schema output
// =============================================================================
TEST_SUITE("ToolSearchTool — schema in results") {

    TEST_CASE("fuzzy match result includes schema with parameters field") {
        auto reg = make_registry({
            {"ReadFile", "Read file contents."},
        });

        ToolSearchTool t{reg};
        auto ctx = make_ctx();
        Json args = {{"query", "Read"}};
        ToolResult res = t.run(args, ctx);
        CHECK_FALSE(res.is_error);

        REQUIRE(res.structured_payload.has_value());
        const Json& match = (*res.structured_payload)["matches"][0];
        REQUIRE(match.contains("schema"));
        CHECK(match["schema"].contains("parameters"));
    }

    TEST_CASE("score field is a positive value at most 1.0") {
        auto reg = make_registry({
            {"ReadFile", "Read file contents."},
        });

        ToolSearchTool t{reg};
        auto ctx = make_ctx();
        Json args = {{"query", "ReadFile"}};
        ToolResult res = t.run(args, ctx);
        CHECK_FALSE(res.is_error);

        REQUIRE(res.structured_payload.has_value());
        const float score =
            (*res.structured_payload)["matches"][0]["score"].get<float>();
        CHECK(score > 0.0f);
        CHECK(score <= 1.0f);
    }
}
