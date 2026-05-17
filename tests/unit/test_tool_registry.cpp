// tests/unit/test_tool_registry.cpp
//
// doctest suite for batbox::tools::ToolRegistry (CPP 5.2).
//
// Coverage:
//   - register_tool: success, null rejection, duplicate name error
//   - find_by_name: hit, miss, string_view forwarding
//   - available_tool_schemas: no filter, filtered subset, unknown filter name
//   - available_tool_schemas: OpenAI envelope shape {"type":"function","function":{...}}
//   - dispatch: success, unknown tool error, plan-mode gate, allowed_tools gate,
//               exception catch-and-wrap
//   - Acceptance criteria from blueprints table:
//       "Register + get works"
//       "Duplicate name register: error"
//       "all_schemas() returns OpenAI tools[*].function format"

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/tools/ToolRegistry.hpp>
#include <batbox/tools/ITool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/permissions/PermissionMode.hpp>

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

using namespace batbox;
using namespace batbox::tools;
using namespace batbox::permissions;

// =============================================================================
// Test doubles
// =============================================================================

// A configurable mock tool for use in registry tests.
class RegMockTool final : public ITool {
public:
    explicit RegMockTool(std::string name,
                         bool read_only    = false,
                         bool confirm      = true,
                         bool should_throw = false)
        : name_(std::move(name))
        , read_only_(read_only)
        , confirm_(confirm)
        , should_throw_(should_throw) {}

    [[nodiscard]] std::string_view name()        const override { return name_; }
    [[nodiscard]] std::string_view description() const override { return "Mock tool for registry tests."; }

    [[nodiscard]] Json schema_json() const override {
        return Json{
            {"name",        name_},
            {"description", "Mock tool for registry tests."},
            {"parameters",  Json{
                {"type",       "object"},
                {"properties", Json{
                    {"input", Json{{"type", "string"}}}
                }},
                {"required", Json::array({"input"})}
            }}
        };
    }

    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override {
        if (should_throw_) {
            throw std::runtime_error("intentional test exception from " + name_);
        }
        if (ctx.is_cancelled()) {
            return ToolResult::error("cancelled");
        }
        std::string input = args.value("input", std::string{});
        return ToolResult::ok(name_ + ":echo:" + input);
    }

    [[nodiscard]] bool is_read_only()          const override { return read_only_; }
    [[nodiscard]] bool requires_confirmation() const override { return confirm_;   }

private:
    std::string name_;
    bool        read_only_;
    bool        confirm_;
    bool        should_throw_;
};

// Convenience factory.
static std::unique_ptr<RegMockTool> make_tool(std::string name,
                                              bool read_only    = false,
                                              bool should_throw = false)
{
    return std::make_unique<RegMockTool>(std::move(name), read_only, /*confirm=*/true, should_throw);
}

// Build a minimal ToolContext.
static ToolContext make_ctx(PermissionMode mode = PermissionMode::Default) {
    ToolContext ctx;
    ctx.cwd        = std::filesystem::current_path();
    ctx.mode       = mode;
    ctx.session_id = "test-session";
    ctx.agent_id   = "";
    return ctx;
}

// =============================================================================
// TEST SUITE: register_tool
// =============================================================================
TEST_SUITE("ToolRegistry — register_tool") {

    TEST_CASE("register a single tool succeeds") {
        ToolRegistry reg;
        CHECK_NOTHROW(reg.register_tool(make_tool("read_file")));
        CHECK(reg.size() == 1);
    }

    TEST_CASE("register multiple tools succeeds") {
        ToolRegistry reg;
        reg.register_tool(make_tool("read_file"));
        reg.register_tool(make_tool("write_file"));
        reg.register_tool(make_tool("bash"));
        CHECK(reg.size() == 3);
    }

    TEST_CASE("null pointer throws std::invalid_argument") {
        ToolRegistry reg;
        CHECK_THROWS_AS(reg.register_tool(nullptr), std::invalid_argument);
    }

    TEST_CASE("duplicate name throws std::runtime_error") {
        ToolRegistry reg;
        reg.register_tool(make_tool("bash"));
        CHECK_THROWS_AS(reg.register_tool(make_tool("bash")), std::runtime_error);
    }

    TEST_CASE("duplicate name error message contains the tool name") {
        ToolRegistry reg;
        reg.register_tool(make_tool("bash"));
        try {
            reg.register_tool(make_tool("bash"));
            FAIL("Expected exception was not thrown");
        } catch (const std::runtime_error& ex) {
            CHECK(std::string(ex.what()).find("bash") != std::string::npos);
        }
    }

    TEST_CASE("empty() returns true before any registrations") {
        ToolRegistry reg;
        CHECK(reg.empty());
    }

    TEST_CASE("empty() returns false after registration") {
        ToolRegistry reg;
        reg.register_tool(make_tool("read_file"));
        CHECK_FALSE(reg.empty());
    }
}

// =============================================================================
// TEST SUITE: find_by_name
// =============================================================================
TEST_SUITE("ToolRegistry — find_by_name") {

    TEST_CASE("registered tool can be found") {
        ToolRegistry reg;
        reg.register_tool(make_tool("read_file"));
        const ITool* t = reg.find_by_name("read_file");
        REQUIRE(t != nullptr);
        CHECK(t->name() == "read_file");
    }

    TEST_CASE("unregistered name returns nullptr") {
        ToolRegistry reg;
        reg.register_tool(make_tool("read_file"));
        CHECK(reg.find_by_name("bash") == nullptr);
    }

    TEST_CASE("empty registry returns nullptr for any name") {
        ToolRegistry reg;
        CHECK(reg.find_by_name("anything") == nullptr);
    }

    TEST_CASE("find_by_name accepts string_view without copying") {
        ToolRegistry reg;
        reg.register_tool(make_tool("glob"));
        std::string name_str = "glob";
        CHECK(reg.find_by_name(name_str) != nullptr);
        CHECK(reg.find_by_name(std::string_view{"glob"}) != nullptr);
    }

    TEST_CASE("multiple tools, each found by own name") {
        ToolRegistry reg;
        reg.register_tool(make_tool("read_file"));
        reg.register_tool(make_tool("write_file"));
        reg.register_tool(make_tool("bash"));

        REQUIRE(reg.find_by_name("read_file")  != nullptr);
        REQUIRE(reg.find_by_name("write_file") != nullptr);
        REQUIRE(reg.find_by_name("bash")       != nullptr);
        CHECK  (reg.find_by_name("glob")       == nullptr);
    }

    TEST_CASE("find_by_name is case-sensitive") {
        ToolRegistry reg;
        reg.register_tool(make_tool("Bash"));
        CHECK(reg.find_by_name("bash")  == nullptr);   // lowercase miss
        CHECK(reg.find_by_name("Bash")  != nullptr);   // exact match
        CHECK(reg.find_by_name("BASH")  == nullptr);   // uppercase miss
    }
}

// =============================================================================
// TEST SUITE: available_tool_schemas
// =============================================================================
TEST_SUITE("ToolRegistry — available_tool_schemas") {

    TEST_CASE("empty registry yields empty vector") {
        ToolRegistry reg;
        auto schemas = reg.available_tool_schemas();
        CHECK(schemas.empty());
    }

    TEST_CASE("schemas count matches registration count (no filter)") {
        ToolRegistry reg;
        reg.register_tool(make_tool("read_file"));
        reg.register_tool(make_tool("write_file"));
        reg.register_tool(make_tool("bash"));
        auto schemas = reg.available_tool_schemas();
        CHECK(schemas.size() == 3);
    }

    TEST_CASE("each schema has OpenAI envelope: {type:function, function:{...}}") {
        ToolRegistry reg;
        reg.register_tool(make_tool("read_file"));
        auto schemas = reg.available_tool_schemas();
        REQUIRE(schemas.size() == 1);

        const Json& entry = schemas[0];
        REQUIRE(entry.is_object());
        REQUIRE(entry.contains("type"));
        REQUIRE(entry.contains("function"));
        CHECK(entry["type"].get<std::string>() == "function");
        CHECK(entry["function"].is_object());
    }

    TEST_CASE("function object contains name, description, parameters") {
        ToolRegistry reg;
        reg.register_tool(make_tool("bash"));
        auto schemas = reg.available_tool_schemas();
        REQUIRE(schemas.size() == 1);

        const Json& fn = schemas[0]["function"];
        REQUIRE(fn.contains("name"));
        REQUIRE(fn.contains("description"));
        REQUIRE(fn.contains("parameters"));
        CHECK(fn["name"].get<std::string>() == "bash");
        CHECK(fn["parameters"]["type"].get<std::string>() == "object");
    }

    TEST_CASE("schema name matches ITool::name()") {
        ToolRegistry reg;
        reg.register_tool(make_tool("glob"));
        auto schemas = reg.available_tool_schemas();
        REQUIRE(schemas.size() == 1);
        std::string schema_name = schemas[0]["function"]["name"].get<std::string>();
        CHECK(schema_name == "glob");
    }

    TEST_CASE("filter by subset returns only matching tools") {
        ToolRegistry reg;
        reg.register_tool(make_tool("read_file"));
        reg.register_tool(make_tool("write_file"));
        reg.register_tool(make_tool("bash"));

        std::vector<std::string> allow = {"read_file", "bash"};
        auto schemas = reg.available_tool_schemas(allow);
        CHECK(schemas.size() == 2);

        // Both should be in the result.
        bool has_read = false, has_bash = false;
        for (const auto& s : schemas) {
            std::string n = s["function"]["name"].get<std::string>();
            if (n == "read_file")  has_read = true;
            if (n == "bash")       has_bash = true;
        }
        CHECK(has_read);
        CHECK(has_bash);
    }

    TEST_CASE("filter with no matching names returns empty vector") {
        ToolRegistry reg;
        reg.register_tool(make_tool("read_file"));
        reg.register_tool(make_tool("bash"));

        std::vector<std::string> allow = {"nonexistent_tool"};
        auto schemas = reg.available_tool_schemas(allow);
        CHECK(schemas.empty());
    }

    TEST_CASE("std::nullopt filter returns all schemas") {
        ToolRegistry reg;
        reg.register_tool(make_tool("read_file"));
        reg.register_tool(make_tool("bash"));

        auto schemas = reg.available_tool_schemas(std::nullopt);
        CHECK(schemas.size() == 2);
    }

    TEST_CASE("registration order is preserved in schema output") {
        ToolRegistry reg;
        reg.register_tool(make_tool("alpha"));
        reg.register_tool(make_tool("beta"));
        reg.register_tool(make_tool("gamma"));

        auto schemas = reg.available_tool_schemas();
        REQUIRE(schemas.size() == 3);
        CHECK(schemas[0]["function"]["name"].get<std::string>() == "alpha");
        CHECK(schemas[1]["function"]["name"].get<std::string>() == "beta");
        CHECK(schemas[2]["function"]["name"].get<std::string>() == "gamma");
    }
}

// =============================================================================
// TEST SUITE: dispatch
// =============================================================================
TEST_SUITE("ToolRegistry — dispatch") {

    TEST_CASE("dispatch to known tool returns Ok(ToolResult)") {
        ToolRegistry reg;
        reg.register_tool(make_tool("echo_tool"));
        auto ctx  = make_ctx();
        Json args = {{"input", "hello"}};
        auto res  = reg.dispatch("echo_tool", args, ctx);
        REQUIRE(res.has_value());
        CHECK_FALSE(res->is_error);
        CHECK(res->body == "echo_tool:echo:hello");
    }

    TEST_CASE("dispatch to unknown tool returns Err") {
        ToolRegistry reg;
        auto ctx  = make_ctx();
        Json args = Json::object();
        auto res  = reg.dispatch("nonexistent", args, ctx);
        CHECK_FALSE(res.has_value());
        // Error string should mention the tool name.
        CHECK(res.error().find("nonexistent") != std::string::npos);
    }

    TEST_CASE("dispatch in Plan mode to non-read-only tool returns Err") {
        ToolRegistry reg;
        reg.register_tool(make_tool("write_file", /*read_only=*/false));
        auto ctx  = make_ctx(PermissionMode::Plan);
        Json args = {{"input", "x"}};
        auto res  = reg.dispatch("write_file", args, ctx);
        CHECK_FALSE(res.has_value());
        CHECK(res.error().find("Plan mode") != std::string::npos);
    }

    TEST_CASE("dispatch in Plan mode to read-only tool succeeds") {
        ToolRegistry reg;
        reg.register_tool(make_tool("read_file", /*read_only=*/true));
        auto ctx  = make_ctx(PermissionMode::Plan);
        Json args = {{"input", "data"}};
        auto res  = reg.dispatch("read_file", args, ctx);
        REQUIRE(res.has_value());
        CHECK_FALSE(res->is_error);
    }

    TEST_CASE("dispatch respects allowed_tools context filter") {
        ToolRegistry reg;
        reg.register_tool(make_tool("bash"));
        reg.register_tool(make_tool("read_file"));

        auto ctx = make_ctx();
        ctx.allowed_tools = std::vector<std::string>{"read_file"}; // bash NOT allowed

        Json args = {{"input", "test"}};

        // bash should be blocked.
        auto res_bash = reg.dispatch("bash", args, ctx);
        CHECK_FALSE(res_bash.has_value());
        CHECK(res_bash.error().find("allowed_tools") != std::string::npos);

        // read_file should succeed.
        auto res_read = reg.dispatch("read_file", args, ctx);
        REQUIRE(res_read.has_value());
        CHECK_FALSE(res_read->is_error);
    }

    TEST_CASE("dispatch with no allowed_tools restriction permits all tools") {
        ToolRegistry reg;
        reg.register_tool(make_tool("bash"));
        auto ctx = make_ctx();
        ctx.allowed_tools = std::nullopt;
        Json args = {{"input", "x"}};
        auto res = reg.dispatch("bash", args, ctx);
        REQUIRE(res.has_value());
        CHECK_FALSE(res->is_error);
    }

    TEST_CASE("exception from run() is caught and returned as Ok(ToolResult::error)") {
        ToolRegistry reg;
        reg.register_tool(make_tool("bad_tool", /*read_only=*/false, /*should_throw=*/true));
        auto ctx  = make_ctx();
        Json args = {{"input", "trigger"}};
        auto res  = reg.dispatch("bad_tool", args, ctx);
        // dispatch itself succeeded (returned Ok), but the ToolResult is an error.
        REQUIRE(res.has_value());
        CHECK(res->is_error);
        CHECK(res->body.find("intentional test exception") != std::string::npos);
    }

    TEST_CASE("cancelled context — tool returns error body 'cancelled'") {
        ToolRegistry reg;
        reg.register_tool(make_tool("read_file"));

        auto [src, tok] = CancelToken::make_root();
        src.request_stop();

        auto ctx = make_ctx();
        ctx.cancel_token = std::move(tok);

        Json args = {{"input", "data"}};
        auto res = reg.dispatch("read_file", args, ctx);
        REQUIRE(res.has_value());
        CHECK(res->is_error);
        CHECK(res->body == "cancelled");
    }

    TEST_CASE("dispatch in Nuclear mode bypasses confirmation requirement") {
        ToolRegistry reg;
        reg.register_tool(make_tool("bash", /*read_only=*/false));
        auto ctx  = make_ctx(PermissionMode::Nuclear);
        Json args = {{"input", "test"}};
        auto res  = reg.dispatch("bash", args, ctx);
        // Nuclear mode — write tools should still run (not blocked by plan-mode gate).
        REQUIRE(res.has_value());
        CHECK_FALSE(res->is_error);
    }
}

// =============================================================================
// Acceptance criteria smoke tests (named per blueprints table)
// =============================================================================
TEST_SUITE("ToolRegistry — acceptance criteria") {

    TEST_CASE("AC1: Register + get works") {
        ToolRegistry reg;
        reg.register_tool(make_tool("read_file"));
        const ITool* t = reg.find_by_name("read_file");
        REQUIRE(t != nullptr);
        CHECK(t->name() == "read_file");
    }

    TEST_CASE("AC2: Duplicate name register: error") {
        ToolRegistry reg;
        reg.register_tool(make_tool("bash"));
        CHECK_THROWS_AS(reg.register_tool(make_tool("bash")), std::runtime_error);
    }

    TEST_CASE("AC3: all_schemas() returns OpenAI tools[*].function format") {
        ToolRegistry reg;
        reg.register_tool(make_tool("read_file"));
        reg.register_tool(make_tool("bash"));

        auto schemas = reg.available_tool_schemas();
        REQUIRE(schemas.size() == 2);

        for (const Json& entry : schemas) {
            // Outer envelope.
            REQUIRE(entry.contains("type"));
            REQUIRE(entry.contains("function"));
            CHECK(entry["type"].get<std::string>() == "function");

            // Inner function object.
            const Json& fn = entry["function"];
            REQUIRE(fn.contains("name"));
            REQUIRE(fn.contains("description"));
            REQUIRE(fn.contains("parameters"));
            CHECK(fn["parameters"]["type"].get<std::string>() == "object");
        }
    }
}
