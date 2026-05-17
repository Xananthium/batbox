// src/tools/ListPeersTool.cpp
//
// Implementation of batbox::tools::ListPeersTool.
//
// Blueprint contract (task CPP 5.29, blueprints rows 16743–16745):
//   ListPeersTool::run — resolves the team from args["team_name"] or by
//   scanning the TeamRegistry for a team that contains ctx.agent_id, then
//   returns a snapshot of all member agent_ids.

#include <batbox/tools/ListPeersTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>
#include <batbox/agents/Team.hpp>
#include <batbox/core/Json.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace batbox::tools {

// =============================================================================
// Construction
// =============================================================================

ListPeersTool::ListPeersTool(batbox::agents::TeamRegistry& registry)
    : registry_(registry)
{}

// =============================================================================
// ITool identity
// =============================================================================

std::string_view ListPeersTool::name() const {
    return "ListPeers";
}

std::string_view ListPeersTool::description() const {
    return "Return a snapshot of the member agent_ids in a named team; when "
           "no team_name is supplied the calling agent's own team is used.";
}

// =============================================================================
// OpenAI tool schema
// =============================================================================

Json ListPeersTool::schema_json() const {
    return Json{
        {"name",        "ListPeers"},
        {"description", description()},
        {"parameters",  Json{
            {"type",       "object"},
            {"properties", Json{
                {"team_name", Json{
                    {"type",        "string"},
                    {"description", "Name of the team to inspect; when omitted "
                                    "the tool resolves the calling agent's own team "
                                    "from ctx.agent_id."}
                }}
            }},
            {"required", Json::array()}
        }}
    };
}

// =============================================================================
// Execution
// =============================================================================

ToolResult ListPeersTool::run(const Json& args, ToolContext& ctx) {
    // --- Cancellation check ---
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    std::string resolved_team_name;
    const batbox::agents::Team* team = nullptr;

    // --- Resolve team ---
    if (args.contains("team_name") && args["team_name"].is_string()) {
        // Explicit team name provided.
        resolved_team_name = args["team_name"].get<std::string>();
        if (!resolved_team_name.empty()) {
            team = registry_.get_team(resolved_team_name);
        }
    } else if (!ctx.agent_id.empty()) {
        // No explicit name — scan registry for a team that contains this agent.
        const std::vector<std::string> names = registry_.team_names();
        for (const auto& tname : names) {
            const batbox::agents::Team* candidate = registry_.get_team(tname);
            if (candidate == nullptr) {
                continue;
            }
            const std::vector<std::string> members = candidate->members();
            const bool contains = std::any_of(
                members.begin(), members.end(),
                [&](const std::string& mid) { return mid == ctx.agent_id; });
            if (contains) {
                team                = candidate;
                resolved_team_name  = tname;
                break;
            }
        }
    }

    // --- Build member list snapshot ---
    Json members_json = Json::array();
    if (team != nullptr) {
        for (const auto& mid : team->members()) {
            members_json.push_back(mid);
        }
    }

    Json payload = {
        {"team",    resolved_team_name},
        {"members", std::move(members_json)}
    };

    return ToolResult::ok(payload.dump(2), std::move(payload));
}

} // namespace batbox::tools
