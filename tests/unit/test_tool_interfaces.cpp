// tests/unit/test_tool_interfaces.cpp
//
// doctest suite for:
//   include/batbox/tools/ITool.hpp
//   include/batbox/tools/ToolResult.hpp
//   include/batbox/tools/ToolContext.hpp
//
// Verifies the contract described in ned-cpp.md §2.C5 and the blueprint table.
// No external dependencies beyond batbox_core and the three new headers.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/tools/ITool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/permissions/PermissionMode.hpp>

#include <filesystem>
#include <string>

using namespace batbox;
using namespace batbox::tools;
using namespace batbox::permissions;

// =============================================================================
// MockTool — minimal concrete ITool for contract verification.
// =============================================================================

class MockTool final : public ITool {
public:
    explicit MockTool(bool read_only = false, bool confirm = true)
        : read_only_(read_only), confirm_(confirm) {}

    [[nodiscard]] std::string_view name() const override {
        return "mock_tool";
    }

    [[nodiscard]] std::string_view description() const override {
        return "A mock tool used only in unit tests.";
    }

    [[nodiscard]] Json schema_json() const override {
        return Json{
            {"name",        "mock_tool"},
            {"description", "A mock tool used only in unit tests."},
            {"parameters",  Json{
                {"type",       "object"},
                {"properties", Json{
                    {"input", Json{{"type", "string"}}}
                }},
                {"required",   Json::array({"input"})}
            }}
        };
    }

    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override {
        // Honour cancellation immediately.
        if (ctx.is_cancelled()) {
            return ToolResult::error("cancelled");
        }

        // Plan mode: refuse if not read-only.
        if (ctx.is_plan_mode() && !is_read_only()) {
            return ToolResult::error("plan mode: write tools are not allowed");
        }

        // Return the input string back as the body, plus structured payload.
        std::string input = args.value("input", std::string{});
        Json payload = Json{{"echo", input}};
        last_call_args_ = args;
        return ToolResult::ok("echo: " + input, std::move(payload));
    }

    [[nodiscard]] bool is_read_only()          const override { return read_only_; }
    [[nodiscard]] bool requires_confirmation() const override { return confirm_;   }

    // Inspect last call for assertions.
    [[nodiscard]] const Json& last_call_args() const { return last_call_args_; }

private:
    bool read_only_;
    bool confirm_;
    Json last_call_args_;
};

// Read-only variant that does nothing but return ok.
class MockReadOnlyTool final : public ITool {
public:
    [[nodiscard]] std::string_view name()        const override { return "mock_read"; }
    [[nodiscard]] std::string_view description() const override { return "Read-only mock."; }
    [[nodiscard]] Json schema_json() const override {
        return Json{
            {"name",        "mock_read"},
            {"description", "Read-only mock."},
            {"parameters",  Json{{"type","object"},{"properties",Json::object()}}}
        };
    }
    [[nodiscard]] ToolResult run(const Json&, ToolContext&) override {
        return ToolResult::ok("read ok");
    }
    [[nodiscard]] bool is_read_only()          const override { return true;  }
    [[nodiscard]] bool requires_confirmation() const override { return false; }
};

// =============================================================================
// Helper: build a minimal ToolContext.
// =============================================================================

static ToolContext make_ctx(PermissionMode mode = PermissionMode::Default) {
    ToolContext ctx;
    ctx.cwd        = std::filesystem::current_path();
    ctx.mode       = mode;
    ctx.session_id = "test-session";
    ctx.agent_id   = "";
    // cancel_token default-constructed = never cancelled.
    return ctx;
}

// =============================================================================
// TEST SUITE: ToolResult
// =============================================================================
TEST_SUITE("ToolResult — construction and helpers") {

    TEST_CASE("default construction yields empty non-error result") {
        ToolResult r;
        CHECK(r.body.empty());
        CHECK_FALSE(r.is_error);
        CHECK_FALSE(r.structured_payload.has_value());
    }

    TEST_CASE("ToolResult::ok(body) — is_error=false, no payload") {
        auto r = ToolResult::ok("hello");
        CHECK(r.body == "hello");
        CHECK_FALSE(r.is_error);
        CHECK_FALSE(r.structured_payload.has_value());
    }

    TEST_CASE("ToolResult::ok(body, payload) — is_error=false, payload present") {
        auto r = ToolResult::ok("data", Json{{"key", 42}});
        CHECK(r.body == "data");
        CHECK_FALSE(r.is_error);
        REQUIRE(r.structured_payload.has_value());
        CHECK(r.structured_payload->at("key") == 42);
    }

    TEST_CASE("ToolResult::error(body) — is_error=true, no payload") {
        auto r = ToolResult::error("something went wrong");
        CHECK(r.body == "something went wrong");
        CHECK(r.is_error);
        CHECK_FALSE(r.structured_payload.has_value());
    }

    TEST_CASE("ToolResult::error(body, payload) — is_error=true, payload present") {
        auto r = ToolResult::error("fail", Json{{"code", 404}});
        CHECK(r.body == "fail");
        CHECK(r.is_error);
        REQUIRE(r.structured_payload.has_value());
        CHECK(r.structured_payload->at("code") == 404);
    }

    TEST_CASE("equality operator") {
        auto a = ToolResult::ok("x");
        auto b = ToolResult::ok("x");
        auto c = ToolResult::error("x");
        CHECK(a == b);
        CHECK(a != c);
    }

    TEST_CASE("direct aggregate construction") {
        ToolResult r{"body text", true, std::nullopt};
        CHECK(r.body == "body text");
        CHECK(r.is_error);
        CHECK_FALSE(r.structured_payload.has_value());
    }
}

// =============================================================================
// TEST SUITE: ToolContext
// =============================================================================
TEST_SUITE("ToolContext — fields and helpers") {

    TEST_CASE("default construction has default mode and empty ids") {
        ToolContext ctx;
        CHECK(ctx.mode == PermissionMode::Default);
        CHECK(ctx.session_id.empty());
        CHECK(ctx.agent_id.empty());
        CHECK_FALSE(ctx.allowed_tools.has_value());
        CHECK_FALSE(ctx.is_cancelled());
    }

    TEST_CASE("is_plan_mode() is true only for Plan mode") {
        CHECK(make_ctx(PermissionMode::Plan).is_plan_mode());
        CHECK_FALSE(make_ctx(PermissionMode::Default).is_plan_mode());
        CHECK_FALSE(make_ctx(PermissionMode::AcceptEdits).is_plan_mode());
        CHECK_FALSE(make_ctx(PermissionMode::Nuclear).is_plan_mode());
    }

    TEST_CASE("is_nuclear() is true only for Nuclear mode") {
        CHECK(make_ctx(PermissionMode::Nuclear).is_nuclear());
        CHECK_FALSE(make_ctx(PermissionMode::Default).is_nuclear());
        CHECK_FALSE(make_ctx(PermissionMode::Plan).is_nuclear());
        CHECK_FALSE(make_ctx(PermissionMode::AcceptEdits).is_nuclear());
    }

    TEST_CASE("tool_is_allowed() — absent list means all tools allowed") {
        auto ctx = make_ctx();
        ctx.allowed_tools = std::nullopt;
        CHECK(ctx.tool_is_allowed("anything"));
        CHECK(ctx.tool_is_allowed("bash"));
    }

    TEST_CASE("tool_is_allowed() — present list enforces membership") {
        auto ctx = make_ctx();
        ctx.allowed_tools = std::vector<std::string>{"read_file", "glob"};
        CHECK(ctx.tool_is_allowed("read_file"));
        CHECK(ctx.tool_is_allowed("glob"));
        CHECK_FALSE(ctx.tool_is_allowed("bash"));
        CHECK_FALSE(ctx.tool_is_allowed("write_file"));
    }

    TEST_CASE("is_cancelled() reflects cancel_token state") {
        auto [src, tok] = CancelToken::make_root();
        ToolContext ctx = make_ctx();
        ctx.cancel_token = std::move(tok);
        CHECK_FALSE(ctx.is_cancelled());
        src.request_stop();
        CHECK(ctx.is_cancelled());
    }

    TEST_CASE("cwd field is accessible and assignable") {
        auto ctx = make_ctx();
        ctx.cwd = std::filesystem::temp_directory_path();
        CHECK(ctx.cwd == std::filesystem::temp_directory_path());
    }
}

// =============================================================================
// TEST SUITE: ITool contract via MockTool
// =============================================================================
TEST_SUITE("ITool — interface contract via MockTool") {

    TEST_CASE("name() and description() return expected string_views") {
        MockTool t;
        CHECK(t.name()        == std::string_view("mock_tool"));
        CHECK(t.description() == std::string_view("A mock tool used only in unit tests."));
    }

    TEST_CASE("schema_json() returns object with name, description, parameters") {
        MockTool t;
        Json s = t.schema_json();
        REQUIRE(s.is_object());
        REQUIRE(s.contains("name"));
        REQUIRE(s.contains("description"));
        REQUIRE(s.contains("parameters"));
        CHECK(s["name"].get<std::string>() == "mock_tool");
        CHECK(s["parameters"]["type"].get<std::string>() == "object");
    }

    TEST_CASE("schema name matches name()") {
        MockTool t;
        Json s = t.schema_json();
        std::string schema_name = s["name"].get<std::string>();
        CHECK(schema_name == std::string(t.name()));
    }

    TEST_CASE("default is_read_only() = false") {
        MockTool t;
        CHECK_FALSE(t.is_read_only());
    }

    TEST_CASE("default requires_confirmation() = true") {
        MockTool t;
        CHECK(t.requires_confirmation());
    }

    TEST_CASE("overridden is_read_only() = true") {
        MockReadOnlyTool t;
        CHECK(t.is_read_only());
    }

    TEST_CASE("overridden requires_confirmation() = false") {
        MockReadOnlyTool t;
        CHECK_FALSE(t.requires_confirmation());
    }

    TEST_CASE("run() succeeds and echoes input") {
        MockTool t;
        auto ctx = make_ctx();
        Json args = {{"input", "hello world"}};
        ToolResult r = t.run(args, ctx);
        CHECK_FALSE(r.is_error);
        CHECK(r.body == "echo: hello world");
        REQUIRE(r.structured_payload.has_value());
        CHECK(r.structured_payload->at("echo") == "hello world");
    }

    TEST_CASE("run() respects cancellation — returns error immediately") {
        MockTool t;
        auto [src, tok] = CancelToken::make_root();
        src.request_stop();
        ToolContext ctx = make_ctx();
        ctx.cancel_token = std::move(tok);
        Json args = {{"input", "should not execute"}};
        ToolResult r = t.run(args, ctx);
        CHECK(r.is_error);
        CHECK(r.body == "cancelled");
    }

    TEST_CASE("run() in Plan mode — non-read-only tool returns error") {
        MockTool t(/*read_only=*/false);
        auto ctx = make_ctx(PermissionMode::Plan);
        Json args = {{"input", "test"}};
        ToolResult r = t.run(args, ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("plan mode") != std::string::npos);
    }

    TEST_CASE("read-only tool runs successfully in Plan mode") {
        MockReadOnlyTool t;
        auto ctx = make_ctx(PermissionMode::Plan);
        ToolResult r = t.run(Json::object(), ctx);
        CHECK_FALSE(r.is_error);
        CHECK(r.body == "read ok");
    }

    TEST_CASE("ITool pointer — virtual dispatch works") {
        // Ownership pattern used by ToolRegistry: unique_ptr<ITool>.
        std::unique_ptr<ITool> tool = std::make_unique<MockTool>();
        CHECK(tool->name() == std::string_view("mock_tool"));
        CHECK_FALSE(tool->is_read_only());
        CHECK(tool->requires_confirmation());

        auto ctx = make_ctx();
        Json args = {{"input", "dispatch"}};
        ToolResult r = tool->run(args, ctx);
        CHECK_FALSE(r.is_error);
        CHECK(r.body == "echo: dispatch");
    }

    TEST_CASE("multiple tools have independent state") {
        MockTool a;
        MockTool b;
        auto ctx = make_ctx();

        Json args_a = {{"input", "alpha"}};
        Json args_b = {{"input", "beta"}};
        ToolResult ra = a.run(args_a, ctx);
        ToolResult rb = b.run(args_b, ctx);

        CHECK(ra.body == "echo: alpha");
        CHECK(rb.body == "echo: beta");
        // Confirm last_call_args recorded independently.
        CHECK(a.last_call_args()["input"] == "alpha");
        CHECK(b.last_call_args()["input"] == "beta");
    }
}
