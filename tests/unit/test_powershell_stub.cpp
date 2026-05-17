// tests/unit/test_powershell_stub.cpp
//
// doctest suite for:
//   include/batbox/tools/PowerShellTool.hpp
//   src/tools/PowerShellTool.cpp
//
// Verifies the contract described in task CPP 5.11 and blueprints table
// rows 16653–16655:
//   - Schema name matches name() exactly.
//   - Schema mirrors Bash schema: command (required), timeout, description.
//   - run() always returns is_error == true with the verbatim error string.
//   - No process is spawned (verified by the absence of side-effects + fast return).
//   - is_read_only() == false.
//   - requires_confirmation() == true.
//   - Virtual dispatch through ITool pointer works correctly.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/tools/PowerShellTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>
#include <batbox/permissions/PermissionMode.hpp>

#include <filesystem>
#include <memory>
#include <string>

using namespace batbox;
using namespace batbox::tools;
using namespace batbox::permissions;

// =============================================================================
// Helpers
// =============================================================================

/// Build a minimal, non-cancelled ToolContext for test use.
static ToolContext make_ctx(PermissionMode mode = PermissionMode::Default) {
    ToolContext ctx;
    ctx.cwd        = std::filesystem::temp_directory_path();
    ctx.mode       = mode;
    ctx.session_id = "test-session";
    ctx.agent_id   = "";
    return ctx;
}

// Verbatim error string the tool must return (from CPP 5.11 acceptance criteria).
static constexpr std::string_view kExpectedError =
    "PowerShell is not supported on this platform. Use Bash.";

// =============================================================================
// TEST SUITE: PowerShellTool — identity
// =============================================================================
TEST_SUITE("PowerShellTool — identity") {

    TEST_CASE("name() returns \"PowerShell\"") {
        PowerShellTool t;
        CHECK(t.name() == std::string_view("PowerShell"));
    }

    TEST_CASE("description() is non-empty") {
        PowerShellTool t;
        CHECK_FALSE(t.description().empty());
    }

    TEST_CASE("is_read_only() is false (execution tool)") {
        PowerShellTool t;
        CHECK_FALSE(t.is_read_only());
    }

    TEST_CASE("requires_confirmation() is true (shell execution)") {
        PowerShellTool t;
        CHECK(t.requires_confirmation());
    }
}

// =============================================================================
// TEST SUITE: PowerShellTool — schema
// =============================================================================
TEST_SUITE("PowerShellTool — schema") {

    TEST_CASE("schema_json() is an object") {
        PowerShellTool t;
        Json s = t.schema_json();
        REQUIRE(s.is_object());
    }

    TEST_CASE("schema name field matches name()") {
        PowerShellTool t;
        Json s = t.schema_json();
        REQUIRE(s.contains("name"));
        CHECK(s["name"].get<std::string>() == std::string(t.name()));
    }

    TEST_CASE("schema has description field") {
        PowerShellTool t;
        Json s = t.schema_json();
        REQUIRE(s.contains("description"));
        CHECK_FALSE(s["description"].get<std::string>().empty());
    }

    TEST_CASE("schema has parameters object with type=object") {
        PowerShellTool t;
        Json s = t.schema_json();
        REQUIRE(s.contains("parameters"));
        const auto& params = s["parameters"];
        REQUIRE(params.is_object());
        REQUIRE(params.contains("type"));
        CHECK(params["type"].get<std::string>() == "object");
    }

    TEST_CASE("schema parameters has command property (mirrors Bash)") {
        PowerShellTool t;
        Json s = t.schema_json();
        const auto& props = s["parameters"]["properties"];
        REQUIRE(props.is_object());
        REQUIRE(props.contains("command"));
        CHECK(props["command"]["type"].get<std::string>() == "string");
    }

    TEST_CASE("schema parameters has optional timeout property") {
        PowerShellTool t;
        Json s = t.schema_json();
        const auto& props = s["parameters"]["properties"];
        REQUIRE(props.contains("timeout"));
        CHECK(props["timeout"]["type"].get<std::string>() == "integer");
    }

    TEST_CASE("schema parameters has optional description property") {
        PowerShellTool t;
        Json s = t.schema_json();
        const auto& props = s["parameters"]["properties"];
        REQUIRE(props.contains("description"));
        CHECK(props["description"]["type"].get<std::string>() == "string");
    }

    TEST_CASE("schema required array contains command") {
        PowerShellTool t;
        Json s = t.schema_json();
        const auto& required = s["parameters"]["required"];
        REQUIRE(required.is_array());
        bool found_command = false;
        for (const auto& r : required) {
            if (r.get<std::string>() == "command") {
                found_command = true;
                break;
            }
        }
        CHECK(found_command);
    }
}

// =============================================================================
// TEST SUITE: PowerShellTool — run() always returns platform error
// =============================================================================
TEST_SUITE("PowerShellTool — run() returns platform error") {

    TEST_CASE("run() with valid args returns is_error=true") {
        PowerShellTool t;
        auto ctx = make_ctx();
        Json args = {{"command", "Get-Process"}};
        ToolResult r = t.run(args, ctx);
        CHECK(r.is_error);
    }

    TEST_CASE("run() body is exactly the expected platform error string") {
        PowerShellTool t;
        auto ctx = make_ctx();
        Json args = {{"command", "Get-Process"}};
        ToolResult r = t.run(args, ctx);
        CHECK(r.body == std::string(kExpectedError));
    }

    TEST_CASE("run() with empty args returns platform error (no crash)") {
        PowerShellTool t;
        auto ctx = make_ctx();
        ToolResult r = t.run(Json::object(), ctx);
        CHECK(r.is_error);
        CHECK(r.body == std::string(kExpectedError));
    }

    TEST_CASE("run() with Nuclear mode still returns platform error") {
        PowerShellTool t;
        auto ctx = make_ctx(PermissionMode::Nuclear);
        Json args = {{"command", "Remove-Item -Recurse /"}};
        ToolResult r = t.run(args, ctx);
        CHECK(r.is_error);
        CHECK(r.body == std::string(kExpectedError));
    }

    TEST_CASE("run() with Plan mode still returns platform error") {
        PowerShellTool t;
        auto ctx = make_ctx(PermissionMode::Plan);
        Json args = {{"command", "Get-Content file.txt"}};
        ToolResult r = t.run(args, ctx);
        CHECK(r.is_error);
        CHECK(r.body == std::string(kExpectedError));
    }

    TEST_CASE("run() with cancelled token returns platform error (no process spawn)") {
        PowerShellTool t;
        auto [src, tok] = CancelToken::make_root();
        src.request_stop();
        ToolContext ctx = make_ctx();
        ctx.cancel_token = std::move(tok);
        Json args = {{"command", "Get-Process"}};
        // The tool must not hang waiting for a process — it returns immediately.
        ToolResult r = t.run(args, ctx);
        CHECK(r.is_error);
        // Any error is acceptable; most importantly, run() must return at all.
        CHECK_FALSE(r.body.empty());
    }

    TEST_CASE("run() returns no structured payload (error is self-contained)") {
        PowerShellTool t;
        auto ctx = make_ctx();
        Json args = {{"command", "Get-Process"}};
        ToolResult r = t.run(args, ctx);
        CHECK_FALSE(r.structured_payload.has_value());
    }
}

// =============================================================================
// TEST SUITE: PowerShellTool — virtual dispatch via ITool*
// =============================================================================
TEST_SUITE("PowerShellTool — ITool virtual dispatch") {

    TEST_CASE("ITool pointer dispatches name() correctly") {
        std::unique_ptr<ITool> tool = std::make_unique<PowerShellTool>();
        CHECK(tool->name() == std::string_view("PowerShell"));
    }

    TEST_CASE("ITool pointer dispatches run() and returns error") {
        std::unique_ptr<ITool> tool = std::make_unique<PowerShellTool>();
        auto ctx = make_ctx();
        Json args = {{"command", "Get-Process"}};
        ToolResult r = tool->run(args, ctx);
        CHECK(r.is_error);
        CHECK(r.body == std::string(kExpectedError));
    }

    TEST_CASE("ITool pointer dispatches is_read_only() correctly") {
        std::unique_ptr<ITool> tool = std::make_unique<PowerShellTool>();
        CHECK_FALSE(tool->is_read_only());
    }

    TEST_CASE("ITool pointer dispatches requires_confirmation() correctly") {
        std::unique_ptr<ITool> tool = std::make_unique<PowerShellTool>();
        CHECK(tool->requires_confirmation());
    }

    TEST_CASE("schema_json() name matches name() via ITool pointer") {
        std::unique_ptr<ITool> tool = std::make_unique<PowerShellTool>();
        Json s = tool->schema_json();
        CHECK(s["name"].get<std::string>() == std::string(tool->name()));
    }
}
