// src/tools/ToolSearchTool.cpp
//
// Implementation of batbox::tools::ToolSearchTool.
//
// Scoring algorithm (adapted from HistorySearch, CPP 2.2):
//
//   score_match(text, query):
//     1. Exact substring hit      → quality = 1.0
//     2. Fuzzy hit (all query chars appear in order):
//          span      = last_matched_pos - first_matched_pos + 1
//          gaps      = span - query_length
//          gap_ratio = gaps / text_length
//          quality   = 1.0 - 0.4 × gap_ratio    (clamped to [0, 1])
//     3. No hit                    → quality = 0.0
//
//   final_score = max(name_quality, desc_quality × 0.5)
//   (Description matches are down-weighted so name matches rank higher.)
//
// select: form:
//   query = "select:ReadFile,Bash,Glob"
//   → splits on comma after "select:" prefix
//   → returns schemas for each named tool exactly, in order
//   → tools not found are mentioned in body but result is not is_error
//
// Blueprint contract: batbox::tools::ToolSearchTool (blueprints table, task CPP 5.14)

#include <batbox/tools/ToolSearchTool.hpp>

#include <batbox/core/Json.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace batbox::tools {

namespace {

// ---------------------------------------------------------------------------
// Helper: split a string_view on a delimiter character.
// ---------------------------------------------------------------------------
static std::vector<std::string> split(std::string_view sv, char delim) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= sv.size()) {
        const auto pos = sv.find(delim, start);
        const auto end = (pos == std::string_view::npos) ? sv.size() : pos;
        // Trim whitespace from each token.
        std::string_view tok = sv.substr(start, end - start);
        while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.front()))) {
            tok.remove_prefix(1);
        }
        while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.back()))) {
            tok.remove_suffix(1);
        }
        if (!tok.empty()) {
            parts.emplace_back(tok);
        }
        if (pos == std::string_view::npos) break;
        start = pos + 1;
    }
    return parts;
}

// ---------------------------------------------------------------------------
// Helper: pretty-print a Json value with 2-space indentation.
// Delegates to nlohmann::json::dump.
// ---------------------------------------------------------------------------
static std::string pretty(const Json& j) {
    return j.dump(2);
}

} // anonymous namespace

// =============================================================================
// Construction
// =============================================================================

ToolSearchTool::ToolSearchTool(const ToolRegistry& registry) noexcept
    : registry_{registry}
{}

// =============================================================================
// Identity
// =============================================================================

std::string_view ToolSearchTool::name() const {
    return "ToolSearch";
}

std::string_view ToolSearchTool::description() const {
    return "Search the tool registry by name or description; "
           "returns matching tools with their full schemas so you can invoke them immediately.";
}

Json ToolSearchTool::schema_json() const {
    return Json{
        {"name",        "ToolSearch"},
        {"description",
         "Search the tool registry by name or description; "
         "returns matching tools with their full schemas so you can invoke them immediately."},
        {"parameters", Json{
            {"type",       "object"},
            {"properties", Json{
                {"query", Json{
                    {"type",        "string"},
                    {"description",
                     "Search string.  Use 'select:Name1,Name2' to retrieve tools by exact name."}
                }},
                {"max_results", Json{
                    {"type",        "integer"},
                    {"description", "Maximum number of results to return (default 10, max 50)."},
                    {"default",     kDefaultMaxResults},
                    {"minimum",     1},
                    {"maximum",     kMaxResultsCap}
                }}
            }},
            {"required", Json::array({"query"})}
        }}
    };
}

// =============================================================================
// Scoring helpers
// =============================================================================

/*static*/ std::string ToolSearchTool::to_lower(std::string_view s) {
    std::string result;
    result.reserve(s.size());
    for (const unsigned char c : s) {
        result.push_back(static_cast<char>(std::tolower(c)));
    }
    return result;
}

/*static*/ float ToolSearchTool::score_match(std::string_view text_lower,
                                              std::string_view query_lower) noexcept {
    if (query_lower.empty()) {
        return 1.0f;  // Empty query matches everything.
    }
    if (text_lower.empty()) {
        return 0.0f;
    }

    // --- Exact substring check ---
    if (text_lower.find(query_lower) != std::string_view::npos) {
        return 1.0f;
    }

    // --- Fuzzy check: all query chars must appear in order ---
    const auto qlen = static_cast<int>(query_lower.size());
    const auto tlen = static_cast<int>(text_lower.size());

    int qi             = 0;
    int first_match    = -1;
    int last_match     = -1;

    for (int ti = 0; ti < tlen && qi < qlen; ++ti) {
        if (text_lower[static_cast<std::size_t>(ti)] ==
            query_lower[static_cast<std::size_t>(qi)]) {
            if (first_match == -1) {
                first_match = ti;
            }
            last_match = ti;
            ++qi;
        }
    }

    if (qi < qlen) {
        // Not all query characters found — no match.
        return 0.0f;
    }

    // Compute gap penalty from the matched span.
    const int   span      = last_match - first_match + 1;
    const int   gaps      = span - qlen;
    const float gap_ratio = static_cast<float>(gaps) / static_cast<float>(tlen);
    const float quality   = 1.0f - 0.4f * gap_ratio;

    // Clamp against floating-point edge cases.
    return (quality < 0.0f) ? 0.0f : (quality > 1.0f ? 1.0f : quality);
}

// =============================================================================
// run()
// =============================================================================

ToolResult ToolSearchTool::run(const Json& args, ToolContext& ctx) {
    // -------------------------------------------------------------------------
    // 1. Extract and validate arguments.
    // -------------------------------------------------------------------------
    if (!args.contains("query") || !args["query"].is_string()) {
        return ToolResult::error("ToolSearch: missing required argument 'query' (string).");
    }
    const std::string query = args["query"].get<std::string>();

    int max_results = kDefaultMaxResults;
    if (args.contains("max_results") && args["max_results"].is_number_integer()) {
        max_results = args["max_results"].get<int>();
        if (max_results < 1)   max_results = 1;
        if (max_results > kMaxResultsCap) max_results = kMaxResultsCap;
    }

    // -------------------------------------------------------------------------
    // 2. Check for cancellation before doing any work.
    // -------------------------------------------------------------------------
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    // -------------------------------------------------------------------------
    // 3. "select:Name1,Name2[,...]" — exact-name retrieval path.
    // -------------------------------------------------------------------------
    static constexpr std::string_view kSelectPrefix = "select:";
    if (query.size() >= kSelectPrefix.size() &&
        std::string_view{query}.substr(0, kSelectPrefix.size()) == kSelectPrefix) {

        const std::string_view remainder{query.data() + kSelectPrefix.size(),
                                         query.size() - kSelectPrefix.size()};
        const auto names = split(remainder, ',');

        if (names.empty()) {
            return ToolResult::error(
                "ToolSearch: 'select:' form requires at least one tool name "
                "(e.g. 'select:Read,Bash').");
        }

        std::ostringstream body;
        Json payload = Json::array();
        int  found   = 0;
        std::vector<std::string> missing;

        body << "ToolSearch — select: results for [";
        for (std::size_t i = 0; i < names.size(); ++i) {
            body << names[i];
            if (i + 1 < names.size()) body << ", ";
        }
        body << "]:\n\n";

        for (const auto& tool_name : names) {
            const ITool* tool = registry_.find_by_name(tool_name);
            if (tool == nullptr) {
                missing.push_back(tool_name);
                body << "[not found] " << tool_name << "\n\n";
                continue;
            }
            ++found;
            body << "[" << found << "] " << tool->name() << "\n"
                 << "Description: " << tool->description() << "\n"
                 << "Schema:\n"
                 << pretty(tool->schema_json()) << "\n\n";

            payload.push_back(Json{
                {"name",   std::string{tool->name()}},
                {"schema", tool->schema_json()}
            });
        }

        if (!missing.empty()) {
            body << "Not found: ";
            for (std::size_t i = 0; i < missing.size(); ++i) {
                body << missing[i];
                if (i + 1 < missing.size()) body << ", ";
            }
            body << "\n";
        }

        return ToolResult::ok(body.str(),
                              Json{{"matches", std::move(payload)},
                                   {"missing", Json(missing)}});
    }

    // -------------------------------------------------------------------------
    // 4. Fuzzy/substring search path.
    // -------------------------------------------------------------------------
    const std::string query_lower = to_lower(query);

    // Collect all registered tool schemas to iterate.
    const auto all_schemas = registry_.available_tool_schemas();  // registered order

    struct Hit {
        float       score;
        std::string tool_name;
        std::string tool_description;
        Json        tool_schema;  // schema_json() result (not the outer envelope)
    };

    std::vector<Hit> hits;
    hits.reserve(all_schemas.size());

    // Walk schemas; extract the inner "function" object.
    for (const auto& envelope : all_schemas) {
        if (ctx.is_cancelled()) {
            return ToolResult::error("cancelled");
        }

        if (!envelope.contains("function") || !envelope["function"].is_object()) {
            continue;
        }
        const Json& fn = envelope["function"];

        const std::string tool_name =
            fn.contains("name") && fn["name"].is_string()
                ? fn["name"].get<std::string>()
                : std::string{};
        const std::string tool_desc =
            fn.contains("description") && fn["description"].is_string()
                ? fn["description"].get<std::string>()
                : std::string{};

        const std::string name_lower = to_lower(tool_name);
        const std::string desc_lower = to_lower(tool_desc);

        const float name_score = score_match(name_lower, query_lower);
        const float desc_score = score_match(desc_lower, query_lower) * 0.5f;

        const float final_score = (name_score > desc_score) ? name_score : desc_score;

        if (final_score > 0.0f) {
            hits.push_back(Hit{final_score, tool_name, tool_desc, fn});
        }
    }

    // Stable descending sort — ties preserve registration order.
    std::stable_sort(hits.begin(), hits.end(),
                     [](const Hit& a, const Hit& b) { return a.score > b.score; });

    // Clamp to max_results.
    if (static_cast<int>(hits.size()) > max_results) {
        hits.resize(static_cast<std::size_t>(max_results));
    }

    // -------------------------------------------------------------------------
    // 5. Format output.
    // -------------------------------------------------------------------------
    std::ostringstream body;
    Json payload = Json::array();

    if (hits.empty()) {
        body << "ToolSearch: no tools matched \"" << query << "\".\n"
             << "The registry contains " << all_schemas.size() << " tool(s).\n"
             << "Try a shorter query or use 'select:ToolName' to retrieve a specific tool.";

        return ToolResult::ok(body.str(),
                              Json{{"matches",     Json::array()},
                                   {"total_tools", static_cast<int>(all_schemas.size())},
                                   {"query",       query}});
    }

    body << "Found " << hits.size() << " tool(s) matching \"" << query << "\":\n\n";

    for (std::size_t i = 0; i < hits.size(); ++i) {
        const Hit& h = hits[i];
        body << "[" << (i + 1) << "] " << h.tool_name
             << " (score: " << h.score << ")\n"
             << "Description: " << h.tool_description << "\n"
             << "Schema:\n"
             << pretty(h.tool_schema) << "\n\n";

        payload.push_back(Json{
            {"name",        h.tool_name},
            {"score",       h.score},
            {"description", h.tool_description},
            {"schema",      h.tool_schema}
        });
    }

    return ToolResult::ok(body.str(),
                          Json{{"matches",     std::move(payload)},
                               {"query",       query},
                               {"total_tools", static_cast<int>(all_schemas.size())}});
}

} // namespace batbox::tools
