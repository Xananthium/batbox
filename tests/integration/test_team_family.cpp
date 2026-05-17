// =============================================================================
// tests/integration/test_team_family.cpp — doctest suite for team-coordination
// tools (task CPP 5.29):
//   TeamCreateTool, TeamDeleteTool, ListPeersTool, WorkflowTool
//
// Build standalone (no CMake, from repo root):
//   c++ -std=c++20 -Iinclude \
//       tests/integration/test_team_family.cpp \
//       src/tools/TeamCreateTool.cpp \
//       src/tools/TeamDeleteTool.cpp \
//       src/tools/ListPeersTool.cpp \
//       src/tools/WorkflowTool.cpp \
//       src/agents/Team.cpp \
//       src/agents/Workflow.cpp \
//       src/agents/AgentSpec.cpp \
//       src/agents/AgentEvent.cpp \
//       src/agents/AgentSupervisor.cpp \
//       src/plugins/FrontmatterParser.cpp \
//       src/core/Json.cpp \
//       -o /tmp/test_team_family && /tmp/test_team_family
//
// Acceptance criteria covered:
//   [AC1]  TeamCreate persists team to in-memory registry
//   [AC2]  TeamCreate returns status="created" with member list
//   [AC3]  TeamCreate with members populates team correctly
//   [AC4]  TeamCreate missing 'name' returns error
//   [AC5]  TeamDelete removes existing team; status="deleted"
//   [AC6]  TeamDelete on non-existent team; status="not_found"
//   [AC7]  TeamDelete missing 'name' returns error
//   [AC8]  ListPeers returns members of a named team
//   [AC9]  ListPeers with no team_name resolves caller's team via agent_id
//   [AC10] ListPeers on unknown team returns empty member list
//   [AC11] WorkflowTool missing 'steps' returns error
//   [AC12] WorkflowTool with malformed step returns error
//   [AC13] WorkflowTool cycle detection surfaces error from Workflow::execute()
//   [AC14] WorkflowTool empty steps array returns status="completed"
//   [AC15] WorkflowTool unknown dependency returns error from Workflow::execute()
//   [AC16] Cancellation: all four tools return error("cancelled") when token fired
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/tools/TeamCreateTool.hpp>
#include <batbox/tools/TeamDeleteTool.hpp>
#include <batbox/tools/ListPeersTool.hpp>
#include <batbox/tools/WorkflowTool.hpp>

#include <batbox/agents/Team.hpp>
#include <batbox/agents/AgentSupervisor.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/permissions/PermissionMode.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>

#include <filesystem>
#include <string>
#include <vector>

using namespace batbox;
using namespace batbox::tools;
using namespace batbox::agents;
using namespace batbox::permissions;

// =============================================================================
// Test helpers
// =============================================================================

static ToolContext make_ctx(const std::string& agent_id = "") {
    ToolContext ctx;
    ctx.cwd        = std::filesystem::temp_directory_path();
    ctx.mode       = PermissionMode::Default;
    ctx.session_id = "test-session-5-29";
    ctx.agent_id   = agent_id;
    return ctx;
}

static ToolContext make_cancelled_ctx() {
    ToolContext ctx = make_ctx();
    auto [src, tok] = CancelToken::make_root();
    src.request_stop();
    ctx.cancel_token = std::move(tok);
    return ctx;
}

/// Wipe a team from the registry so tests don't interfere with each other.
static void cleanup_team(std::string_view name) {
    TeamRegistry::instance().delete_team(name);
}

// =============================================================================
// [AC1] TeamCreate — team is persisted to registry
// =============================================================================
TEST_CASE("TeamCreate [AC1]: team is persisted to registry") {
    cleanup_team("ac1-team");

    TeamCreateTool tool(TeamRegistry::instance());
    auto ctx = make_ctx();

    Json args = {{"name", "ac1-team"}};
    ToolResult result = tool.run(args, ctx);

    CHECK_FALSE(result.is_error);

    // Verify registry now holds the team.
    CHECK(TeamRegistry::instance().get_team("ac1-team") != nullptr);

    cleanup_team("ac1-team");
}

// =============================================================================
// [AC2] TeamCreate — returns status="created" with member list
// =============================================================================
TEST_CASE("TeamCreate [AC2]: returns status=created with member list") {
    cleanup_team("ac2-team");

    TeamCreateTool tool(TeamRegistry::instance());
    auto ctx = make_ctx();

    Json args = {{"name", "ac2-team"}};
    ToolResult result = tool.run(args, ctx);

    REQUIRE_FALSE(result.is_error);
    REQUIRE(result.structured_payload.has_value());

    const Json& payload = *result.structured_payload;
    CHECK(payload["team"].get<std::string>() == "ac2-team");
    CHECK(payload["status"].get<std::string>() == "created");
    CHECK(payload["members"].is_array());
    CHECK(payload["members"].empty());

    cleanup_team("ac2-team");
}

// =============================================================================
// [AC3] TeamCreate — populates team with provided members
// =============================================================================
TEST_CASE("TeamCreate [AC3]: populates team with provided members") {
    cleanup_team("ac3-team");

    TeamCreateTool tool(TeamRegistry::instance());
    auto ctx = make_ctx();

    Json args = {
        {"name",    "ac3-team"},
        {"members", Json::array({"agent-alpha", "agent-beta"})}
    };
    ToolResult result = tool.run(args, ctx);

    REQUIRE_FALSE(result.is_error);

    Team* team = TeamRegistry::instance().get_team("ac3-team");
    REQUIRE(team != nullptr);

    const std::vector<std::string> members = team->members();
    CHECK(members.size() == 2);

    bool has_alpha = false;
    bool has_beta  = false;
    for (const auto& m : members) {
        if (m == "agent-alpha") has_alpha = true;
        if (m == "agent-beta")  has_beta  = true;
    }
    CHECK(has_alpha);
    CHECK(has_beta);

    cleanup_team("ac3-team");
}

// =============================================================================
// [AC4] TeamCreate — missing 'name' returns error
// =============================================================================
TEST_CASE("TeamCreate [AC4]: missing name returns error") {
    TeamCreateTool tool(TeamRegistry::instance());
    auto ctx = make_ctx();

    Json args = Json::object();  // no 'name' key
    ToolResult result = tool.run(args, ctx);

    CHECK(result.is_error);
    CHECK_FALSE(result.body.empty());
}

// =============================================================================
// [AC5] TeamDelete — removes existing team; status="deleted"
// =============================================================================
TEST_CASE("TeamDelete [AC5]: removes existing team") {
    // Pre-create the team.
    TeamRegistry::instance().create_team("ac5-team");

    TeamDeleteTool tool(TeamRegistry::instance());
    auto ctx = make_ctx();

    Json args = {{"name", "ac5-team"}};
    ToolResult result = tool.run(args, ctx);

    REQUIRE_FALSE(result.is_error);
    REQUIRE(result.structured_payload.has_value());

    const Json& payload = *result.structured_payload;
    CHECK(payload["status"].get<std::string>() == "deleted");
    CHECK(payload["team"].get<std::string>() == "ac5-team");

    // Team must no longer be in the registry.
    CHECK(TeamRegistry::instance().get_team("ac5-team") == nullptr);
}

// =============================================================================
// [AC6] TeamDelete — non-existent team returns status="not_found"
// =============================================================================
TEST_CASE("TeamDelete [AC6]: non-existent team returns not_found") {
    cleanup_team("ac6-team-ghost");  // ensure it really doesn't exist

    TeamDeleteTool tool(TeamRegistry::instance());
    auto ctx = make_ctx();

    Json args = {{"name", "ac6-team-ghost"}};
    ToolResult result = tool.run(args, ctx);

    REQUIRE_FALSE(result.is_error);
    REQUIRE(result.structured_payload.has_value());

    const Json& payload = *result.structured_payload;
    CHECK(payload["status"].get<std::string>() == "not_found");
}

// =============================================================================
// [AC7] TeamDelete — missing 'name' returns error
// =============================================================================
TEST_CASE("TeamDelete [AC7]: missing name returns error") {
    TeamDeleteTool tool(TeamRegistry::instance());
    auto ctx = make_ctx();

    Json args = Json::object();
    ToolResult result = tool.run(args, ctx);

    CHECK(result.is_error);
}

// =============================================================================
// [AC8] ListPeers — returns members of a named team
// =============================================================================
TEST_CASE("ListPeers [AC8]: returns members of named team") {
    cleanup_team("ac8-team");
    Team* team = TeamRegistry::instance().create_team("ac8-team");
    team->add_member("peer-1");
    team->add_member("peer-2");

    ListPeersTool tool(TeamRegistry::instance());
    auto ctx = make_ctx();

    Json args = {{"team_name", "ac8-team"}};
    ToolResult result = tool.run(args, ctx);

    REQUIRE_FALSE(result.is_error);
    REQUIRE(result.structured_payload.has_value());

    const Json& payload = *result.structured_payload;
    CHECK(payload["team"].get<std::string>() == "ac8-team");
    CHECK(payload["members"].is_array());
    CHECK(payload["members"].size() == 2);

    cleanup_team("ac8-team");
}

// =============================================================================
// [AC9] ListPeers — resolves caller's team via ctx.agent_id when no team_name
// =============================================================================
TEST_CASE("ListPeers [AC9]: resolves caller team via agent_id") {
    const std::string my_agent_id = "caller-agent-ac9";

    cleanup_team("ac9-team");
    Team* team = TeamRegistry::instance().create_team("ac9-team");
    team->add_member(my_agent_id);
    team->add_member("other-agent-ac9");

    ListPeersTool tool(TeamRegistry::instance());
    auto ctx = make_ctx(my_agent_id);

    // No team_name in args — tool must resolve via ctx.agent_id.
    Json args = Json::object();
    ToolResult result = tool.run(args, ctx);

    REQUIRE_FALSE(result.is_error);
    REQUIRE(result.structured_payload.has_value());

    const Json& payload = *result.structured_payload;
    CHECK(payload["team"].get<std::string>() == "ac9-team");
    CHECK(payload["members"].size() == 2);

    cleanup_team("ac9-team");
}

// =============================================================================
// [AC10] ListPeers — unknown team returns empty member list
// =============================================================================
TEST_CASE("ListPeers [AC10]: unknown team returns empty members") {
    cleanup_team("ac10-ghost-team");

    ListPeersTool tool(TeamRegistry::instance());
    auto ctx = make_ctx();

    Json args = {{"team_name", "ac10-ghost-team"}};
    ToolResult result = tool.run(args, ctx);

    REQUIRE_FALSE(result.is_error);
    REQUIRE(result.structured_payload.has_value());

    const Json& payload = *result.structured_payload;
    CHECK(payload["members"].is_array());
    CHECK(payload["members"].empty());
}

// =============================================================================
// [AC11] WorkflowTool — missing 'steps' returns error
// =============================================================================
TEST_CASE("WorkflowTool [AC11]: missing steps argument returns error") {
    AgentSupervisor supervisor;
    WorkflowTool tool(supervisor);
    auto ctx = make_ctx();

    Json args = Json::object();
    ToolResult result = tool.run(args, ctx);

    CHECK(result.is_error);
}

// =============================================================================
// [AC12] WorkflowTool — malformed step (missing required field) returns error
// =============================================================================
TEST_CASE("WorkflowTool [AC12]: step missing required agent_name returns error") {
    AgentSupervisor supervisor;
    WorkflowTool tool(supervisor);
    auto ctx = make_ctx();

    Json step = {
        {"name",   "bad-step"},
        {"prompt", "Do something"}
        // agent_name is intentionally missing
    };

    Json args = {{"steps", Json::array({step})}};
    ToolResult result = tool.run(args, ctx);

    CHECK(result.is_error);
    CHECK(result.body.find("agent_name") != std::string::npos);
}

// =============================================================================
// [AC13] WorkflowTool — cycle detection surfaces error
// =============================================================================
TEST_CASE("WorkflowTool [AC13]: cycle in DAG causes execute() error") {
    AgentSupervisor supervisor;
    WorkflowTool tool(supervisor);
    auto ctx = make_ctx();

    // A <-> B cycle
    Json step_a = {
        {"name",       "step-a"},
        {"agent_name", "generic-agent"},
        {"prompt",     "Hello"},
        {"depends_on", Json::array({"step-b"})}
    };
    Json step_b = {
        {"name",       "step-b"},
        {"agent_name", "generic-agent"},
        {"prompt",     "Hello"},
        {"depends_on", Json::array({"step-a"})}
    };

    Json args = {{"steps", Json::array({step_a, step_b})}};
    ToolResult result = tool.run(args, ctx);

    CHECK(result.is_error);
    // Error body must mention cycle (from Workflow::execute).
    CHECK(result.body.find("cycle") != std::string::npos);
}

// =============================================================================
// [AC14] WorkflowTool — empty steps returns status="completed"
// =============================================================================
TEST_CASE("WorkflowTool [AC14]: empty steps array returns completed") {
    AgentSupervisor supervisor;
    WorkflowTool tool(supervisor);
    auto ctx = make_ctx();

    Json args = {{"steps", Json::array()}};
    ToolResult result = tool.run(args, ctx);

    REQUIRE_FALSE(result.is_error);
    REQUIRE(result.structured_payload.has_value());
    CHECK((*result.structured_payload)["status"].get<std::string>() == "completed");
}

// =============================================================================
// [AC15] WorkflowTool — unknown dependency surfaces error
// =============================================================================
TEST_CASE("WorkflowTool [AC15]: unknown dependency causes error") {
    AgentSupervisor supervisor;
    WorkflowTool tool(supervisor);
    auto ctx = make_ctx();

    Json step = {
        {"name",       "lonely-step"},
        {"agent_name", "generic-agent"},
        {"prompt",     "Hello"},
        {"depends_on", Json::array({"nonexistent-step"})}
    };

    Json args = {{"steps", Json::array({step})}};
    ToolResult result = tool.run(args, ctx);

    CHECK(result.is_error);
    CHECK(result.body.find("nonexistent-step") != std::string::npos);
}

// =============================================================================
// [AC16] Cancellation — all four tools return error("cancelled")
// =============================================================================
TEST_CASE("Cancellation [AC16]: TeamCreateTool returns cancelled") {
    TeamCreateTool tool(TeamRegistry::instance());
    auto ctx = make_cancelled_ctx();

    Json args = {{"name", "cancel-team"}};
    ToolResult result = tool.run(args, ctx);

    CHECK(result.is_error);
    CHECK(result.body.find("cancelled") != std::string::npos);
    cleanup_team("cancel-team");
}

TEST_CASE("Cancellation [AC16]: TeamDeleteTool returns cancelled") {
    TeamDeleteTool tool(TeamRegistry::instance());
    auto ctx = make_cancelled_ctx();

    Json args = {{"name", "cancel-team-del"}};
    ToolResult result = tool.run(args, ctx);

    CHECK(result.is_error);
    CHECK(result.body.find("cancelled") != std::string::npos);
}

TEST_CASE("Cancellation [AC16]: ListPeersTool returns cancelled") {
    ListPeersTool tool(TeamRegistry::instance());
    auto ctx = make_cancelled_ctx();

    Json args = {{"team_name", "cancel-team-list"}};
    ToolResult result = tool.run(args, ctx);

    CHECK(result.is_error);
    CHECK(result.body.find("cancelled") != std::string::npos);
}

TEST_CASE("Cancellation [AC16]: WorkflowTool returns cancelled") {
    AgentSupervisor supervisor;
    WorkflowTool tool(supervisor);
    auto ctx = make_cancelled_ctx();

    Json args = {{"steps", Json::array()}};
    ToolResult result = tool.run(args, ctx);

    CHECK(result.is_error);
    CHECK(result.body.find("cancelled") != std::string::npos);
}

// =============================================================================
// [Additional] schema_json / name / description / permission gate accessors
// =============================================================================
TEST_CASE("TeamCreateTool: ITool contract — name, description, schema, flags") {
    TeamCreateTool tool(TeamRegistry::instance());

    CHECK(tool.name() == "TeamCreate");
    CHECK_FALSE(tool.description().empty());
    CHECK_FALSE(tool.is_read_only());
    CHECK_FALSE(tool.requires_confirmation());

    const Json schema = tool.schema_json();
    CHECK(schema.contains("name"));
    CHECK(schema["name"].get<std::string>() == "TeamCreate");
    CHECK(schema.contains("parameters"));
}

TEST_CASE("TeamDeleteTool: ITool contract — name, description, schema, flags") {
    TeamDeleteTool tool(TeamRegistry::instance());

    CHECK(tool.name() == "TeamDelete");
    CHECK_FALSE(tool.description().empty());
    CHECK_FALSE(tool.is_read_only());
    CHECK_FALSE(tool.requires_confirmation());

    const Json schema = tool.schema_json();
    CHECK(schema["name"].get<std::string>() == "TeamDelete");
}

TEST_CASE("ListPeersTool: ITool contract — name, description, schema, flags") {
    ListPeersTool tool(TeamRegistry::instance());

    CHECK(tool.name() == "ListPeers");
    CHECK_FALSE(tool.description().empty());
    CHECK(tool.is_read_only());
    CHECK_FALSE(tool.requires_confirmation());

    const Json schema = tool.schema_json();
    CHECK(schema["name"].get<std::string>() == "ListPeers");
}

TEST_CASE("WorkflowTool: ITool contract — name, description, schema, flags") {
    AgentSupervisor supervisor;
    WorkflowTool tool(supervisor);

    CHECK(tool.name() == "Workflow");
    CHECK_FALSE(tool.description().empty());
    CHECK_FALSE(tool.is_read_only());
    CHECK_FALSE(tool.requires_confirmation());

    const Json schema = tool.schema_json();
    CHECK(schema["name"].get<std::string>() == "Workflow");
    CHECK(schema.contains("parameters"));
}
