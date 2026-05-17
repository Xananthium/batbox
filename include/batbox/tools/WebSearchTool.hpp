// include/batbox/tools/WebSearchTool.hpp
// =============================================================================
// batbox::tools::WebSearchTool — ITool that performs a web search via the
// Python Scrapling sidecar's POST /search endpoint.
//
// Contract (blueprints table, task CPP 5.13):
//
//   Tool name       : "WebSearch"
//   is_read_only()  : true  (search is a read-only operation)
//   requires_confirmation() : false  (no side-effects; never prompts)
//
//   JSON args:
//     query           (string, required)  — search query text
//     n               (integer, optional) — max results to return; default 10,
//                                           clamped to [1, 50]
//     engine          (string, optional)  — override the default engine:
//                                           "ddg" or "searxng"
//     allowed_domains (array of strings, optional)  — only include results
//                                           whose URL is under one of these
//                                           domains (post-filter; e.g. ["github.com"])
//     blocked_domains (array of strings, optional)  — exclude results
//                                           whose URL is under any of these
//                                           domains (post-filter applied after
//                                           allowed_domains)
//
//   Engine selection precedence:
//     1. args["engine"] — explicit override per call
//     2. Config::search.engine — from BATBOX_SEARCH_ENGINE env var
//     Default (when config is Ddg): engine = "ddg"
//
//   SearXNG URL:
//     When engine resolves to "searxng", Config::search.searxng_url is sent
//     as SearchRequest::searxng_url to the sidecar.
//
//   Returns (body — numbered list, model-friendly):
//     "1. <title>\n   URL: <url>\n   <snippet>\n\n2. ..." 
//     OR
//     "No results found for: <query>"  (when filtered/empty)
//     OR  ToolResult::error("<message>") on sidecar failure.
//
//   Structured payload:
//     {
//       "query":   "<query>",
//       "engine":  "<engine>",
//       "n":       <count>,
//       "results": [{"title":"…","url":"…","snippet":"…"}, …]
//     }
//
// Thread safety:
//   WebSearchTool holds a non-owning reference to SidecarManager.  The sidecar
//   manager must outlive the tool.  Concurrent calls to run() are safe because
//   SidecarManager::request() is thread-safe once Running.
//
// Blueprint contract: batbox::tools::WebSearchTool (task CPP 5.13)
// =============================================================================

#pragma once

#include <batbox/config/Config.hpp>
#include <batbox/sidecar/SidecarManager.hpp>
#include <batbox/tools/ITool.hpp>

namespace batbox::tools {

// =============================================================================
// WebSearchTool
// =============================================================================

class WebSearchTool final : public ITool {
public:
    // -------------------------------------------------------------------------
    // Construction
    //
    // cfg     — reference to the live Config (read search.engine and
    //           search.searxng_url at call-time so hot-reload is respected).
    // sidecar — reference to the SidecarManager that owns the scrapling subprocess.
    //           Both references must outlive this tool instance.
    // -------------------------------------------------------------------------
    explicit WebSearchTool(const batbox::config::Config& cfg,
                           batbox::sidecar::SidecarManager& sidecar);

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

    /// Web search is read-only — it may run in Plan mode.
    [[nodiscard]] bool is_read_only() const override { return true; }

    /// No side-effects; never requires a confirmation prompt.
    [[nodiscard]] bool requires_confirmation() const override { return false; }

private:
    const batbox::config::Config&    cfg_;     ///< Live config reference.
    batbox::sidecar::SidecarManager& sidecar_; ///< Sidecar subprocess manager.
};

} // namespace batbox::tools
