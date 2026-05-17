// src/tools/TeamCreateTool.cpp
//
// Implementation of batbox::tools::TeamCreateTool.
//
// Blueprint contract (task CPP 5.29, blueprints rows 16737–16739):
//   TeamCreateTool::run — calls TeamRegistry::create_team(name) then adds any
//   provided member agent_ids via Team::add_member().

#include <batbox/tools/TeamCreateTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>
#include <batbox/agents/Team.hpp>
#include <batbox/core/Json.hpp>

#include <string>
#include <vector>

namespace batbox::tools {

// =============================================================================
// Construction
// =============================================================================

TeamCreateTool::TeamCreateTool(batbox::agents::TeamRegistry& registry)
    : registry_(registry)
{}

// =============================================================================
// ITool identity
// =============================================================================

std::string_view TeamCreateTool::name() const {
    return "TeamCreate";
}

std::string_view TeamCreateTool::description() const {
    return "Create a named team in the agent registry and optionally populate "
           "it with an initial list of member agent_ids; returns the team name "
           "and member list on success.";
}

// =============================================================================
// OpenAI tool schema
// =============================================================================

Json TeamCreateTool::schema_json() const {
    return Json{
        {"name",        "TeamCreate"},
        {"description", description()},
        {"parameters",  Json{
            {"type",       "object"},
            {"properties", Json{
                {"name", Json{
                    {"type",        "string"},
                    {"description", "Unique name for the new team; non-empty."}
                }},
                {"members", Json{
                    {"type",        "array"},
                    {"items",       Json{{"type", "string"}}},
                    {"description", "Optional array of agent_id strings to add "
                                    "as initial members of the team."}
                }}
            }},
            {"required", Json::array({"name"})}
        }}
    };
}

// =============================================================================
// Execution
// =============================================================================

ToolResult TeamCreateTool::run(const Json& args, ToolContext& ctx) {
    // --- Cancellation check ---
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    // --- Validate name ---
    if (!args.contains("name") || !args["name"].is_string()) {
        return ToolResult::error(
            "TeamCreate: missing or non-string 'name' argument");
    }

    const std::string team_name = args["name"].get<std::string>();

    if (team_name.empty()) {
        return ToolResult::error(
            "TeamCreate: 'name' must not be empty");
    }

    // --- Validate members (optional) ---
    std::vector<std::string> members;
    if (args.contains("members")) {
        const auto& members_arg = args["members"];
        if (!members_arg.is_array()) {
            return ToolResult::error(
                "TeamCreate: 'members' must be a JSON array of strings");
        }
        for (const auto& elem : members_arg) {
            if (!elem.is_string()) {
                return ToolResult::error(
                    "TeamCreate: each element of 'members' must be a string");
            }
            const std::string mid = elem.get<std::string>();
            if (!mid.empty()) {
                members.push_back(mid);
            }
        }
    }

    // --- Second cancellation check before registry mutation ---
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    // --- Create team (idempotent) and add members ---
    batbox::agents::Team* team = registry_.create_team(team_name);

    for (const auto& mid : members) {
        team->add_member(mid);
    }

    // --- Build result ---
    Json members_json = Json::array();
    for (const auto& mid : members) {
        members_json.push_back(mid);
    }

    Json payload = {
        {"team",    team_name},
        {"members", std::move(members_json)},
        {"status",  "created"}
    };

    return ToolResult::ok(payload.dump(2), std::move(payload));
}

} // namespace batbox::tools
