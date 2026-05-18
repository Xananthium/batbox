// tests/integration/test_plan_mode.cpp
//
// Integration tests for:
//   batbox::tools::EnterPlanModeTool
//   batbox::tools::ExitPlanModeTool
//   batbox::tools::VerifyPlanExecutionTool
//
// These tests drive the tools end-to-end using a real PlanMode state machine,
// verifying state transitions, tool gating semantics, and advisory output.
//
// Build (standalone, no CMake):
//   c++ -std=c++20 \
//       -I<repo-root>/include \
//       -I$(DOCTEST_INCLUDE) \
//       tests/integration/test_plan_mode.cpp \
//       src/conversation/PlanMode.cpp \
//       src/tools/EnterPlanModeTool.cpp \
//       src/tools/ExitPlanModeTool.cpp \
//       src/tools/VerifyPlanExecutionTool.cpp \
//       -o /tmp/test_plan_mode && /tmp/test_plan_mode
//
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/tools/EnterPlanModeTool.hpp>
#include <batbox/tools/ExitPlanModeTool.hpp>
#include <batbox/tools/VerifyPlanExecutionTool.hpp>
#include <batbox/conversation/PlanMode.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/core/CancelToken.hpp>

using batbox::tools::EnterPlanModeTool;
using batbox::tools::ExitPlanModeTool;
using batbox::tools::VerifyPlanExecutionTool;
using batbox::conversation::PlanMode;
using batbox::conversation::PlanState;
using batbox::tools::ToolContext;
using batbox::tools::ToolResult;
using batbox::CancelSource;
using batbox::Json;
using namespace batbox::conversation;

// ---------------------------------------------------------------------------
// Helper: build a minimal ToolContext (no cancellation, default permissions)
// ---------------------------------------------------------------------------

static ToolContext make_ctx() {
    ToolContext ctx;
    ctx.cwd        = "/tmp";
    ctx.session_id = "test-session";
    ctx.agent_id   = "";
    return ctx;
}

// ---------------------------------------------------------------------------
// Helper: build a pre-cancelled ToolContext
// ---------------------------------------------------------------------------

static ToolContext make_cancelled_ctx() {
    CancelSource src;
    src.request_stop();
    ToolContext ctx = make_ctx();
    ctx.cancel_token = src.token();
    return ctx;
}

// ---------------------------------------------------------------------------
// Helper: ExitPlanModeTool pre-wired to auto-approve
// ---------------------------------------------------------------------------

static ExitPlanModeTool make_approving_exit(PlanMode& pm) {
    return ExitPlanModeTool(pm, [](const std::string&) { return true; });
}

// ---------------------------------------------------------------------------
// Helper: ExitPlanModeTool pre-wired to auto-reject
// ---------------------------------------------------------------------------

static ExitPlanModeTool make_rejecting_exit(PlanMode& pm) {
    return ExitPlanModeTool(pm, [](const std::string&) { return false; });
}

// ===========================================================================
// 1. EnterPlanModeTool — identity contract
// ===========================================================================

TEST_CASE("EnterPlanModeTool identity contract") {
    PlanMode pm;
    EnterPlanModeTool tool(pm);

    CHECK(tool.name()        == "EnterPlanMode");
    CHECK_FALSE(tool.name().empty());
    CHECK_FALSE(tool.description().empty());
    CHECK(tool.is_read_only()           == true);
    CHECK(tool.requires_confirmation()  == false);
}

TEST_CASE("EnterPlanModeTool schema_json has correct name") {
    PlanMode pm;
    EnterPlanModeTool tool(pm);
    const Json schema = tool.schema_json();
    CHECK(schema.contains("name"));
    CHECK(schema["name"].get<std::string>() == "EnterPlanMode");
    CHECK(schema.contains("parameters"));
}

// ===========================================================================
// 2. EnterPlanModeTool — state transitions
// ===========================================================================

TEST_CASE("EnterPlanMode transitions Inactive to Planning") {
    PlanMode pm;
    EnterPlanModeTool tool(pm);
    ToolContext ctx = make_ctx();

    CHECK(pm.state() == PlanState::Inactive);

    const ToolResult result = tool.run(Json::object(), ctx);

    CHECK_FALSE(result.is_error);
    CHECK_FALSE(result.body.empty());
    CHECK(pm.state() == PlanState::Planning);
    CHECK(pm.is_write_denied());
}

TEST_CASE("EnterPlanMode is idempotent when already Planning") {
    PlanMode pm;
    pm.enter_plan();
    EnterPlanModeTool tool(pm);
    ToolContext ctx = make_ctx();

    const ToolResult result = tool.run(Json::object(), ctx);

    CHECK_FALSE(result.is_error);
    CHECK(result.body.find("already active") != std::string::npos);
    CHECK(pm.state() == PlanState::Planning);
}

TEST_CASE("EnterPlanMode returns error when state is Approved") {
    PlanMode pm;
    pm.enter_plan();
    (void)pm.approve("existing plan");
    EnterPlanModeTool tool(pm);
    ToolContext ctx = make_ctx();

    const ToolResult result = tool.run(Json::object(), ctx);

    CHECK(result.is_error);
    CHECK(result.body.find("approved plan is already in progress") != std::string::npos);
    CHECK(pm.state() == PlanState::Approved); // unchanged
}

TEST_CASE("EnterPlanMode returns error on cancelled context") {
    PlanMode pm;
    EnterPlanModeTool tool(pm);
    ToolContext ctx = make_cancelled_ctx();

    const ToolResult result = tool.run(Json::object(), ctx);

    CHECK(result.is_error);
    CHECK(result.body.find("cancelled") != std::string::npos);
    CHECK(pm.state() == PlanState::Inactive); // unchanged
}

// ===========================================================================
// 3. ExitPlanModeTool — identity contract
// ===========================================================================

TEST_CASE("ExitPlanModeTool identity contract") {
    PlanMode pm;
    ExitPlanModeTool tool(pm, [](const std::string&) { return true; });

    CHECK(tool.name()        == "ExitPlanMode");
    CHECK_FALSE(tool.name().empty());
    CHECK_FALSE(tool.description().empty());
    CHECK(tool.is_read_only()           == false);
    CHECK(tool.requires_confirmation()  == false);
}

TEST_CASE("ExitPlanModeTool schema_json has correct name and plan parameter") {
    PlanMode pm;
    ExitPlanModeTool tool(pm, [](const std::string&) { return true; });
    const Json schema = tool.schema_json();
    CHECK(schema["name"].get<std::string>() == "ExitPlanMode");
    CHECK(schema.contains("parameters"));
    CHECK(schema["parameters"]["properties"].contains("plan"));
}

// ===========================================================================
// 4. ExitPlanModeTool — approval flow
// ===========================================================================

TEST_CASE("ExitPlanMode approves: transitions Planning to Approved") {
    PlanMode pm;
    pm.enter_plan();
    auto tool = make_approving_exit(pm);
    ToolContext ctx = make_ctx();

    Json args = {{"plan", "Step 1: do X\nStep 2: do Y"}};
    const ToolResult result = tool.run(args, ctx);

    CHECK_FALSE(result.is_error);
    CHECK(result.body.find("approved") != std::string::npos);
    CHECK(result.body.find("plan_id=1") != std::string::npos);
    CHECK(pm.state() == PlanState::Approved);
    CHECK(pm.plan_id() == 1u);
    CHECK(pm.plan_text() == "Step 1: do X\nStep 2: do Y");
}

TEST_CASE("ExitPlanMode approval result contains structured payload with plan_id") {
    PlanMode pm;
    pm.enter_plan();
    auto tool = make_approving_exit(pm);
    ToolContext ctx = make_ctx();

    Json args = {{"plan", "My plan"}};
    const ToolResult result = tool.run(args, ctx);

    REQUIRE(result.structured_payload.has_value());
    const Json& payload = *result.structured_payload;
    CHECK(payload.contains("plan_id"));
    CHECK(payload["plan_id"].get<std::uint32_t>() == 1u);
    CHECK(payload.contains("plan_text"));
    CHECK(payload["plan_text"].get<std::string>() == "My plan");
}

TEST_CASE("ExitPlanMode rejection: transitions Planning to Inactive") {
    PlanMode pm;
    pm.enter_plan();
    auto tool = make_rejecting_exit(pm);
    ToolContext ctx = make_ctx();

    Json args = {{"plan", "Some plan to reject"}};
    const ToolResult result = tool.run(args, ctx);

    CHECK_FALSE(result.is_error);
    CHECK(result.body.find("rejected") != std::string::npos);
    CHECK(result.body.find("Revise") != std::string::npos);
    CHECK(pm.state() == PlanState::Inactive);
}

TEST_CASE("ExitPlanMode returns error when state is Inactive") {
    PlanMode pm;
    auto tool = make_approving_exit(pm);
    ToolContext ctx = make_ctx();

    Json args = {{"plan", "Some plan"}};
    const ToolResult result = tool.run(args, ctx);

    CHECK(result.is_error);
    CHECK(result.body.find("Inactive") != std::string::npos);
    CHECK(result.body.find("EnterPlanMode") != std::string::npos);
}

TEST_CASE("ExitPlanMode returns error when state is Approved") {
    PlanMode pm;
    pm.enter_plan();
    (void)pm.approve("existing plan");

    auto tool = make_approving_exit(pm);
    ToolContext ctx = make_ctx();

    Json args = {{"plan", "New plan"}};
    const ToolResult result = tool.run(args, ctx);

    CHECK(result.is_error);
    CHECK(result.body.find("Approved") != std::string::npos);
    CHECK(pm.state() == PlanState::Approved); // unchanged
}

TEST_CASE("ExitPlanMode returns error on missing plan argument") {
    PlanMode pm;
    pm.enter_plan();
    auto tool = make_approving_exit(pm);
    ToolContext ctx = make_ctx();

    const ToolResult result = tool.run(Json::object(), ctx);

    CHECK(result.is_error);
    CHECK(result.body.find("plan") != std::string::npos);
    CHECK(pm.state() == PlanState::Planning); // unchanged
}

TEST_CASE("ExitPlanMode returns error on empty plan string") {
    PlanMode pm;
    pm.enter_plan();
    auto tool = make_approving_exit(pm);
    ToolContext ctx = make_ctx();

    Json args = {{"plan", ""}};
    const ToolResult result = tool.run(args, ctx);

    CHECK(result.is_error);
    CHECK(pm.state() == PlanState::Planning); // unchanged
}

TEST_CASE("ExitPlanMode returns error on cancelled context") {
    PlanMode pm;
    pm.enter_plan();
    auto tool = make_approving_exit(pm);
    ToolContext ctx = make_cancelled_ctx();

    Json args = {{"plan", "Some plan"}};
    const ToolResult result = tool.run(args, ctx);

    CHECK(result.is_error);
    CHECK(result.body.find("cancelled") != std::string::npos);
    CHECK(pm.state() == PlanState::Planning); // unchanged
}

TEST_CASE("ExitPlanMode plan_id increments across approval cycles") {
    PlanMode pm;
    auto approving_exit = make_approving_exit(pm);
    ToolContext ctx = make_ctx();

    // Cycle 1
    pm.enter_plan();
    Json args1 = {{"plan", "Plan 1"}};
    auto r1 = approving_exit.run(args1, ctx);
    CHECK_FALSE(r1.is_error);
    CHECK(r1.body.find("plan_id=1") != std::string::npos);
    pm.advance_turn(); // Approved → Inactive

    // Cycle 2
    EnterPlanModeTool enter_tool(pm);
    (void)enter_tool.run(Json::object(), ctx);
    Json args2 = {{"plan", "Plan 2"}};
    auto r2 = approving_exit.run(args2, ctx);
    CHECK_FALSE(r2.is_error);
    CHECK(r2.body.find("plan_id=2") != std::string::npos);
}

// ===========================================================================
// 5. VerifyPlanExecutionTool — identity contract
// ===========================================================================

TEST_CASE("VerifyPlanExecutionTool identity contract") {
    PlanMode pm;
    VerifyPlanExecutionTool tool(pm);

    CHECK(tool.name()        == "VerifyPlanExecution");
    CHECK_FALSE(tool.name().empty());
    CHECK_FALSE(tool.description().empty());
    CHECK(tool.is_read_only()           == true);
    CHECK(tool.requires_confirmation()  == false);
}

TEST_CASE("VerifyPlanExecutionTool schema_json has correct name") {
    PlanMode pm;
    VerifyPlanExecutionTool tool(pm);
    const Json schema = tool.schema_json();
    CHECK(schema["name"].get<std::string>() == "VerifyPlanExecution");
    CHECK(schema.contains("parameters"));
    CHECK(schema["parameters"]["properties"].contains("steps_completed"));
}

// ===========================================================================
// 6. VerifyPlanExecutionTool — advisory output per state
// ===========================================================================

TEST_CASE("VerifyPlanExecution in Inactive state returns advisory notice") {
    PlanMode pm;
    VerifyPlanExecutionTool tool(pm);
    ToolContext ctx = make_ctx();

    const ToolResult result = tool.run(Json::object(), ctx);

    CHECK_FALSE(result.is_error);
    CHECK(result.body.find("no active plan") != std::string::npos);
    REQUIRE(result.structured_payload.has_value());
    CHECK((*result.structured_payload)["plan_state"].get<std::string>() == "Inactive");
}

TEST_CASE("VerifyPlanExecution in Planning state returns advisory notice") {
    PlanMode pm;
    pm.enter_plan();
    VerifyPlanExecutionTool tool(pm);
    ToolContext ctx = make_ctx();

    const ToolResult result = tool.run(Json::object(), ctx);

    CHECK_FALSE(result.is_error);
    CHECK(result.body.find("not yet approved") != std::string::npos);
    REQUIRE(result.structured_payload.has_value());
    CHECK((*result.structured_payload)["plan_state"].get<std::string>() == "Planning");
}

TEST_CASE("VerifyPlanExecution in Approved state returns checklist with steps") {
    PlanMode pm;
    pm.enter_plan();
    (void)pm.approve("Step A\nStep B\nStep C");
    VerifyPlanExecutionTool tool(pm);
    ToolContext ctx = make_ctx();

    Json args = {{"steps_completed", Json::array({"Step A", "Step B"})}};
    const ToolResult result = tool.run(args, ctx);

    CHECK_FALSE(result.is_error);
    CHECK(result.body.find("plan_id=1") != std::string::npos);
    CHECK(result.body.find("[x] Step A") != std::string::npos);
    CHECK(result.body.find("[x] Step B") != std::string::npos);
    CHECK(result.body.find("2 step(s)") != std::string::npos);
}

TEST_CASE("VerifyPlanExecution in Approved state with no steps returns advisory") {
    PlanMode pm;
    pm.enter_plan();
    (void)pm.approve("Step A\nStep B");
    VerifyPlanExecutionTool tool(pm);
    ToolContext ctx = make_ctx();

    const ToolResult result = tool.run(Json::object(), ctx);

    CHECK_FALSE(result.is_error);
    CHECK(result.body.find("plan_id=1") != std::string::npos);
    CHECK(result.body.find("no steps provided") != std::string::npos);
}

TEST_CASE("VerifyPlanExecution structured payload always present") {
    PlanMode pm;
    pm.enter_plan();
    (void)pm.approve("the plan");
    VerifyPlanExecutionTool tool(pm);
    ToolContext ctx = make_ctx();

    Json args = {{"steps_completed", Json::array({"step one"})}};
    const ToolResult result = tool.run(args, ctx);

    REQUIRE(result.structured_payload.has_value());
    const Json& payload = *result.structured_payload;
    CHECK(payload.contains("plan_id"));
    CHECK(payload.contains("plan_state"));
    CHECK(payload.contains("steps_completed"));
    CHECK(payload.contains("advisory"));
    CHECK(payload.contains("plan_text"));
    CHECK(payload["plan_state"].get<std::string>() == "Approved");
    CHECK(payload["plan_id"].get<std::uint32_t>() == 1u);
}

TEST_CASE("VerifyPlanExecution structured payload plan_id is null when Inactive") {
    PlanMode pm;
    VerifyPlanExecutionTool tool(pm);
    ToolContext ctx = make_ctx();

    const ToolResult result = tool.run(Json::object(), ctx);

    REQUIRE(result.structured_payload.has_value());
    const Json& payload = *result.structured_payload;
    CHECK(payload.contains("plan_id"));
    CHECK(payload["plan_id"].is_null());
}

TEST_CASE("VerifyPlanExecution returns error on cancelled context") {
    PlanMode pm;
    pm.enter_plan();
    (void)pm.approve("plan");
    VerifyPlanExecutionTool tool(pm);
    ToolContext ctx = make_cancelled_ctx();

    const ToolResult result = tool.run(Json::object(), ctx);

    CHECK(result.is_error);
    CHECK(result.body.find("cancelled") != std::string::npos);
}

// ===========================================================================
// 7. Full round-trip: Enter → Exit (approve) → Verify → advance_turn
// ===========================================================================

TEST_CASE("full plan mode round-trip: enter, approve, verify, advance") {
    PlanMode pm;
    EnterPlanModeTool  enter_tool(pm);
    ExitPlanModeTool   exit_tool(pm, [](const std::string&) { return true; });
    VerifyPlanExecutionTool verify_tool(pm);
    ToolContext ctx = make_ctx();

    // Step 1: Enter plan mode
    {
        auto r = enter_tool.run(Json::object(), ctx);
        CHECK_FALSE(r.is_error);
        CHECK(pm.state() == PlanState::Planning);
    }

    // Step 2: Approve the plan
    {
        Json args = {{"plan", "Step 1: write tests\nStep 2: run tests"}};
        auto r = exit_tool.run(args, ctx);
        CHECK_FALSE(r.is_error);
        CHECK(pm.state() == PlanState::Approved);
    }

    // Step 3: Verify execution (simulate completing both steps)
    {
        Json args = {{"steps_completed",
                      Json::array({"Step 1: write tests", "Step 2: run tests"})}};
        auto r = verify_tool.run(args, ctx);
        CHECK_FALSE(r.is_error);
        CHECK(r.body.find("[x]") != std::string::npos);
        CHECK(r.body.find("2 step(s)") != std::string::npos);
        CHECK(pm.state() == PlanState::Approved); // verify does not mutate state
    }

    // Step 4: Advance turn (Approved → Inactive)
    pm.advance_turn();
    CHECK(pm.state() == PlanState::Inactive);
}

// ===========================================================================
// 8. Tool gating: write tools denied after EnterPlanMode
// ===========================================================================

TEST_CASE("write-side tools blocked by PlanMode after EnterPlanMode") {
    PlanMode pm;
    EnterPlanModeTool tool(pm);
    ToolContext ctx = make_ctx();

    (void)tool.run(Json::object(), ctx);

    CHECK(pm.is_write_denied());
    CHECK_FALSE(pm.is_tool_allowed("Write"));
    CHECK_FALSE(pm.is_tool_allowed("Edit"));
    CHECK_FALSE(pm.is_tool_allowed("Bash"));
    CHECK_FALSE(pm.is_tool_allowed("PowerShell"));
    CHECK_FALSE(pm.is_tool_allowed("TodoWrite"));
}

TEST_CASE("plan mode tools allowed while Planning") {
    PlanMode pm;
    EnterPlanModeTool tool(pm);
    ToolContext ctx = make_ctx();

    (void)tool.run(Json::object(), ctx);

    CHECK(pm.is_tool_allowed("EnterPlanMode"));
    CHECK(pm.is_tool_allowed("ExitPlanMode"));
    CHECK(pm.is_tool_allowed("VerifyPlanExecution"));
    CHECK(pm.is_tool_allowed("Read"));
    CHECK(pm.is_tool_allowed("Glob"));
    CHECK(pm.is_tool_allowed("Grep"));
}

TEST_CASE("write tools remain blocked in Approved state") {
    PlanMode pm;
    pm.enter_plan();
    (void)pm.approve("the plan");

    CHECK(pm.is_write_denied());
    CHECK_FALSE(pm.is_tool_allowed("Write"));
    CHECK_FALSE(pm.is_tool_allowed("Edit"));
    CHECK_FALSE(pm.is_tool_allowed("Bash"));
}

TEST_CASE("write tools restored after advance_turn from Approved") {
    PlanMode pm;
    EnterPlanModeTool enter_tool(pm);
    ExitPlanModeTool  exit_tool(pm, [](const std::string&) { return true; });
    ToolContext ctx = make_ctx();

    (void)enter_tool.run(Json::object(), ctx);
    (void)exit_tool.run(Json{{"plan", "plan text"}}, ctx);

    CHECK(pm.is_write_denied()); // still denied while Approved

    pm.advance_turn();

    CHECK_FALSE(pm.is_write_denied()); // restored to Inactive
    CHECK(pm.is_tool_allowed("Write"));
    CHECK(pm.is_tool_allowed("Edit"));
    CHECK(pm.is_tool_allowed("Bash"));
}

// ===========================================================================
// 9. EnterPlanModeTool body contains correct messaging
// ===========================================================================

TEST_CASE("EnterPlanMode body contains 'blocked' messaging") {
    PlanMode pm;
    EnterPlanModeTool tool(pm);
    ToolContext ctx = make_ctx();

    auto result = tool.run(Json::object(), ctx);
    CHECK_FALSE(result.is_error);
    // Body should inform the model that write tools are now blocked.
    CHECK(result.body.find("blocked") != std::string::npos);
}

// ===========================================================================
// 10. ExitPlanMode reject → re-enter → approve cycle
// ===========================================================================

TEST_CASE("plan can be revised and re-submitted after rejection") {
    PlanMode pm;
    EnterPlanModeTool enter_tool(pm);
    ToolContext ctx = make_ctx();

    // Enter planning
    (void)enter_tool.run(Json::object(), ctx);
    CHECK(pm.state() == PlanState::Planning);

    // First attempt: user rejects
    {
        ExitPlanModeTool exit_tool(pm, [](const std::string&) { return false; });
        Json args = {{"plan", "Incomplete plan"}};
        auto r = exit_tool.run(args, ctx);
        CHECK_FALSE(r.is_error);
        CHECK(pm.state() == PlanState::Inactive);
    }

    // Re-enter planning after rejection
    (void)enter_tool.run(Json::object(), ctx);
    CHECK(pm.state() == PlanState::Planning);

    // Second attempt: user approves revised plan
    {
        ExitPlanModeTool exit_tool(pm, [](const std::string&) { return true; });
        Json args = {{"plan", "Complete revised plan"}};
        auto r = exit_tool.run(args, ctx);
        CHECK_FALSE(r.is_error);
        CHECK(pm.state() == PlanState::Approved);
        CHECK(pm.plan_text() == "Complete revised plan");
    }
}
