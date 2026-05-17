// src/tools/TeamDeleteTool.cpp
//
// Implementation of batbox::tools::TeamDeleteTool.
//
// Blueprint contract (task CPP 5.29, blueprints rows 16740–16742):
//   TeamDeleteTool::run — calls TeamRegistry::delete_team(name).
//   If the team does not exist the call is a no-op; returns status="not_found".

#include <batbox/tools/TeamDeleteTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>
#include <batbox/agents/Team.hpp>
#include <batbox/core/Json.hpp>

#include <string>

namespace batbox::tools {

// =============================================================================
// Construction
// =============================================================================

TeamDeleteTool::TeamDeleteTool(batbox::agents::TeamRegistry& registry)
    : registry_(registry)
{}

// =============================================================================
// ITool identity
// =============================================================================

std::string_view TeamDeleteTool::name() const {
    return "TeamDelete";
}

std::string_view TeamDeleteTool::description() const {
    return "Delete a named team from the agent registry, disbanding its "
           "membership; no-op if the team does not exist.";
}

// =============================================================================
// OpenAI tool schema
// =============================================================================

Json TeamDeleteTool::schema_json() const {
    return Json{
        {"name",        "TeamDelete"},
        {"description", description()},
        {"parameters",  Json{
            {"type",       "object"},
            {"properties", Json{
                {"name", Json{
                    {"type",        "string"},
                    {"description", "Name of the team to delete; non-empty."}
                }}
            }},
            {"required", Json::array({"name"})}
        }}
    };
}

// =============================================================================
// Execution
// =============================================================================

ToolResult TeamDeleteTool::run(const Json& args, ToolContext& ctx) {
    // --- Cancellation check ---
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    // --- Validate name ---
    if (!args.contains("name") || !args["name"].is_string()) {
        return ToolResult::error(
            "TeamDelete: missing or non-string 'name' argument");
    }

    const std::string team_name = args["name"].get<std::string>();

    if (team_name.empty()) {
        return ToolResult::error(
            "TeamDelete: 'name' must not be empty");
    }

    // --- Check existence, then delete ---
    const bool existed = (registry_.get_team(team_name) != nullptr);

    // delete_team is a no-op if the team is not found — safe to call unconditionally.
    registry_.delete_team(team_name);

    // --- Build result ---
    const std::string status = existed ? "deleted" : "not_found";

    Json payload = {
        {"team",   team_name},
        {"status", status}
    };

    return ToolResult::ok(payload.dump(2), std::move(payload));
}

} // namespace batbox::tools
