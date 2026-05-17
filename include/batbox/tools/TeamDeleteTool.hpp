// include/batbox/tools/TeamDeleteTool.hpp
//
// batbox::tools::TeamDeleteTool — ITool implementation that removes a named
// team from the TeamRegistry.
//
// Blueprint contract (task CPP 5.29, blueprints rows 16740–16742):
//   class TeamDeleteTool : public ITool
//   name() = "TeamDelete"
//   Delegates to TeamRegistry::delete_team(name).
//
// Tool arguments (JSON object):
//   name (string, required) — team name to delete; non-empty
//
// Returns JSON object on success:
//   {
//     "team":   "<name>",
//     "status": "deleted"
//   }
//
// If the team does not exist, returns status="not_found" (not an error —
// delete is idempotent from the model's perspective).
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
// TeamDeleteTool
// =============================================================================

/// Implements the "TeamDelete" tool: removes a named team from the TeamRegistry.
///
/// Blueprint contract: batbox::tools::TeamDeleteTool (blueprints rows 16740–16742)
class TeamDeleteTool final : public ITool {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    /// Construct with a reference to the process-wide TeamRegistry.
    explicit TeamDeleteTool(batbox::agents::TeamRegistry& registry);

    // -------------------------------------------------------------------------
    // ITool identity
    // -------------------------------------------------------------------------

    /// Returns "TeamDelete".
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

    /// Delete the named team from the registry.
    ///
    /// args["name"] — required non-empty string
    ///
    /// @returns ToolResult::ok(json)   always (even if team was not found).
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
