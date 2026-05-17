// include/batbox/tools/EditTool.hpp
//
// batbox::tools::EditTool — exact-string-match file editor with atomic write.
//
// Contract (blueprints table, task CPP 5.5):
//
//   Tool name     : "Edit"
//   Arguments     : args["path"]        — target file path (string)
//                   args["old_string"]  — exact text to find and replace (string)
//                   args["new_string"]  — replacement text (string)
//                   args["replace_all"] — optional bool (default false); when true
//                                         all occurrences are replaced instead of
//                                         requiring exactly one.
//
//   Behaviour:
//     1. Refuse in Plan mode (is_plan_mode()).
//     2. Resolve args["path"] relative to ctx.cwd when it is a relative path;
//        expand a leading "~/" to the home directory otherwise.
//     3. Read the entire file content into memory.
//     4. Count exact (byte-for-byte) occurrences of old_string in the content.
//     5. If replace_all == false (default):
//          - 0 matches  →  ToolResult::error("old_string not found in <path>")
//          - N > 1      →  ToolResult::error(
//                            "found N matches; pass replace_all:true or pick a
//                             longer unique snippet")
//     6. If replace_all == true:
//          Replace all N occurrences (N may be 0, producing an error the same
//          as case 5 — 0 matches).
//     7. Write the modified content to a unique temporary file in the same
//        parent directory as the target, then rename it over the target
//        atomically (POSIX-rename guarantee on the same filesystem).
//     8. Compute a unified diff (--- a/  +++ b/  @@ hunks) of the original
//        vs modified content and return it as the ToolResult body.
//     9. Return ToolResult::ok(diff_body, payload) on success where payload
//        carries { "replacements": N, "path": "..." }.
//    10. Return ToolResult::error(msg) on any error (missing args, file I/O,
//        match errors).
//    11. requires_confirmation() == true  (mutates the filesystem).
//    12. is_read_only()          == false (mutates the filesystem).
//
// Blueprint contract: batbox::tools::EditTool (blueprints table, task CPP 5.5)

#pragma once

#include <batbox/tools/ITool.hpp>
#include <batbox/core/Json.hpp>

#include <filesystem>
#include <string>

namespace batbox::tools {

// =============================================================================
// EditTool
// =============================================================================

/// Implements the "Edit" tool: performs an exact-string-match replacement in a
/// file and writes it back atomically, returning a unified diff in the body.
class EditTool final : public ITool {
public:
    EditTool() = default;

    // -------------------------------------------------------------------------
    // ITool identity
    // -------------------------------------------------------------------------

    /// Returns "Edit" — the stable tool name registered in the ToolRegistry.
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

    /// Edit the file at args["path"]: replace args["old_string"] with
    /// args["new_string"], writing the result back atomically.
    ///
    /// Steps:
    ///   1. Refuse if ctx.is_plan_mode().
    ///   2. Validate required args (path, old_string, new_string).
    ///   3. Resolve path relative to ctx.cwd or expand ~/ prefix.
    ///   4. Read file contents.
    ///   5. Count exact occurrences of old_string.
    ///   6. Enforce uniqueness contract (unless replace_all == true).
    ///   7. Replace and write atomically via temp-file rename.
    ///   8. Build and return unified diff.
    ///
    /// @returns ToolResult::ok(unified_diff_body, payload)  on success.
    ///          ToolResult::error(message)                   on any error.
    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override;

    // -------------------------------------------------------------------------
    // Permission gate hooks
    // -------------------------------------------------------------------------

    /// false — EditTool mutates the filesystem.
    [[nodiscard]] bool is_read_only() const override { return false; }

    /// true — a confirmation prompt should be shown before editing.
    [[nodiscard]] bool requires_confirmation() const override { return true; }
};

} // namespace batbox::tools
