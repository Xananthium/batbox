// include/batbox/tools/GlobTool.hpp
//
// batbox::tools::GlobTool — filesystem glob tool.
//
// Tool name: "Glob"
// Schema parameter:
//   pattern  (string, required) — fnmatch-style glob; ** crosses path separators,
//                                  * does not, ? matches one char, [abc]/[!abc]
//                                  bracket expressions.
//   path     (string, optional) — base directory to search from; defaults to
//                                  ctx.cwd when absent or empty.
//
// Behaviour:
//   1. Resolve the base directory: path arg if present, otherwise ctx.cwd.
//   2. Walk the tree with std::filesystem::recursive_directory_iterator.
//   3. Test each regular file's path (relative to base, forward-slash normalised)
//      against the pattern using batbox::permissions::glob_match.
//   4. Sort matching paths by last-write-time descending (newest first).
//   5. Return one absolute path per line in the ToolResult body.
//      Structured payload: { "matches": ["abs/path", ...], "count": N }
//
// Plan-mode: is_read_only() returns true — this tool may run in Plan mode.
// Confirmation: requires_confirmation() returns false — read-only, no prompt.
//
// Error handling:
//   - Missing / non-directory base path returns ToolResult::error.
//   - Permission errors on individual entries are silently skipped.
//   - Cancellation is polled between batches of 64 entries.
//   - All exceptions are caught and returned as ToolResult::error.
//
// Blueprint contract: batbox::tools::GlobTool (blueprints table rows 16636-16638)

#pragma once

#include <batbox/tools/ITool.hpp>

namespace batbox::tools {

// =============================================================================
// GlobTool
// =============================================================================

class GlobTool final : public ITool {
public:
    GlobTool() = default;

    // -------------------------------------------------------------------------
    // ITool interface
    // -------------------------------------------------------------------------

    /// Tool name used by the model: "Glob".
    [[nodiscard]] std::string_view name() const override;

    /// One-sentence description for the model's tool schema.
    [[nodiscard]] std::string_view description() const override;

    /// OpenAI tools[*].function JSON object for this tool.
    [[nodiscard]] Json schema_json() const override;

    /// Execute the glob.
    ///
    /// args must contain:
    ///   "pattern"  — glob pattern string (required)
    ///   "path"     — base directory string (optional; defaults to ctx.cwd)
    ///
    /// Returns ToolResult::ok with matching absolute paths, one per line,
    /// sorted by mtime descending, plus a structured JSON payload.
    /// Returns ToolResult::error on invalid base directory or other failures.
    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override;

    /// Returns true — Glob only reads the filesystem, never writes.
    [[nodiscard]] bool is_read_only() const override { return true; }

    /// Returns false — no confirmation prompt needed for a read-only tool.
    [[nodiscard]] bool requires_confirmation() const override { return false; }
};

} // namespace batbox::tools
