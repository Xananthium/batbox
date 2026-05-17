// include/batbox/tools/GrepTool.hpp
//
// batbox::tools::GrepTool — regex search tool.
//
// Preferred back-end: shells out to `rg` (ripgrep) when the binary is on PATH.
// Fallback back-end: pure C++ recursive walker using std::regex.
//
// Tool name: "Grep"
// is_read_only(): true
// requires_confirmation(): false
//
// JSON arguments accepted by run():
//
//   pattern          (string, required) — ECMAScript / Rust regex pattern.
//   path             (string, optional) — Directory or file to search.
//                    Defaults to ctx.cwd when absent.
//   glob             (string, optional) — File-name glob filter, e.g. "*.cpp".
//                    Passed to rg as -g; applied by the fallback walker too.
//   type             (string, optional) — ripgrep file-type alias, e.g. "cpp".
//                    Ignored by the fallback back-end (documented limitation).
//   output_mode      (string, optional) — one of:
//                      "text"              (default) file:line:content lines
//                      "files_with_matches"  deduplicated list of matching paths
//                      "count"               count-per-file  "N  file" lines
//   case_insensitive (bool,   optional) — -i flag equivalent. Default false.
//   line_numbers     (bool,   optional) — include line numbers in text mode.
//                    Default true.
//   context_before   (int,   optional)  — lines of context before each match (-B).
//   context_after    (int,   optional)  — lines of context after  each match (-A).
//   context          (int,   optional)  — symmetric context lines (-C).
//                    context_before/context_after override context when both present.
//   head_limit       (int,   optional)  — cap on output lines (applied after all
//                    other filtering). 0 or absent = unlimited.
//   multiline        (bool,  optional)  — enable multiline matching. When using
//                    rg, passes -U. The fallback back-end uses
//                    std::regex::multiline. Default false.
//
// Output format ("text" mode, with line_numbers=true):
//   /abs/path/to/file.cpp:42:    matching line content
//
// Fallback limitations (documented):
//   - `type` arg is ignored.
//   - Context flags (-A/-B/-C) are not supported; lines are returned without
//     surrounding context.
//   - multiline patterns that span physical lines are not supported.
//   The result body will include a NOTE line when the fallback is active and
//   unsupported flags were supplied.
//
// Blueprint contract: batbox::tools::GrepTool (blueprints table, task CPP 5.7)

#pragma once

#include <batbox/tools/ITool.hpp>

namespace batbox::tools {

// =============================================================================
// GrepTool
// =============================================================================

class GrepTool final : public ITool {
public:
    GrepTool() = default;

    // -------------------------------------------------------------------------
    // ITool identity
    // -------------------------------------------------------------------------

    [[nodiscard]] std::string_view name()        const override;
    [[nodiscard]] std::string_view description() const override;
    [[nodiscard]] Json             schema_json()  const override;

    // -------------------------------------------------------------------------
    // Execution
    // -------------------------------------------------------------------------

    /// Execute the grep search.
    ///
    /// Tries ripgrep first (fast, full feature parity).  Falls back to the
    /// pure-C++ recursive walker when `rg` is not on PATH.
    ///
    /// Returns ToolResult::error(...) for semantic failures (no pattern, bad
    /// path, regex compile error).  Never throws.
    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override;

    // -------------------------------------------------------------------------
    // Permission gates
    // -------------------------------------------------------------------------

    [[nodiscard]] bool is_read_only()          const override { return true;  }
    [[nodiscard]] bool requires_confirmation() const override { return false; }
};

} // namespace batbox::tools
