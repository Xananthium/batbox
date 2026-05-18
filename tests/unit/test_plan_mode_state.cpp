// ---------------------------------------------------------------------------
// tests/unit/test_plan_mode_state.cpp
//
// Unit tests for batbox::conversation::PlanMode.
//
// Covers:
//   - All four valid transitions (enter_plan, approve, reject, advance_turn)
//   - One-shot Approved → Inactive via advance_turn
//   - Invalid transitions throw PlanModeError
//   - Tool gate: write-side tools denied in Planning and Approved states
//   - Tool gate: read-side tools allowed in all states
//   - Sample of all 39 curated tools — 10 representative entries
//   - Observer registration, notification, and deregistration via handle
//   - Plan text accumulator: stored on approve, cleared on next approve
//   - Plan id: increments each approve()
//   - Banner text: correct string per state
//
// Build (standalone, no CMake):
//   c++ -std=c++20 \
//       -I<repo-root>/include \
//       -I$(DOCTEST_INCLUDE) \
//       tests/unit/test_plan_mode_state.cpp \
//       src/conversation/PlanMode.cpp \
//       -o /tmp/test_plan_mode_state && /tmp/test_plan_mode_state
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "batbox/conversation/PlanMode.hpp"

using batbox::conversation::PlanMode;
using batbox::conversation::PlanModeError;
using batbox::conversation::PlanState;

// ===========================================================================
// 1. Initial state
// ===========================================================================

TEST_CASE("initial state is Inactive") {
    PlanMode pm;
    CHECK(pm.state() == PlanState::Inactive);
    CHECK_FALSE(pm.is_planning());
    CHECK_FALSE(pm.is_approved());
    CHECK(pm.plan_text().empty());
    CHECK(pm.plan_id() == 0u);
}

// ===========================================================================
// 2. Transition: Inactive → Planning via enter_plan()
// ===========================================================================

TEST_CASE("enter_plan transitions Inactive to Planning") {
    PlanMode pm;
    pm.enter_plan();
    CHECK(pm.state() == PlanState::Planning);
    CHECK(pm.is_planning());
}

TEST_CASE("enter_plan is idempotent when already Planning") {
    PlanMode pm;
    pm.enter_plan();
    CHECK_NOTHROW(pm.enter_plan()); // second call: noop
    CHECK(pm.state() == PlanState::Planning);
}

TEST_CASE("enter_plan throws when state is Approved") {
    PlanMode pm;
    pm.enter_plan();
    (void)pm.approve("my plan");
    CHECK_THROWS_AS(pm.enter_plan(), PlanModeError);
}

// ===========================================================================
// 3. Transition: Planning → Approved via approve()
// ===========================================================================

TEST_CASE("approve transitions Planning to Approved and stores plan text") {
    PlanMode pm;
    pm.enter_plan();
    auto id = pm.approve("step 1: do x; step 2: do y");
    CHECK(pm.state() == PlanState::Approved);
    CHECK(pm.is_approved());
    CHECK(pm.plan_text() == "step 1: do x; step 2: do y");
    CHECK(id == 1u);
    CHECK(pm.plan_id() == 1u);
}

TEST_CASE("approve throws when state is Inactive") {
    PlanMode pm;
    CHECK_THROWS_AS(pm.approve("plan"), PlanModeError);
}

TEST_CASE("approve throws when state is already Approved") {
    PlanMode pm;
    pm.enter_plan();
    (void)pm.approve("first plan");
    CHECK_THROWS_AS(pm.approve("second plan"), PlanModeError);
}

TEST_CASE("approve plan_id increments on successive approve cycles") {
    PlanMode pm;

    pm.enter_plan();
    auto id1 = pm.approve("plan 1");
    CHECK(id1 == 1u);

    pm.advance_turn();  // Approved → Inactive

    pm.enter_plan();
    auto id2 = pm.approve("plan 2");
    CHECK(id2 == 2u);
    CHECK(pm.plan_text() == "plan 2");
}

// ===========================================================================
// 4. Transition: Approved → Inactive via advance_turn()
// ===========================================================================

TEST_CASE("advance_turn transitions Approved to Inactive") {
    PlanMode pm;
    pm.enter_plan();
    (void)pm.approve("the plan");
    pm.advance_turn();
    CHECK(pm.state() == PlanState::Inactive);
    CHECK_FALSE(pm.is_approved());
}

TEST_CASE("advance_turn is a noop when state is Inactive") {
    PlanMode pm;
    pm.advance_turn(); // should not throw or change state
    CHECK(pm.state() == PlanState::Inactive);
}

TEST_CASE("advance_turn is a noop when state is Planning") {
    PlanMode pm;
    pm.enter_plan();
    pm.advance_turn(); // noop — not Approved
    CHECK(pm.state() == PlanState::Planning);
}

// ===========================================================================
// 5. Transition: Planning → Inactive via reject()
// ===========================================================================

TEST_CASE("reject transitions Planning to Inactive") {
    PlanMode pm;
    pm.enter_plan();
    pm.reject();
    CHECK(pm.state() == PlanState::Inactive);
}

TEST_CASE("reject throws when state is Inactive") {
    PlanMode pm;
    CHECK_THROWS_AS(pm.reject(), PlanModeError);
}

TEST_CASE("reject throws when state is Approved") {
    PlanMode pm;
    pm.enter_plan();
    (void)pm.approve("some plan");
    CHECK_THROWS_AS(pm.reject(), PlanModeError);
}

// ===========================================================================
// 6. Full round-trip: enter → approve → advance_turn → enter again
// ===========================================================================

TEST_CASE("full approve cycle can be repeated multiple times") {
    PlanMode pm;

    for (int cycle = 1; cycle <= 3; ++cycle) {
        CHECK(pm.state() == PlanState::Inactive);
        pm.enter_plan();
        CHECK(pm.state() == PlanState::Planning);
        auto id = pm.approve("cycle " + std::to_string(cycle));
        CHECK(pm.state() == PlanState::Approved);
        CHECK(static_cast<int>(id) == cycle);
        pm.advance_turn();
        CHECK(pm.state() == PlanState::Inactive);
    }
}

// ===========================================================================
// 7. Tool gate: write-side tools denied in Planning state
// ===========================================================================

TEST_CASE("write tools denied in Planning state") {
    PlanMode pm;
    pm.enter_plan();

    CHECK_FALSE(pm.is_tool_allowed("Write"));
    CHECK_FALSE(pm.is_tool_allowed("Edit"));
    CHECK_FALSE(pm.is_tool_allowed("Bash"));
    CHECK_FALSE(pm.is_tool_allowed("PowerShell"));
    CHECK_FALSE(pm.is_tool_allowed("TodoWrite"));
}

TEST_CASE("write tools denied in Approved state") {
    PlanMode pm;
    pm.enter_plan();
    (void)pm.approve("plan");

    CHECK_FALSE(pm.is_tool_allowed("Write"));
    CHECK_FALSE(pm.is_tool_allowed("Edit"));
    CHECK_FALSE(pm.is_tool_allowed("Bash"));
    CHECK_FALSE(pm.is_tool_allowed("PowerShell"));
    CHECK_FALSE(pm.is_tool_allowed("TodoWrite"));
}

TEST_CASE("write tools allowed in Inactive state") {
    PlanMode pm;

    CHECK(pm.is_tool_allowed("Write"));
    CHECK(pm.is_tool_allowed("Edit"));
    CHECK(pm.is_tool_allowed("Bash"));
    CHECK(pm.is_tool_allowed("PowerShell"));
    CHECK(pm.is_tool_allowed("TodoWrite"));
}

// ===========================================================================
// 8. Tool gate: sample of all 39 curated read-side tools (10 of 39)
//    All of these must be allowed in Planning state.
//    Full curated surface list from curated-surface.md.
// ===========================================================================

TEST_CASE("read-side curated tools allowed in Planning — 10 representative samples") {
    PlanMode pm;
    pm.enter_plan();

    // File / Search group (read-only subset)
    CHECK(pm.is_tool_allowed("Read"));
    CHECK(pm.is_tool_allowed("Glob"));
    CHECK(pm.is_tool_allowed("Grep"));
    CHECK(pm.is_tool_allowed("ToolSearch"));
    CHECK(pm.is_tool_allowed("WebFetch"));
    CHECK(pm.is_tool_allowed("WebSearch"));

    // Task / Plan / Diagnostics group
    CHECK(pm.is_tool_allowed("Task"));
    CHECK(pm.is_tool_allowed("TaskList"));
    CHECK(pm.is_tool_allowed("CtxInspect"));
    CHECK(pm.is_tool_allowed("EnterPlanMode"));
}

TEST_CASE("all remaining curated tools allowed in Planning state") {
    PlanMode pm;
    pm.enter_plan();

    // Task / Plan / Skill group
    CHECK(pm.is_tool_allowed("TaskOutput"));
    CHECK(pm.is_tool_allowed("TaskStop"));
    CHECK(pm.is_tool_allowed("SendMessage"));
    CHECK(pm.is_tool_allowed("TaskCreate"));
    CHECK(pm.is_tool_allowed("TaskGet"));
    CHECK(pm.is_tool_allowed("TaskUpdate"));
    CHECK(pm.is_tool_allowed("CronCreate"));
    CHECK(pm.is_tool_allowed("CronDelete"));
    CHECK(pm.is_tool_allowed("CronList"));
    CHECK(pm.is_tool_allowed("ExitPlanMode"));
    CHECK(pm.is_tool_allowed("VerifyPlanExecution"));
    CHECK(pm.is_tool_allowed("Skill"));
    CHECK(pm.is_tool_allowed("AskUserQuestion"));

    // MCP / Scheduling / Diagnostics group
    CHECK(pm.is_tool_allowed("ListMcpResources"));
    CHECK(pm.is_tool_allowed("ReadMcpResource"));
    CHECK(pm.is_tool_allowed("MCP"));
    CHECK(pm.is_tool_allowed("Sleep"));
    CHECK(pm.is_tool_allowed("RemoteTrigger"));
    CHECK(pm.is_tool_allowed("Config"));

    // Team / Misc group
    CHECK(pm.is_tool_allowed("TeamCreate"));
    CHECK(pm.is_tool_allowed("TeamDelete"));
    CHECK(pm.is_tool_allowed("ListPeers"));
    CHECK(pm.is_tool_allowed("Workflow"));
    CHECK(pm.is_tool_allowed("Snip"));
}

TEST_CASE("unknown tool name treated as allowed in Planning state") {
    PlanMode pm;
    pm.enter_plan();
    // Any tool not in the write-side set is permitted.
    CHECK(pm.is_tool_allowed("SomeFutureReadTool"));
    CHECK(pm.is_tool_allowed(""));
}

// ===========================================================================
// 9. Write tools re-allowed after advance_turn (Approved → Inactive)
// ===========================================================================

TEST_CASE("write tools re-allowed after advance_turn completes cycle") {
    PlanMode pm;
    pm.enter_plan();
    (void)pm.approve("the plan");
    pm.advance_turn();

    CHECK(pm.is_tool_allowed("Write"));
    CHECK(pm.is_tool_allowed("Edit"));
    CHECK(pm.is_tool_allowed("Bash"));
}

// ===========================================================================
// 10. Observer: notified on every transition
// ===========================================================================

TEST_CASE("observer is notified on enter_plan") {
    PlanMode pm;
    PlanState observed = PlanState::Inactive;
    auto h = pm.add_observer([&](PlanState s) { observed = s; });

    pm.enter_plan();
    CHECK(observed == PlanState::Planning);
}

TEST_CASE("observer is notified on approve") {
    PlanMode pm;
    pm.enter_plan();
    PlanState observed = PlanState::Planning;
    auto h = pm.add_observer([&](PlanState s) { observed = s; });

    (void)pm.approve("plan text");
    CHECK(observed == PlanState::Approved);
}

TEST_CASE("observer is notified on reject") {
    PlanMode pm;
    pm.enter_plan();
    PlanState observed = PlanState::Planning;
    auto h = pm.add_observer([&](PlanState s) { observed = s; });

    pm.reject();
    CHECK(observed == PlanState::Inactive);
}

TEST_CASE("observer is notified on advance_turn") {
    PlanMode pm;
    pm.enter_plan();
    (void)pm.approve("plan");
    PlanState observed = PlanState::Approved;
    auto h = pm.add_observer([&](PlanState s) { observed = s; });

    pm.advance_turn();
    CHECK(observed == PlanState::Inactive);
}

TEST_CASE("multiple observers all receive notifications") {
    PlanMode pm;
    int count = 0;
    auto h1 = pm.add_observer([&](PlanState) { ++count; });
    auto h2 = pm.add_observer([&](PlanState) { ++count; });

    pm.enter_plan();
    CHECK(count == 2);
}

TEST_CASE("destroying handle deregisters observer") {
    PlanMode pm;
    int count = 0;
    {
        auto h = pm.add_observer([&](PlanState) { ++count; });
        pm.enter_plan();
        CHECK(count == 1);
        // h destroyed here
    }
    pm.reject();
    // Observer was deregistered; count should still be 1.
    CHECK(count == 1);
}

// ===========================================================================
// 11. Plan text accumulator
// ===========================================================================

TEST_CASE("plan_text is empty before first approve") {
    PlanMode pm;
    pm.enter_plan();
    CHECK(pm.plan_text().empty());
}

TEST_CASE("plan_text retains value after advance_turn") {
    PlanMode pm;
    pm.enter_plan();
    (void)pm.approve("retained text");
    pm.advance_turn();
    // History retained for reference until next approve.
    CHECK(pm.plan_text() == "retained text");
}

TEST_CASE("plan_text updated to new value on second approve") {
    PlanMode pm;

    pm.enter_plan();
    (void)pm.approve("first");
    pm.advance_turn();

    pm.enter_plan();
    (void)pm.approve("second");
    CHECK(pm.plan_text() == "second");
}

// ===========================================================================
// 12. Banner text
// ===========================================================================

TEST_CASE("banner is empty when Inactive") {
    PlanMode pm;
    CHECK(pm.banner().empty());
}

TEST_CASE("banner is 'PLAN MODE' when Planning") {
    PlanMode pm;
    pm.enter_plan();
    CHECK(pm.banner() == "PLAN MODE");
}

TEST_CASE("banner is 'PLAN MODE — APPROVED' when Approved") {
    PlanMode pm;
    pm.enter_plan();
    (void)pm.approve("plan");
    CHECK(pm.banner() == "PLAN MODE \xe2\x80\x94 APPROVED"); // UTF-8 em dash
}

TEST_CASE("banner returns empty after advance_turn resets to Inactive") {
    PlanMode pm;
    pm.enter_plan();
    (void)pm.approve("plan");
    pm.advance_turn();
    CHECK(pm.banner().empty());
}

// ===========================================================================
// 13. is_write_denied() — blueprint contract method
// ===========================================================================

TEST_CASE("is_write_denied returns false in Inactive state") {
    PlanMode pm;
    CHECK_FALSE(pm.is_write_denied());
}

TEST_CASE("is_write_denied returns true in Planning state") {
    PlanMode pm;
    pm.enter_plan();
    CHECK(pm.is_write_denied());
}

TEST_CASE("is_write_denied returns true in Approved state") {
    PlanMode pm;
    pm.enter_plan();
    (void)pm.approve("plan");
    CHECK(pm.is_write_denied());
}

TEST_CASE("is_write_denied consistent with is_tool_allowed for Write tool") {
    PlanMode pm;
    pm.enter_plan();
    CHECK(pm.is_write_denied() == !pm.is_tool_allowed("Write"));
}

// ===========================================================================
// 14. transition() — low-level blueprint contract method
// ===========================================================================

TEST_CASE("transition() to Planning sets state") {
    PlanMode pm;
    pm.transition(PlanState::Planning);
    CHECK(pm.state() == PlanState::Planning);
}

TEST_CASE("transition() to Approved sets state") {
    PlanMode pm;
    pm.transition(PlanState::Approved);
    CHECK(pm.state() == PlanState::Approved);
}

TEST_CASE("transition() to Inactive sets state") {
    PlanMode pm;
    pm.transition(PlanState::Planning);
    pm.transition(PlanState::Inactive);
    CHECK(pm.state() == PlanState::Inactive);
}

TEST_CASE("transition() notifies observers") {
    PlanMode pm;
    PlanState observed = PlanState::Inactive;
    auto h = pm.add_observer([&](PlanState s) { observed = s; });

    pm.transition(PlanState::Planning);
    CHECK(observed == PlanState::Planning);

    pm.transition(PlanState::Approved);
    CHECK(observed == PlanState::Approved);
}

TEST_CASE("transition() enforces tool gate for Planning state") {
    PlanMode pm;
    pm.transition(PlanState::Planning);
    CHECK(pm.is_write_denied());
    CHECK_FALSE(pm.is_tool_allowed("Bash"));
    CHECK(pm.is_tool_allowed("Read"));
}
