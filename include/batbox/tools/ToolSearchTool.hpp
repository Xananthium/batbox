// include/batbox/tools/ToolSearchTool.hpp
//
// batbox::tools::ToolSearchTool — fuzzy/substring search over the ToolRegistry.
//
// Tool name : "ToolSearch"
// is_read_only()          : true  (allowed in Plan mode)
// requires_confirmation() : false (read-only, no side effects)
//
// Arguments (JSON args):
//   query       (string, required)  — Search string.
//                                     Special form: "select:Name1,Name2[,...]"
//                                     performs exact-name lookup and returns
//                                     the schemas for each named tool directly.
//   max_results (int,    optional)  — Maximum hits to return; default 10, max 50.
//
// Scoring (when not in select: mode):
//   1. Both the tool name and description are searched case-insensitively.
//   2. Substring hit in name   → name_score   = 1.0
//      Fuzzy    hit in name   → name_score   = fuzzy_quality (< 1.0, > 0)
//      No hit in name         → name_score   = 0.0
//   3. Substring hit in desc   → desc_score   = 1.0
//      Fuzzy    hit in desc   → desc_score   = fuzzy_quality × 0.5 (down-weighted)
//      No hit in desc         → desc_score   = 0.0
//   4. final_score = max(name_score, desc_score)
//      Ties resolved by registration order (stable sort).
//   5. Only tools with final_score > 0 are included.
//
// Fuzzy quality formula (adapted from HistorySearch, CPP 2.2):
//   - Check for exact substring: return 1.0 if found.
//   - Walk entry chars, consume query chars in order; count first/last match pos.
//     If not all query chars found: return 0.0.
//     Otherwise:
//       span      = last_match_pos - first_match_pos + 1
//       gaps      = span - query_length
//       gap_ratio = gaps / entry_length
//       quality   = 1.0 - 0.4 × gap_ratio        (clamped to [0,1])
//
// Output format (ToolResult body):
//   Found N tool(s) matching "<query>":
//
//   [1] <ToolName> (score: 0.95)
//   Description: <description>
//   Schema:
//   <schema_json pretty-printed>
//
//   [2] ...
//
// Structured payload:
//   { "matches": [ { "name": ..., "score": ..., "schema": <schema_json> }, ... ] }
//
// Injection pattern:
//   ToolSearchTool takes a const ToolRegistry& at construction time.  The
//   registry is read-only after startup so this is safe for concurrent calls.
//
// Blueprint contract: batbox::tools::ToolSearchTool (blueprints table, task CPP 5.14)

#pragma once

#include <batbox/tools/ITool.hpp>
#include <batbox/tools/ToolRegistry.hpp>

namespace batbox::tools {

// =============================================================================
// ToolSearchTool
// =============================================================================

class ToolSearchTool final : public ITool {
public:
    // -------------------------------------------------------------------------
    // Construction
    //
    // registry — const reference to the ToolRegistry; must outlive this tool.
    //            Typically the one owned by App::init() and passed to every
    //            tool that needs to inspect the registry.
    // -------------------------------------------------------------------------
    explicit ToolSearchTool(const ToolRegistry& registry) noexcept;

    // -------------------------------------------------------------------------
    // ITool interface
    // -------------------------------------------------------------------------

    /// Tool name: "ToolSearch".
    [[nodiscard]] std::string_view name() const override;

    /// One-sentence description for the model's tool schema.
    [[nodiscard]] std::string_view description() const override;

    /// OpenAI tools[*].function JSON object for this tool.
    [[nodiscard]] Json schema_json() const override;

    /// Execute the search.
    ///
    /// args must contain:
    ///   "query"       — search string (required)
    ///   "max_results" — int limit (optional; default 10, capped at 50)
    ///
    /// Special form: when query begins with "select:" the remainder is parsed
    /// as a comma-separated list of exact tool names to retrieve.  Each named
    /// tool's schema is returned verbatim.  Names not found are reported in
    /// the body but do NOT cause is_error=true.
    ///
    /// Returns ToolResult::ok with a ranked list of matching tools including
    /// their full schemas, or ToolResult::error for argument validation failures.
    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override;

    /// Returns true — ToolSearch only reads the registry, never mutates state.
    [[nodiscard]] bool is_read_only() const override { return true; }

    /// Returns false — no confirmation prompt needed; purely informational.
    [[nodiscard]] bool requires_confirmation() const override { return false; }

    // -------------------------------------------------------------------------
    // Constants (public so tests can reference them)
    // -------------------------------------------------------------------------
    static constexpr int kDefaultMaxResults = 10;
    static constexpr int kMaxResultsCap     = 50;

private:
    // -------------------------------------------------------------------------
    // score_match(text_lower, query_lower)
    //
    // Returns match quality in [0, 1]:
    //   1.0   — contiguous substring
    //   (0,1) — fuzzy (all query chars in order)
    //   0.0   — no match
    //
    // Both arguments must be pre-lowercased.
    // -------------------------------------------------------------------------
    [[nodiscard]] static float score_match(std::string_view text_lower,
                                           std::string_view query_lower) noexcept;

    // -------------------------------------------------------------------------
    // to_lower(s) — ASCII case fold to a new string.
    // -------------------------------------------------------------------------
    [[nodiscard]] static std::string to_lower(std::string_view s);

    const ToolRegistry& registry_;  ///< Non-owning ref; outlives this tool.
};

} // namespace batbox::tools
