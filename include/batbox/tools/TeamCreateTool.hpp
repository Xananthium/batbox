// include/batbox/tools/TeamCreateTool.hpp
//
// batbox::tools::TeamCreateTool — ITool implementation that creates a named
// team in the TeamRegistry and adds zero or more member agent_ids.
//
// Blueprint contract (task CPP 5.29, blueprints rows 16737–16739):
//   class TeamCreateTool : public ITool
//   name()    = "TeamCreate"
//   Delegates to AgentSupervisor::… (via TeamRegistry) to create + populate team.
//
// Tool arguments (JSON object):
//   name    (string,  required) — unique team name; non-empty
//   members (array,   optional) — array of agent_id strings to add as initial members
//
// Returns JSON object on success:
//   {
//     "team":    "<name>",
//     "members": ["<id1>", "<id2>", …],
//     "status":  "created"
//   }
//
// Permission flags:
//   is_read_only()          = false  (modifies the team registry)
//   requires_confirmation() = false  (orchestration primitive)

#pragma once

#include <batbox/tools/ITool.hpp>
#include <batbox/core/Json.hpp>

namespace batbox::agents {
class TeamRegistry;
} // namespace batbox::agents

namespace batbox::tools {

// =============================================================================
// TeamCreateTool
// =============================================================================

/// Implements the "TeamCreate" tool: creates a named team in the TeamRegistry
/// and populates it with an optional initial list of member agent_ids.
///
/// Blueprint contract: batbox::tools::TeamCreateTool (blueprints rows 16737–16739)
class TeamCreateTool final : public ITool {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    /// Construct with a reference to the process-wide TeamRegistry.
    explicit TeamCreateTool(batbox::agents::TeamRegistry& registry);

    // -------------------------------------------------------------------------
    // ITool identity
    // -------------------------------------------------------------------------

    /// Returns "TeamCreate".
    [[nodiscard]] std::string_view name() const override;

    /// Returns a one-sentence description for the OpenAI tool schema.
    [[nodiscard]] std::string_view description() const override;

    // -------------------------------------------------------------------------
    // OpenAI tool schema
    // -------------------------------------------------------------------------

    /// Returns the full tools[*].function JSON object.
    [[nodiscard]] Json schema_json() const override;

    // -------------------------------------------------------------------------
    // Execution
    // -------------------------------------------------------------------------

    /// Create a named team (idempotent) and add any provided member ids.
    ///
    /// args["name"]    — required non-empty string
    /// args["members"] — optional JSON array of agent_id strings
    ///
    /// @returns ToolResult::ok(json)   on success; body is formatted JSON.
    ///          ToolResult::error(msg) on validation failure.
    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override;

    // -------------------------------------------------------------------------
    // Permission gate hooks
    // -------------------------------------------------------------------------

    /// false — modifies the team registry.
    [[nodiscard]] bool is_read_only() const override { return false; }

    /// false — orchestration primitive; no user confirmation required.
    [[nodiscard]] bool requires_confirmation() const override { return false; }

private:
    batbox::agents::TeamRegistry& registry_;
};

} // namespace batbox::tools
