// src/tools/WebSearchTool.cpp
// =============================================================================
// Implementation of batbox::tools::WebSearchTool.
//
// Routes web search requests through the Python Scrapling sidecar's
// POST /search endpoint.  Engine selection follows Config::search.engine
// unless the caller overrides via args["engine"].  When engine resolves to
// "searxng", Config::search.searxng_url is forwarded to the sidecar.
//
// Post-filters (allowed_domains / blocked_domains) are applied in C++ after
// the sidecar returns results, so domain filtering never round-trips over
// the IPC boundary.
//
// Output format: a numbered list suitable for model consumption.
//
// Blueprint contract: batbox::tools::WebSearchTool (task CPP 5.13)
// =============================================================================

#include <batbox/tools/WebSearchTool.hpp>

#include <batbox/config/Config.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/sidecar/ScraplingProto.hpp>
#include <batbox/sidecar/SidecarManager.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>

#include <algorithm>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace batbox::tools {

namespace {

// ---------------------------------------------------------------------------
// domain_matches — returns true when `url` is under `domain`.
//
// A URL "is under" a domain when the URL's host equals the domain or ends
// with "." + domain (subdomain match).
// Comparison is case-insensitive; scheme and path are ignored.
//
// Examples:
//   domain_matches("https://github.com/foo",     "github.com")   → true
//   domain_matches("https://api.github.com/bar", "github.com")   → true
//   domain_matches("https://notgithub.com",      "github.com")   → false
// ---------------------------------------------------------------------------
bool domain_matches(std::string_view url, std::string_view domain) {
    // Strip scheme.
    auto pos = url.find("://");
    if (pos != std::string_view::npos) {
        url.remove_prefix(pos + 3);
    }
    // Strip everything after the first '/' or '?' or '#'.
    for (char sep : {'/', '?', '#'}) {
        auto sep_pos = url.find(sep);
        if (sep_pos != std::string_view::npos) {
            url = url.substr(0, sep_pos);
        }
    }
    // Strip port.
    auto colon = url.rfind(':');
    if (colon != std::string_view::npos) {
        url = url.substr(0, colon);
    }
    // Lowercase both for comparison.
    std::string host(url);
    std::string dom(domain);
    for (auto& c : host) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto& c : dom)  c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    return host == dom ||
           (host.size() > dom.size() + 1 &&
            host.substr(host.size() - dom.size() - 1) == "." + dom);
}

// ---------------------------------------------------------------------------
// url_passes_filters — returns true when `url` satisfies both domain filters.
//
// Filtering logic:
//   1. If allowed_domains is non-empty: the URL must match at least one.
//   2. If blocked_domains is non-empty: the URL must NOT match any.
// Both filters are applied; a URL must pass both to be included.
// ---------------------------------------------------------------------------
bool url_passes_filters(const std::string& url,
                        const std::vector<std::string>& allowed_domains,
                        const std::vector<std::string>& blocked_domains) {
    // Allowed-domains gate.
    if (!allowed_domains.empty()) {
        bool in_allowed = false;
        for (const auto& d : allowed_domains) {
            if (domain_matches(url, d)) {
                in_allowed = true;
                break;
            }
        }
        if (!in_allowed) return false;
    }
    // Blocked-domains gate.
    for (const auto& d : blocked_domains) {
        if (domain_matches(url, d)) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// format_results — formats the filtered results as a numbered list.
// ---------------------------------------------------------------------------
std::string format_results(
        const std::vector<batbox::sidecar::proto::SearchResult>& results,
        const std::string& query) {
    if (results.empty()) {
        return "No results found for: " + query;
    }
    std::ostringstream out;
    std::size_t idx = 1;
    for (const auto& r : results) {
        out << idx << ". " << r.title << "\n";
        out << "   URL: " << r.url << "\n";
        if (!r.snippet.empty()) {
            out << "   " << r.snippet << "\n";
        }
        out << "\n";
        ++idx;
    }
    // Trim trailing newline.
    std::string s = out.str();
    while (!s.empty() && s.back() == '\n') {
        s.pop_back();
    }
    return s;
}

// ---------------------------------------------------------------------------
// extract_string_array — extracts a JSON string array from args[key].
// Returns an empty vector (and leaves `error` empty) if the key is absent or
// null.  Writes an error message and returns empty if the value is present
// but malformed.
// ---------------------------------------------------------------------------
std::vector<std::string> extract_string_array(const Json& args,
                                               std::string_view key,
                                               std::string& error) {
    std::vector<std::string> out;
    if (!args.contains(key) || args[key].is_null()) {
        return out;
    }
    const auto& val = args[key];
    if (!val.is_array()) {
        error = std::string("WebSearch: '") + std::string(key) + "' must be an array of strings.";
        return out;
    }
    for (const auto& elem : val) {
        if (!elem.is_string()) {
            error = std::string("WebSearch: all elements of '") + std::string(key) + "' must be strings.";
            return {};
        }
        out.push_back(elem.get<std::string>());
    }
    return out;
}

} // anonymous namespace

// =============================================================================
// WebSearchTool — construction
// =============================================================================

WebSearchTool::WebSearchTool(const batbox::config::Config& cfg,
                             batbox::sidecar::SidecarManager& sidecar)
    : cfg_(cfg)
    , sidecar_(sidecar) {}

// =============================================================================
// ITool identity
// =============================================================================

std::string_view WebSearchTool::name() const {
    return "WebSearch";
}

std::string_view WebSearchTool::description() const {
    return "Search the web and return the top N results (title, URL, snippet). "
           "Routes through the local Scrapling sidecar using DuckDuckGo (default) "
           "or a user-hosted SearXNG instance.";
}

Json WebSearchTool::schema_json() const {
    return Json{
        {"name",        "WebSearch"},
        {"description", description()},
        {"parameters",  Json{
            {"type",       "object"},
            {"properties", Json{
                {"query", Json{
                    {"type",        "string"},
                    {"description", "The search query string."}
                }},
                {"n", Json{
                    {"type",        "integer"},
                    {"minimum",     1},
                    {"maximum",     50},
                    {"description", "Maximum number of results to return. "
                                    "Defaults to 10."}
                }},
                {"engine", Json{
                    {"type",        "string"},
                    {"enum",        Json::array({"ddg", "searxng"})},
                    {"description", "Override the default search engine for this call. "
                                    "\"ddg\" uses DuckDuckGo HTML scraping (no API key). "
                                    "\"searxng\" requires BATBOX_SEARXNG_URL to be set. "
                                    "Defaults to BATBOX_SEARCH_ENGINE config value."}
                }},
                {"allowed_domains", Json{
                    {"type",        "array"},
                    {"items",       Json{{"type", "string"}}},
                    {"description", "If provided, only results whose URL falls under "
                                    "one of these domains are returned. "
                                    "Example: [\"github.com\", \"docs.python.org\"]."}
                }},
                {"blocked_domains", Json{
                    {"type",        "array"},
                    {"items",       Json{{"type", "string"}}},
                    {"description", "If provided, results whose URL falls under "
                                    "any of these domains are excluded. "
                                    "Applied after allowed_domains."}
                }}
            }},
            {"required", Json::array({"query"})}
        }}
    };
}

// =============================================================================
// ITool execution
// =============================================================================

ToolResult WebSearchTool::run(const Json& args, ToolContext& ctx) {
    // ------------------------------------------------------------------
    // 0. Cancellation check — bail out fast.
    // ------------------------------------------------------------------
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    // ------------------------------------------------------------------
    // 1. Extract and validate "query".
    // ------------------------------------------------------------------
    if (!args.contains("query") || !args["query"].is_string()) {
        return ToolResult::error(
            "WebSearch: required argument 'query' is missing or not a string.");
    }
    const std::string query = args["query"].get<std::string>();
    if (query.empty()) {
        return ToolResult::error("WebSearch: 'query' must be a non-empty string.");
    }

    // ------------------------------------------------------------------
    // 2. Extract optional "n" (max results), clamped to [1, 50].
    // ------------------------------------------------------------------
    int n = 10; // default
    if (args.contains("n") && !args["n"].is_null()) {
        if (!args["n"].is_number_integer()) {
            return ToolResult::error("WebSearch: 'n' must be an integer.");
        }
        const auto v = args["n"].get<long long>();
        if (v < 1 || v > 50) {
            return ToolResult::error("WebSearch: 'n' must be between 1 and 50.");
        }
        n = static_cast<int>(v);
    }

    // ------------------------------------------------------------------
    // 3. Determine engine from args override or config.
    // ------------------------------------------------------------------
    std::string engine;
    if (args.contains("engine") && !args["engine"].is_null()) {
        if (!args["engine"].is_string()) {
            return ToolResult::error("WebSearch: 'engine' must be a string.");
        }
        engine = args["engine"].get<std::string>();
        if (engine != "ddg" && engine != "searxng") {
            return ToolResult::error(
                "WebSearch: 'engine' must be \"ddg\" or \"searxng\".");
        }
    } else {
        // Use config default.
        engine = batbox::config::Config::to_string(cfg_.search.engine);
    }

    // ------------------------------------------------------------------
    // 4. Resolve searxng_url when engine = "searxng".
    // ------------------------------------------------------------------
    std::string searxng_url;
    if (engine == "searxng") {
        searxng_url = cfg_.search.searxng_url;
        if (searxng_url.empty()) {
            return ToolResult::error(
                "WebSearch: engine is \"searxng\" but BATBOX_SEARXNG_URL is not set. "
                "Set BATBOX_SEARXNG_URL to your SearXNG instance URL.");
        }
    }

    // ------------------------------------------------------------------
    // 5. Extract optional domain filter lists.
    // ------------------------------------------------------------------
    std::string filter_error;
    const auto allowed_domains = extract_string_array(args, "allowed_domains", filter_error);
    if (!filter_error.empty()) {
        return ToolResult::error(filter_error);
    }
    const auto blocked_domains = extract_string_array(args, "blocked_domains", filter_error);
    if (!filter_error.empty()) {
        return ToolResult::error(filter_error);
    }

    // ------------------------------------------------------------------
    // 6. Build the sidecar SearchRequest.
    // ------------------------------------------------------------------
    batbox::sidecar::proto::SearchRequest req;
    req.query       = query;
    req.n           = n;
    req.engine      = engine;
    req.searxng_url = searxng_url;

    // ------------------------------------------------------------------
    // 7. Cancellation check before hitting the network.
    // ------------------------------------------------------------------
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    // ------------------------------------------------------------------
    // 8. Dispatch to the sidecar via SidecarManager::request.
    // ------------------------------------------------------------------
    auto [child_src, child_tok] = ctx.cancel_token.child();
    (void)child_src;

    auto result = sidecar_.request<batbox::sidecar::proto::SearchRequest,
                                   batbox::sidecar::proto::SearchResponse>(
        "/search", req, std::move(child_tok));

    if (!result.has_value()) {
        return ToolResult::error("WebSearch: sidecar error: " + result.error());
    }

    const auto& resp = result.value();

    // ------------------------------------------------------------------
    // 9. Check inline sidecar-level error flag.
    // ------------------------------------------------------------------
    if (resp.is_error) {
        return ToolResult::error(
            "WebSearch: search failed: " + resp.error_message);
    }

    // ------------------------------------------------------------------
    // 10. Apply domain filters.
    // ------------------------------------------------------------------
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    std::vector<batbox::sidecar::proto::SearchResult> filtered;
    filtered.reserve(resp.results.size());
    for (const auto& r : resp.results) {
        if (url_passes_filters(r.url, allowed_domains, blocked_domains)) {
            filtered.push_back(r);
        }
    }

    // ------------------------------------------------------------------
    // 11. Format results and build structured payload.
    // ------------------------------------------------------------------
    const std::string body = format_results(filtered, query);

    Json payload = Json::object();
    payload["query"]   = query;
    payload["engine"]  = resp.engine.empty() ? engine : resp.engine;
    payload["n"]       = static_cast<int>(filtered.size());

    Json results_arr = Json::array();
    for (const auto& r : filtered) {
        results_arr.push_back(Json{
            {"title",   r.title},
            {"url",     r.url},
            {"snippet", r.snippet}
        });
    }
    payload["results"] = std::move(results_arr);

    return ToolResult::ok(body, std::move(payload));
}

} // namespace batbox::tools
