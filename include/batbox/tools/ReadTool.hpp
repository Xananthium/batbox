// include/batbox/tools/ReadTool.hpp
//
// batbox::tools::ReadTool — ITool implementation that reads a file and
// returns its contents with cat -n style line numbers.
//
// Contract (CPP 5.3 blueprint):
//   name()         = "Read"
//   is_read_only() = true   (allowed in Plan mode)
//   requires_confirmation() = false
//
// Parameters (JSON args):
//   file_path  (string, required)  — path to read; relative resolved against ctx.cwd.
//   offset     (int, optional)     — 1-based first line to include; default 1.
//   limit      (int, optional)     — max lines to return; default 2000.
//
// Output format (cat -n style, 1-indexed):
//     1→line one
//     2→line two
//   ...
//
// Error handling (returns ToolResult::error):
//   - file_path argument missing or not a string
//   - resolved path does not exist
//   - resolved path is a directory
//   - permission denied
//   - file exceeds 5 MB (hard cap, enforced before reading)
//   - cancellation requested
//
// Image / binary detection:
//   Known image extensions (.png .jpg .jpeg .gif .bmp .webp .svg .ico .tiff)
//   return a text placeholder: "(image: <ext> file, use a vision-capable model
//   to inspect this file)".  Binary files (null bytes in first 8 KB) return a
//   similar placeholder.
//
// Blueprint contract: batbox::tools::ReadTool (CPP 5.3)

#pragma once

#include <batbox/tools/ITool.hpp>

namespace batbox::tools {

// =============================================================================
// ReadTool
// =============================================================================

class ReadTool final : public ITool {
public:
    ReadTool() = default;

    // -------------------------------------------------------------------------
    // ITool identity
    // -------------------------------------------------------------------------
    [[nodiscard]] std::string_view name()        const override;
    [[nodiscard]] std::string_view description() const override;
    [[nodiscard]] Json             schema_json() const override;

    // -------------------------------------------------------------------------
    // ITool execution
    // -------------------------------------------------------------------------
    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override;

    // -------------------------------------------------------------------------
    // Permission gate hooks
    // -------------------------------------------------------------------------
    /// ReadTool never mutates state; allowed in Plan mode.
    [[nodiscard]] bool is_read_only()          const override { return true;  }
    /// No confirmation prompt needed for a read-only operation.
    [[nodiscard]] bool requires_confirmation() const override { return false; }

    // -------------------------------------------------------------------------
    // Constants (public so tests can reference them)
    // -------------------------------------------------------------------------
    static constexpr std::size_t kDefaultLineLimit = 2000;
    static constexpr std::size_t kMaxFileSizeBytes = 5 * 1024 * 1024; // 5 MB
};

} // namespace batbox::tools
