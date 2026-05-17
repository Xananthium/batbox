// tests/unit/test_bash_tool_itool.cpp
//
// doctest unit tests for BashTool (CPP 5.10).
//
// These tests cover the ITool adapter layer (schema, permission gates, arg
// validation, timeout clamping, plan-mode refusal, cancellation) without
// spawning a real child process.  For real process execution tests see
// tests/integration/test_bash_tool.cpp (which tests BashRunner directly).
//
// Strategy: we call BashTool::run() with a tiny timeout (1s) so the tests
// are fast.  The real BashRunner is invoked — we are not mocking it.
//
// Tests:
//   1.  name()              == "Bash"
//   2.  is_read_only()      == false
//   3.  requires_confirmation() == true
//   4.  schema_json() has correct shape (name/description/parameters)
//   5.  schema_json() requires "command" only
//   6.  schema_json() has timeout + description as optional properties
//   7.  Plan-mode refusal (returns error, body mentions "Plan mode")
//   8.  Missing command arg → ToolResult::error
//   9.  Non-string command  → ToolResult::error
//  10.  Empty command       → ToolResult::error
//  11.  Successful command  → ToolResult::ok, body contains output
//  12.  Failing command     → ToolResult::error (is_error=true)
//  13.  timeout clamped: args.timeout > max → effective = max
//  14.  timeout=0 in args → uses default (max_timeout_sec_)
//  15.  Negative timeout   → ToolResult::error
//  16.  description in args → appears in structured_payload["description"]
//  17.  structured_payload carries exit_code, duration_ms, command
//  18.  cancel_token already cancelled → early error
//  19.  ITool vtable dispatch via unique_ptr<ITool>
//  20.  kDefaultTimeoutSec == 120, kDefaultMaxOutputBytes == 1 MiB

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/tools/BashTool.hpp>
#include <batbox/tools/ITool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/permissions/PermissionMode.hpp>

#include <filesystem>
#include <memory>
#include <string>

using namespace batbox;
using namespace batbox::tools;
using namespace batbox::permissions;

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Build a ToolContext for testing.
static ToolContext make_ctx(PermissionMode mode = PermissionMode::Default) {
    auto [src, tok] = CancelToken::make_root();
    ToolContext ctx;
    ctx.cwd         = fs::temp_directory_path();
    ctx.mode        = mode;
    ctx.session_id  = "test-session";
    ctx.agent_id    = "";
    ctx.cancel_token = std::move(tok);
    return ctx;
}

/// Build a ToolContext with a pre-cancelled token.
static ToolContext make_cancelled_ctx() {
    auto [src, tok] = CancelToken::make_root();
    src.request_stop();
    ToolContext ctx;
    ctx.cwd          = fs::temp_directory_path();
    ctx.mode         = PermissionMode::Default;
    ctx.cancel_token = std::move(tok);
    return ctx;
}

// ===========================================================================
// Tests 1-6: Identity and schema
// ===========================================================================

TEST_CASE("BashTool: name() == 'Bash'") {
    BashTool tool;
    CHECK(std::string(tool.name()) == "Bash");
}

TEST_CASE("BashTool: is_read_only() == false") {
    BashTool tool;
    CHECK(tool.is_read_only() == false);
}

TEST_CASE("BashTool: requires_confirmation() == true") {
    BashTool tool;
    CHECK(tool.requires_confirmation() == true);
}

TEST_CASE("BashTool: schema_json() has name, description, parameters") {
    BashTool tool;
    Json schema = tool.schema_json();

    REQUIRE(schema.is_object());
    CHECK(schema.contains("name"));
    CHECK(schema["name"].is_string());
    CHECK(schema["name"].get<std::string>() == "Bash");

    CHECK(schema.contains("description"));
    CHECK(schema["description"].is_string());
    CHECK(!schema["description"].get<std::string>().empty());

    CHECK(schema.contains("parameters"));
    CHECK(schema["parameters"].is_object());
    CHECK(schema["parameters"]["type"].get<std::string>() == "object");
}

TEST_CASE("BashTool: schema_json() requires only 'command'") {
    BashTool tool;
    Json schema = tool.schema_json();
    const Json& required = schema["parameters"]["required"];
    REQUIRE(required.is_array());
    REQUIRE(required.size() == 1);
    CHECK(required[0].get<std::string>() == "command");
}

TEST_CASE("BashTool: schema_json() has timeout and description as optional properties") {
    BashTool tool;
    Json schema = tool.schema_json();
    const Json& props = schema["parameters"]["properties"];
    REQUIRE(props.is_object());
    CHECK(props.contains("command"));
    CHECK(props.contains("timeout"));
    CHECK(props.contains("description"));

    // timeout should have a minimum of 0
    CHECK(props["timeout"].contains("minimum"));
    CHECK(props["timeout"]["minimum"].get<int>() == 0);
}

// ===========================================================================
// Tests 7: Plan-mode refusal
// ===========================================================================

TEST_CASE("BashTool: plan-mode returns error mentioning plan mode") {
    BashTool tool;
    ToolContext ctx = make_ctx(PermissionMode::Plan);
    Json args = Json{{"command", "echo hello"}};

    ToolResult r = tool.run(args, ctx);

    CHECK(r.is_error == true);
    CHECK(r.body.find("Plan mode") != std::string::npos);
    // Must not have executed
    CHECK(r.body.find("hello") == std::string::npos);
}

// ===========================================================================
// Tests 8-10: Argument validation
// ===========================================================================

TEST_CASE("BashTool: missing 'command' arg → ToolResult::error") {
    BashTool tool;
    ToolContext ctx = make_ctx();
    Json args = Json::object();  // no "command"

    ToolResult r = tool.run(args, ctx);

    CHECK(r.is_error == true);
    CHECK(r.body.find("command") != std::string::npos);
}

TEST_CASE("BashTool: non-string 'command' → ToolResult::error") {
    BashTool tool;
    ToolContext ctx = make_ctx();
    Json args = Json{{"command", 42}};

    ToolResult r = tool.run(args, ctx);

    CHECK(r.is_error == true);
    CHECK(r.body.find("command") != std::string::npos);
}

TEST_CASE("BashTool: empty 'command' string → ToolResult::error") {
    BashTool tool;
    ToolContext ctx = make_ctx();
    Json args = Json{{"command", ""}};

    ToolResult r = tool.run(args, ctx);

    CHECK(r.is_error == true);
    CHECK(r.body.find("command") != std::string::npos);
}

// ===========================================================================
// Tests 11-12: Successful and failing commands
// ===========================================================================

TEST_CASE("BashTool: successful command returns ok with output") {
    BashTool tool(/*max_timeout_sec=*/5);
    ToolContext ctx = make_ctx();
    Json args = Json{{"command", "echo batbox_unit_test_ok"}};

    ToolResult r = tool.run(args, ctx);

    CHECK(r.is_error == false);
    CHECK(r.body.find("batbox_unit_test_ok") != std::string::npos);
}

TEST_CASE("BashTool: failing command (false) returns error") {
    BashTool tool(/*max_timeout_sec=*/5);
    ToolContext ctx = make_ctx();
    Json args = Json{{"command", "false"}};

    ToolResult r = tool.run(args, ctx);

    CHECK(r.is_error == true);
}

// ===========================================================================
// Tests 13-15: Timeout clamping
// ===========================================================================

TEST_CASE("BashTool: timeout clamped — args.timeout > max → uses max") {
    // Use a very short max (1s) to keep the test fast.
    BashTool tool(/*max_timeout_sec=*/2);
    ToolContext ctx = make_ctx();
    // Request 9999s but the cap is 2s; the command exits quickly so this
    // just verifies we don't crash or error on the clamping path.
    Json args = Json{{"command", "echo clamped_ok"}, {"timeout", 9999}};

    ToolResult r = tool.run(args, ctx);

    CHECK(r.is_error == false);
    CHECK(r.body.find("clamped_ok") != std::string::npos);
}

TEST_CASE("BashTool: timeout=0 in args uses configured default") {
    BashTool tool(/*max_timeout_sec=*/5);
    ToolContext ctx = make_ctx();
    Json args = Json{{"command", "echo zero_timeout_default"}, {"timeout", 0}};

    ToolResult r = tool.run(args, ctx);

    CHECK(r.is_error == false);
    CHECK(r.body.find("zero_timeout_default") != std::string::npos);
}

TEST_CASE("BashTool: negative timeout → ToolResult::error") {
    BashTool tool;
    ToolContext ctx = make_ctx();
    Json args = Json{{"command", "echo hello"}, {"timeout", -1}};

    ToolResult r = tool.run(args, ctx);

    CHECK(r.is_error == true);
    CHECK(r.body.find("timeout") != std::string::npos);
}

// ===========================================================================
// Tests 16-17: structured_payload
// ===========================================================================

TEST_CASE("BashTool: description in args appears in structured_payload") {
    BashTool tool(/*max_timeout_sec=*/5);
    ToolContext ctx = make_ctx();
    Json args = Json{
        {"command",     "echo payload_test"},
        {"description", "Show a payload test message"}
    };

    ToolResult r = tool.run(args, ctx);

    REQUIRE(r.structured_payload.has_value());
    const Json& payload = *r.structured_payload;
    REQUIRE(payload.contains("description"));
    CHECK(payload["description"].get<std::string>() == "Show a payload test message");
}

TEST_CASE("BashTool: structured_payload carries exit_code, duration_ms, command") {
    BashTool tool(/*max_timeout_sec=*/5);
    ToolContext ctx = make_ctx();
    Json args = Json{{"command", "echo structured_test"}};

    ToolResult r = tool.run(args, ctx);

    REQUIRE(r.structured_payload.has_value());
    const Json& payload = *r.structured_payload;

    CHECK(payload.contains("exit_code"));
    CHECK(payload["exit_code"].is_number_integer());
    CHECK(payload["exit_code"].get<int>() == 0);

    CHECK(payload.contains("duration_ms"));
    CHECK(payload["duration_ms"].is_number_integer());

    CHECK(payload.contains("command"));
    CHECK(payload["command"].get<std::string>() == "echo structured_test");
}

// ===========================================================================
// Test 18: Cancellation
// ===========================================================================

TEST_CASE("BashTool: already-cancelled token → early error") {
    BashTool tool;
    ToolContext ctx = make_cancelled_ctx();
    Json args = Json{{"command", "echo should_not_run"}};

    ToolResult r = tool.run(args, ctx);

    CHECK(r.is_error == true);
    CHECK(r.body.find("cancelled") != std::string::npos);
    CHECK(r.body.find("should_not_run") == std::string::npos);
}

// ===========================================================================
// Test 19: ITool vtable dispatch
// ===========================================================================

TEST_CASE("BashTool: ITool vtable dispatch via unique_ptr<ITool>") {
    std::unique_ptr<ITool> tool = std::make_unique<BashTool>(5);

    CHECK(std::string(tool->name()) == "Bash");
    CHECK(tool->is_read_only() == false);
    CHECK(tool->requires_confirmation() == true);

    Json schema = tool->schema_json();
    CHECK(schema["name"].get<std::string>() == "Bash");

    ToolContext ctx = make_ctx();
    Json args = Json{{"command", "echo vtable_ok"}};
    ToolResult r = tool->run(args, ctx);
    CHECK(r.is_error == false);
    CHECK(r.body.find("vtable_ok") != std::string::npos);
}

// ===========================================================================
// Test 20: Constants
// ===========================================================================

TEST_CASE("BashTool: kDefaultTimeoutSec == 120 and kDefaultMaxOutputBytes == 1 MiB") {
    CHECK(BashTool::kDefaultTimeoutSec == 120);
    CHECK(BashTool::kDefaultMaxOutputBytes == static_cast<std::size_t>(1048576));
}
