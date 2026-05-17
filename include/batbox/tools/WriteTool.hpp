// include/batbox/tools/WriteTool.hpp
//
// batbox::tools::WriteTool — write (create or overwrite) a file atomically.
//
// Contract (blueprints table rows 16630–16631, task CPP 5.4):
//
//   Tool name     : "Write"
//   Arguments     : args["path"]    — destination file path (string)
//                   args["content"] — new file contents (string)
//
//   Behaviour:
//     1. Refuse in Plan mode (is_plan_mode()).
//     2. Expand a leading "~/" in the path via std::filesystem.
//     3. Create all parent directories (std::filesystem::create_directories).
//     4. Write content to a unique temporary file in the same directory as the
//        destination, then rename it over the destination atomically.
//        On POSIX the rename is guaranteed to be atomic if src and dst are on
//        the same filesystem (same directory ensures this).
//     5. Read the old content (if any) and compute a unified diff to include
//        in the ToolResult body as a DiffCard.
//     6. Return ToolResult::ok(diff_body) on success.
//     7. Return ToolResult::error(msg) on any filesystem error.
//     8. requires_confirmation() == true  (write operation).
//     9. is_read_only()          == false (write operation).
//
//   Note on nuclear mode: nuclear mode bypasses the confirmation *prompt*
//   shown by the dispatch layer; WriteTool does not itself check is_nuclear().
//   The tool always writes unconditionally once dispatched — the caller (dispatch
//   layer) decides whether to prompt.
//
// Blueprint contract: batbox::tools::WriteTool (blueprints table row 16631)

#pragma once

#include <batbox/tools/ITool.hpp>
#include <batbox/core/Json.hpp>

#include <filesystem>
#include <string>

namespace batbox::tools {

// =============================================================================
// WriteTool
// =============================================================================

/// Implements the "Write" tool: creates or overwrites a file atomically using a
/// temp-file-then-rename strategy.  Parent directories are created on demand.
/// Returns a unified diff of the change in the ToolResult body.
class WriteTool final : public ITool {
public:
    WriteTool() = default;

    // -------------------------------------------------------------------------
    // ITool identity
    // -------------------------------------------------------------------------

    /// Returns "Write" — the stable tool name registered in the ToolRegistry.
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

    /// Write args["content"] to args["path"] atomically.
    ///
    /// Steps:
    ///   1. Refuse if ctx.is_plan_mode().
    ///   2. Resolve path (expand leading ~/) relative to ctx.cwd if relative.
    ///   3. Create parent directories.
    ///   4. Write to a temp file in the parent directory, rename over dst.
    ///   5. Build a unified diff (old → new) and include it in the body.
    ///
    /// @returns ToolResult::ok(unified_diff_body)  on success.
    ///          ToolResult::error(message)          on any error.
    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override;

    // -------------------------------------------------------------------------
    // Permission gate hooks
    // -------------------------------------------------------------------------

    /// false — WriteTool mutates the filesystem.
    [[nodiscard]] bool is_read_only() const override { return false; }

    /// true — a confirmation prompt should be shown before writing.
    [[nodiscard]] bool requires_confirmation() const override { return true; }
};

} // namespace batbox::tools
