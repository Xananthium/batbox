// src/tools/SelectionHeuristic.cpp
//
// Implementation of batbox::tools::ShapeSelectionHeuristic (DIS-1007).
// See include/batbox/tools/SelectionHeuristic.hpp for the full contract and the
// per-signal rationale of the investigation-vs-lookup judgment.

#include <batbox/tools/SelectionHeuristic.hpp>

#include <array>
#include <cstddef>
#include <string_view>

namespace batbox::tools {

// =============================================================================
// is_investigation_tool — the small, documented broad-search tool-name list.
//
// Each entry is justified in the header.  Kept small ON PURPOSE: tool identity
// may PROMOTE a result to investigation, but a tool NOT on this list is not
// demoted — a genuinely broad result still trips the result-shape signal.  This
// is the DIFFERENT-from-S1 decision: S1 is identity-blind (size is the cost);
// here identity is the cheapest reliable predictor of "this is the start of a
// dig the parent will follow up on."
// =============================================================================

bool ShapeSelectionHeuristic::is_investigation_tool(std::string_view tool_name) noexcept {
    // Canonical batbox tool names (see src/tools/DIRECTORY.md): GrepTool="grep",
    // GlobTool="glob", WebSearchTool="web_search", WebFetchTool="web_fetch".
    static constexpr std::array<std::string_view, 4> kInvestigationTools{
        "grep",        // pattern search across a corpus → many hits / follow-ups
        "glob",        // filesystem pattern match → a set of paths to dig through
        "web_search",  // ranked query results → a survey the parent narrows
        "web_fetch",   // pulls a whole page/document → a large follow-up surface
    };
    for (const auto& t : kInvestigationTools) {
        if (t == tool_name) {
            return true;
        }
    }
    return false;
}

// =============================================================================
// classify — tool semantics OR result shape → DispatchMode.
// =============================================================================

DispatchMode ShapeSelectionHeuristic::classify(std::string_view  tool_name,
                                               const Json&       /*args*/,
                                               const ToolResult& result) const {
    // An error result is never an investigation: it must surface verbatim so the
    // model self-corrects, and there is nothing worth keeping a warm window for.
    if (result.is_error) {
        return DispatchMode::Closed;
    }

    // (1) TOOL SEMANTICS — a known broad-search tool reads as investigation.
    if (is_investigation_tool(tool_name)) {
        return DispatchMode::Standing;
    }

    // (2) RESULT SHAPE — identity-independent corroboration.

    // Large body → a broad surface the parent will likely follow up against.
    if (result.body.size() > params_.large_body_bytes) {
        return DispatchMode::Standing;
    }

    // Many distinct non-empty sections (newline-delimited hits/records) → a dig,
    // not a single resolved value.  Count cheaply without allocating.
    std::size_t sections = 0;
    bool        in_section = false;
    for (const char c : result.body) {
        if (c == '\n') {
            in_section = false;
        } else if (!in_section) {
            in_section = true;
            ++sections;
            if (sections >= params_.many_sections) {
                return DispatchMode::Standing;
            }
        }
    }

    // Otherwise: a single resolved fact → LOOKUP (the LEAST_FORCE default).
    return DispatchMode::Closed;
}

} // namespace batbox::tools
