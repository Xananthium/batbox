// tests/unit/test_ctx_inspect.cpp
//
// doctest suite for batbox::tools::CtxInspectTool (CPP 5.24).
//
// Acceptance criteria:
//   [x] Returns JSON with all listed fields
//   [x] No side effects
//   [x] is_read_only() == true
//   [x] requires_confirmation() == false
//   [x] name() == "CtxInspect"
//   [x] schema_json() has correct OpenAI structure
//   [x] pct_used computed correctly and clamped to [0.0, 100.0]
//   [x] Missing args produce safe defaults
//   [x] ctx fields (cwd, mode, session_id, agent_id) appear in output
//   [x] tools_available array forwarded correctly
//   [x] tool_call_count forwarded correctly
//   [x] result body is valid JSON round-trippable to structured_payload

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/tools/CtxInspectTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/permissions/PermissionMode.hpp>

#include <filesystem>
#include <tuple>

using namespace batbox;
using namespace batbox::tools;
using namespace batbox::permissions;

// =============================================================================
// Helper: build a minimal ToolContext
// =============================================================================

static ToolContext make_ctx(PermissionMode mode = PermissionMode::Default) {
    ToolContext ctx;
    ctx.cwd        = std::filesystem::path("/tmp/batbox_test");
    ctx.mode       = mode;
    ctx.session_id = "sess-abc";
    ctx.agent_id   = "agent-xyz";
    return ctx;
}

// =============================================================================
// Helper: build a typical args object
// =============================================================================

static Json make_args(int msg_count   = 10,
                      int est_tokens  = 4000,
                      int model_limit = 200000,
                      int tc_count    = 3) {
    return Json{
        {"message_count",       msg_count},
        {"estimated_tokens",    est_tokens},
        {"model_context_limit", model_limit},
        {"tool_call_count",     tc_count},
        {"tools_available",     Json::array({"Glob", "Write", "CtxInspect"})}
    };
}

// =============================================================================
// TEST SUITE: identity contract
// =============================================================================
TEST_SUITE("CtxInspectTool — identity contract") {

    TEST_CASE("name() returns 'CtxInspect'") {
        CtxInspectTool t;
        CHECK(t.name() == std::string_view("CtxInspect"));
    }

    TEST_CASE("description() is non-empty") {
        CtxInspectTool t;
        CHECK_FALSE(t.description().empty());
    }

    TEST_CASE("is_read_only() returns true") {
        CtxInspectTool t;
        CHECK(t.is_read_only());
    }

    TEST_CASE("requires_confirmation() returns false") {
        CtxInspectTool t;
        CHECK_FALSE(t.requires_confirmation());
    }
}

// =============================================================================
// TEST SUITE: schema_json contract
// =============================================================================
TEST_SUITE("CtxInspectTool — schema_json") {

    TEST_CASE("schema has name, description, parameters keys") {
        CtxInspectTool t;
        Json s = t.schema_json();
        REQUIRE(s.is_object());
        CHECK(s.contains("name"));
        CHECK(s.contains("description"));
        CHECK(s.contains("parameters"));
    }

    TEST_CASE("schema name matches tool name()") {
        CtxInspectTool t;
        Json s = t.schema_json();
        CHECK(s["name"].get<std::string>() == std::string(t.name()));
    }

    TEST_CASE("parameters type is 'object'") {
        CtxInspectTool t;
        Json s = t.schema_json();
        CHECK(s["parameters"]["type"].get<std::string>() == "object");
    }

    TEST_CASE("parameters.properties contains all five documented fields") {
        CtxInspectTool t;
        Json props = t.schema_json()["parameters"]["properties"];
        CHECK(props.contains("message_count"));
        CHECK(props.contains("estimated_tokens"));
        CHECK(props.contains("model_context_limit"));
        CHECK(props.contains("tool_call_count"));
        CHECK(props.contains("tools_available"));
    }

    TEST_CASE("required array is present and is an array") {
        CtxInspectTool t;
        Json s = t.schema_json();
        REQUIRE(s["parameters"].contains("required"));
        CHECK(s["parameters"]["required"].is_array());
    }
}

// =============================================================================
// TEST SUITE: run() — all fields present in result
// =============================================================================
TEST_SUITE("CtxInspectTool::run — field presence") {

    TEST_CASE("result body is non-error") {
        CtxInspectTool t;
        auto ctx  = make_ctx();
        auto args = make_args();
        ToolResult r = t.run(args, ctx);
        CHECK_FALSE(r.is_error);
    }

    TEST_CASE("structured_payload is present") {
        CtxInspectTool t;
        auto ctx  = make_ctx();
        auto args = make_args();
        ToolResult r = t.run(args, ctx);
        REQUIRE(r.structured_payload.has_value());
    }

    TEST_CASE("all ten fields present in payload") {
        CtxInspectTool t;
        auto ctx  = make_ctx();
        auto args = make_args();
        ToolResult r = t.run(args, ctx);
        REQUIRE(r.structured_payload.has_value());
        const Json& p = *r.structured_payload;
        CHECK(p.contains("message_count"));
        CHECK(p.contains("estimated_tokens"));
        CHECK(p.contains("model_context_limit"));
        CHECK(p.contains("pct_used"));
        CHECK(p.contains("tool_call_count"));
        CHECK(p.contains("tools_available"));
        CHECK(p.contains("cwd"));
        CHECK(p.contains("permission_mode"));
        CHECK(p.contains("session_id"));
        CHECK(p.contains("agent_id"));
    }

    TEST_CASE("body parses to the same JSON as structured_payload") {
        CtxInspectTool t;
        auto ctx  = make_ctx();
        auto args = make_args();
        ToolResult r = t.run(args, ctx);
        REQUIRE(r.structured_payload.has_value());
        // The body should be valid JSON equal to the structured payload.
        Json parsed = Json::parse(r.body);
        CHECK(parsed == *r.structured_payload);
    }
}

// =============================================================================
// TEST SUITE: run() — field values
// =============================================================================
TEST_SUITE("CtxInspectTool::run — field values") {

    TEST_CASE("message_count forwarded from args") {
        CtxInspectTool t;
        auto ctx  = make_ctx();
        ToolResult r = t.run(make_args(/*msg=*/42), ctx);
        REQUIRE(r.structured_payload.has_value());
        CHECK((*r.structured_payload)["message_count"].get<int>() == 42);
    }

    TEST_CASE("estimated_tokens forwarded from args") {
        CtxInspectTool t;
        auto ctx  = make_ctx();
        ToolResult r = t.run(make_args(10, /*est=*/15000), ctx);
        REQUIRE(r.structured_payload.has_value());
        CHECK((*r.structured_payload)["estimated_tokens"].get<int>() == 15000);
    }

    TEST_CASE("model_context_limit forwarded from args") {
        CtxInspectTool t;
        auto ctx  = make_ctx();
        ToolResult r = t.run(make_args(10, 4000, /*limit=*/128000), ctx);
        REQUIRE(r.structured_payload.has_value());
        CHECK((*r.structured_payload)["model_context_limit"].get<int>() == 128000);
    }

    TEST_CASE("pct_used computed correctly: 4000/200000 * 100 = 2.0") {
        CtxInspectTool t;
        auto ctx  = make_ctx();
        // 4000 / 200000 = 0.02 → 2.0%
        ToolResult r = t.run(make_args(10, 4000, 200000, 3), ctx);
        REQUIRE(r.structured_payload.has_value());
        double pct = (*r.structured_payload)["pct_used"].get<double>();
        CHECK(pct == doctest::Approx(2.0).epsilon(1e-9));
    }

    TEST_CASE("pct_used is 0.0 when model_context_limit is 0 (avoid divide-by-zero)") {
        CtxInspectTool t;
        auto ctx  = make_ctx();
        ToolResult r = t.run(make_args(10, 5000, /*limit=*/0), ctx);
        REQUIRE(r.structured_payload.has_value());
        double pct = (*r.structured_payload)["pct_used"].get<double>();
        CHECK(pct == doctest::Approx(0.0).epsilon(1e-9));
    }

    TEST_CASE("pct_used clamped to 100.0 when tokens exceed limit") {
        CtxInspectTool t;
        auto ctx  = make_ctx();
        // 300000 / 200000 = 150% → clamped to 100.0
        ToolResult r = t.run(make_args(10, 300000, 200000), ctx);
        REQUIRE(r.structured_payload.has_value());
        double pct = (*r.structured_payload)["pct_used"].get<double>();
        CHECK(pct == doctest::Approx(100.0).epsilon(1e-9));
    }

    TEST_CASE("tool_call_count forwarded from args") {
        CtxInspectTool t;
        auto ctx  = make_ctx();
        ToolResult r = t.run(make_args(10, 4000, 200000, /*tc=*/7), ctx);
        REQUIRE(r.structured_payload.has_value());
        CHECK((*r.structured_payload)["tool_call_count"].get<int>() == 7);
    }

    TEST_CASE("tools_available array forwarded from args") {
        CtxInspectTool t;
        auto ctx  = make_ctx();
        ToolResult r = t.run(make_args(), ctx);
        REQUIRE(r.structured_payload.has_value());
        const Json& ta = (*r.structured_payload)["tools_available"];
        REQUIRE(ta.is_array());
        CHECK(ta.size() == 3);
        CHECK(ta[0].get<std::string>() == "Glob");
        CHECK(ta[1].get<std::string>() == "Write");
        CHECK(ta[2].get<std::string>() == "CtxInspect");
    }

    TEST_CASE("cwd reflects ctx.cwd") {
        CtxInspectTool t;
        auto ctx  = make_ctx();
        ctx.cwd   = std::filesystem::path("/home/user/project");
        ToolResult r = t.run(make_args(), ctx);
        REQUIRE(r.structured_payload.has_value());
        CHECK((*r.structured_payload)["cwd"].get<std::string>() == "/home/user/project");
    }

    TEST_CASE("permission_mode reflects ctx.mode — Default") {
        CtxInspectTool t;
        auto ctx  = make_ctx(PermissionMode::Default);
        ToolResult r = t.run(make_args(), ctx);
        REQUIRE(r.structured_payload.has_value());
        CHECK((*r.structured_payload)["permission_mode"].get<std::string>() == "default");
    }

    TEST_CASE("permission_mode reflects ctx.mode — Plan") {
        CtxInspectTool t;
        auto ctx  = make_ctx(PermissionMode::Plan);
        ToolResult r = t.run(make_args(), ctx);
        REQUIRE(r.structured_payload.has_value());
        CHECK((*r.structured_payload)["permission_mode"].get<std::string>() == "plan");
    }

    TEST_CASE("permission_mode reflects ctx.mode — AcceptEdits") {
        CtxInspectTool t;
        auto ctx  = make_ctx(PermissionMode::AcceptEdits);
        ToolResult r = t.run(make_args(), ctx);
        REQUIRE(r.structured_payload.has_value());
        CHECK((*r.structured_payload)["permission_mode"].get<std::string>() == "acceptedits");
    }

    TEST_CASE("permission_mode reflects ctx.mode — Nuclear") {
        CtxInspectTool t;
        auto ctx  = make_ctx(PermissionMode::Nuclear);
        ToolResult r = t.run(make_args(), ctx);
        REQUIRE(r.structured_payload.has_value());
        CHECK((*r.structured_payload)["permission_mode"].get<std::string>() == "nuclear");
    }

    TEST_CASE("session_id reflects ctx.session_id") {
        CtxInspectTool t;
        auto ctx       = make_ctx();
        ctx.session_id = "my-session-123";
        ToolResult r   = t.run(make_args(), ctx);
        REQUIRE(r.structured_payload.has_value());
        CHECK((*r.structured_payload)["session_id"].get<std::string>() == "my-session-123");
    }

    TEST_CASE("agent_id reflects ctx.agent_id") {
        CtxInspectTool t;
        auto ctx     = make_ctx();
        ctx.agent_id = "sub-agent-007";
        ToolResult r = t.run(make_args(), ctx);
        REQUIRE(r.structured_payload.has_value());
        CHECK((*r.structured_payload)["agent_id"].get<std::string>() == "sub-agent-007");
    }

    TEST_CASE("agent_id is empty string for root conversation") {
        CtxInspectTool t;
        auto ctx     = make_ctx();
        ctx.agent_id = "";
        ToolResult r = t.run(make_args(), ctx);
        REQUIRE(r.structured_payload.has_value());
        CHECK((*r.structured_payload)["agent_id"].get<std::string>().empty());
    }
}

// =============================================================================
// TEST SUITE: run() — safe defaults when args are empty
// =============================================================================
TEST_SUITE("CtxInspectTool::run — safe defaults with empty args") {

    TEST_CASE("empty args produces successful result with zero/empty defaults") {
        CtxInspectTool t;
        auto ctx  = make_ctx();
        Json empty_args = Json::object();
        ToolResult r = t.run(empty_args, ctx);
        CHECK_FALSE(r.is_error);
        REQUIRE(r.structured_payload.has_value());
        const Json& p = *r.structured_payload;
        CHECK(p["message_count"].get<int>()        == 0);
        CHECK(p["estimated_tokens"].get<int>()     == 0);
        CHECK(p["model_context_limit"].get<int>()  == 0);
        CHECK(p["pct_used"].get<double>()          == doctest::Approx(0.0));
        CHECK(p["tool_call_count"].get<int>()      == 0);
        CHECK(p["tools_available"].is_array());
        CHECK(p["tools_available"].empty());
    }

    TEST_CASE("null / wrong-type args fields fall back to defaults") {
        CtxInspectTool t;
        auto ctx  = make_ctx();
        // Intentionally wrong types — should not crash, should use defaults.
        Json bad_args{
            {"message_count",       "not_an_int"},
            {"estimated_tokens",    nullptr},
            {"model_context_limit", true},
            {"tools_available",     "not_an_array"}
        };
        ToolResult r = t.run(bad_args, ctx);
        CHECK_FALSE(r.is_error);
        REQUIRE(r.structured_payload.has_value());
        const Json& p = *r.structured_payload;
        // All bad-type fields fall back to 0 or empty.
        CHECK(p["message_count"].get<int>()       == 0);
        CHECK(p["estimated_tokens"].get<int>()    == 0);
        CHECK(p["model_context_limit"].get<int>() == 0);
        CHECK(p["tools_available"].is_array());
        CHECK(p["tools_available"].empty());
    }
}

// =============================================================================
// TEST SUITE: run() — plan mode and cancellation
// =============================================================================
TEST_SUITE("CtxInspectTool::run — plan mode and cancellation") {

    TEST_CASE("run succeeds in Plan mode (read-only tool)") {
        CtxInspectTool t;
        auto ctx  = make_ctx(PermissionMode::Plan);
        ToolResult r = t.run(make_args(), ctx);
        CHECK_FALSE(r.is_error);
        REQUIRE(r.structured_payload.has_value());
        CHECK((*r.structured_payload)["permission_mode"].get<std::string>() == "plan");
    }

    TEST_CASE("run succeeds in Nuclear mode") {
        CtxInspectTool t;
        auto ctx  = make_ctx(PermissionMode::Nuclear);
        ToolResult r = t.run(make_args(), ctx);
        CHECK_FALSE(r.is_error);
    }

    TEST_CASE("run succeeds even when cancel_token is already cancelled") {
        // CtxInspectTool has no long-running ops; cancellation does not abort it.
        CtxInspectTool t;
        auto [src, tok] = CancelToken::make_root();
        src.request_stop();
        auto ctx        = make_ctx();
        ctx.cancel_token = std::move(tok);
        ToolResult r = t.run(make_args(), ctx);
        // No side effects — still returns a valid snapshot.
        CHECK_FALSE(r.is_error);
        REQUIRE(r.structured_payload.has_value());
    }
}

// =============================================================================
// TEST SUITE: run() — no side effects
// =============================================================================
TEST_SUITE("CtxInspectTool::run — no side effects") {

    TEST_CASE("calling run() multiple times on the same tool returns consistent results") {
        CtxInspectTool t;
        auto ctx  = make_ctx();
        auto args = make_args(5, 1000, 100000, 2);

        ToolResult r1 = t.run(args, ctx);
        ToolResult r2 = t.run(args, ctx);

        REQUIRE(r1.structured_payload.has_value());
        REQUIRE(r2.structured_payload.has_value());
        // Pure function: same inputs → identical outputs.
        CHECK(*r1.structured_payload == *r2.structured_payload);
        CHECK(r1.body == r2.body);
    }

    TEST_CASE("ctx fields are read-only — ctx is unchanged after run()") {
        CtxInspectTool t;
        auto ctx       = make_ctx();
        ctx.session_id = "unchanged-session";
        ctx.agent_id   = "unchanged-agent";
        auto args      = make_args();

        std::ignore = t.run(args, ctx);

        // ctx must not have been mutated.
        CHECK(ctx.session_id == "unchanged-session");
        CHECK(ctx.agent_id   == "unchanged-agent");
        CHECK(ctx.mode       == PermissionMode::Default);
    }
}

// =============================================================================
// TEST SUITE: ITool virtual dispatch via unique_ptr<ITool>
// =============================================================================
TEST_SUITE("CtxInspectTool — ITool virtual dispatch") {

    TEST_CASE("dispatch through unique_ptr<ITool> works correctly") {
        std::unique_ptr<ITool> tool = std::make_unique<CtxInspectTool>();
        CHECK(tool->name()                == std::string_view("CtxInspect"));
        CHECK(tool->is_read_only()        == true);
        CHECK(tool->requires_confirmation() == false);

        auto ctx  = make_ctx();
        auto args = make_args(3, 500, 100000, 1);
        ToolResult r = tool->run(args, ctx);
        CHECK_FALSE(r.is_error);
        REQUIRE(r.structured_payload.has_value());
        CHECK((*r.structured_payload)["message_count"].get<int>() == 3);
    }
}
