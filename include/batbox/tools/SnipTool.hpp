// include/batbox/tools/SnipTool.hpp
//
// batbox::tools::SnipTool — persist named code/text snippets to local storage.
//
// Contract (blueprints table rows 16725-16727, task CPP 5.25):
//
//   Tool name     : "Snip"
//   Storage root  : ~/.batbox/snippets/   (created on first use)
//   Arguments     :
//     action  (string, required) — one of: "save", "load", "list", "delete"
//     name    (string, optional) — snippet identifier; required for save/load/delete
//     content (string, optional) — snippet text; required for save
//
//   Action semantics:
//     save   — write content to ~/.batbox/snippets/<name>.txt
//              Creates the directory if absent.
//              Returns ToolResult::ok with the saved file path.
//              Fails in Plan mode (is_plan_mode()).
//     load   — read and return content of ~/.batbox/snippets/<name>.txt.
//              Returns ToolResult::error if the snippet does not exist.
//              Allowed in Plan mode (read-only path).
//     list   — return all snippet names with first-line preview.
//              Format: "<name>: <first line of content>\n" per snippet.
//              Returns empty list message if no snippets exist.
//              Allowed in Plan mode.
//     delete — remove ~/.batbox/snippets/<name>.txt.
//              Returns ToolResult::error if the snippet does not exist.
//              Fails in Plan mode.
//
//   Validation:
//     - Name must not be empty and must contain only [a-zA-Z0-9_.-] characters.
//       This prevents path-traversal attacks.
//     - Names must not contain path separators or ".." components.
//
//   Permission flags:
//     is_read_only()          — false  (save/delete are mutating)
//     requires_confirmation() — false  (snippet store is user-local, no confirmation needed)
//
// Blueprint contract: batbox::tools::SnipTool (blueprints table rows 16725-16727)

#pragma once

#include <batbox/tools/ITool.hpp>
#include <batbox/core/Json.hpp>

#include <filesystem>

namespace batbox::tools {

// =============================================================================
// SnipTool
// =============================================================================

/// Implements the "Snip" tool: persist and retrieve named text/code snippets
/// in ~/.batbox/snippets/.  Supports save, load, list, and delete actions.
class SnipTool final : public ITool {
public:
    SnipTool() = default;

    // -------------------------------------------------------------------------
    // ITool identity
    // -------------------------------------------------------------------------

    /// Returns "Snip" — stable tool name registered in ToolRegistry.
    [[nodiscard]] std::string_view name() const override;

    /// Returns a one-sentence description surfaced to the model in the schema.
    [[nodiscard]] std::string_view description() const override;

    // -------------------------------------------------------------------------
    // OpenAI tool schema
    // -------------------------------------------------------------------------

    /// Returns the full OpenAI tools[*].function JSON object for this tool.
    [[nodiscard]] Json schema_json() const override;

    // -------------------------------------------------------------------------
    // Execution
    // -------------------------------------------------------------------------

    /// Execute a snippet operation.
    ///
    /// args["action"]  — "save" | "load" | "list" | "delete"  (required)
    /// args["name"]    — snippet identifier                    (required for save/load/delete)
    /// args["content"] — text to persist                      (required for save)
    ///
    /// @returns ToolResult::ok(...)   on success.
    ///          ToolResult::error(…)  on missing/invalid args, permission denial,
    ///                                or filesystem errors.
    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override;

    // -------------------------------------------------------------------------
    // Permission gate hooks
    // -------------------------------------------------------------------------

    /// false — SnipTool can mutate the filesystem (save/delete actions).
    [[nodiscard]] bool is_read_only() const override { return false; }

    /// false — snippet storage is user-local; no interactive confirmation needed.
    [[nodiscard]] bool requires_confirmation() const override { return false; }

private:
    // -------------------------------------------------------------------------
    // Internal helpers
    // -------------------------------------------------------------------------

    /// Returns the canonical snippets directory: ~/.batbox/snippets/
    /// Does not create it; call ensure_snippets_dir() before writing.
    [[nodiscard]] static std::filesystem::path snippets_dir();

    /// Creates the snippets directory (and all parents) if absent.
    /// Returns an error string on failure, or an empty string on success.
    [[nodiscard]] static std::string ensure_snippets_dir();

    /// Validates that `name` contains only safe characters [a-zA-Z0-9_.-]
    /// and does not start with '.'.  Returns an error message or empty string.
    [[nodiscard]] static std::string validate_name(const std::string& name);

    /// Returns the full path for a named snippet: <snippets_dir>/<name>.txt
    [[nodiscard]] static std::filesystem::path snippet_path(const std::string& name);

    // -------------------------------------------------------------------------
    // Action implementations
    // -------------------------------------------------------------------------

    [[nodiscard]] static ToolResult action_save(const std::string& name,
                                                const std::string& content);

    [[nodiscard]] static ToolResult action_load(const std::string& name,
                                                ToolContext& ctx);

    [[nodiscard]] static ToolResult action_list(ToolContext& ctx);

    [[nodiscard]] static ToolResult action_delete(const std::string& name);
};

} // namespace batbox::tools
