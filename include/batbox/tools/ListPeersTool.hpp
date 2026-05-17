// include/batbox/tools/ListPeersTool.hpp
//
// batbox::tools::ListPeersTool — ITool implementation that returns the current
// members of a named team (or the calling agent's own team when no team_name
// is supplied).
//
// Blueprint contract (task CPP 5.29, blueprints rows 16743–16745):
//   class ListPeersTool : public ITool
//   name()         = "ListPeers"
//   is_read_only() = true
//   Returns list of { agent_id, status } for each peer in the team.
//
// Tool arguments (JSON object):
//   team_name (string, optional) — team to query; defaults to the calling
//                                  agent's own team (ctx.agent_id looked up
//                                  via TeamRegistry).  If neither is found,
//                                  returns an empty list.
//
// Returns JSON object on success:
//   {
//     "team":    "<resolved team name or empty>",
//     "members": ["<agent_id>", …]
//   }
//
// Permission flags:
//   is_read_only()          = true   (read-only snapshot; no side effects)
//   requires_confirmation() = false  (informational; no prompt needed)

#pragma once

#include <batbox/tools/ITool.hpp>
#include <batbox/core/Json.hpp>

namespace batbox::agents {
class TeamRegistry;
} // namespace batbox::agents

namespace batbox::tools {

// =============================================================================
// ListPeersTool
// =============================================================================

/// Implements the "ListPeers" tool: returns a point-in-time snapshot of the
/// member list for a named team (or the calling agent's team).
///
/// Blueprint contract: batbox::tools::ListPeersTool (blueprints rows 16743–16745)
class ListPeersTool final : public ITool {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    /// Construct with a reference to the process-wide TeamRegistry.
    explicit ListPeersTool(batbox::agents::TeamRegistry& registry);

    // -------------------------------------------------------------------------
    // ITool identity
    // -------------------------------------------------------------------------

    /// Returns "ListPeers".
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

    /// Return a member snapshot for the specified team.
    ///
    /// args["team_name"] — optional string; when absent the tool uses
    ///                     ctx.agent_id to locate the calling agent's team.
    ///
    /// @returns ToolResult::ok(json)   always — empty member list if the team
    ///                                 is not found.
    ///          ToolResult::error(msg) on cancellation only.
    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override;

    // -------------------------------------------------------------------------
    // Permission gate hooks
    // -------------------------------------------------------------------------

    /// true — read-only snapshot; no mutations.
    [[nodiscard]] bool is_read_only() const override { return true; }

    /// false — informational; no user confirmation needed.
    [[nodiscard]] bool requires_confirmation() const override { return false; }

private:
    batbox::agents::TeamRegistry& registry_;
};

} // namespace batbox::tools
