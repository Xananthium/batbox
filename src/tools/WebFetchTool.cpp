// src/tools/WebFetchTool.cpp
//
// batbox::tools::WebFetchTool — implementation.
//
// Routes through SidecarManager:
//   no css_selector → POST /fetch   → FetchResponse.markdown
//   css_selector    → POST /select  → SelectResponse.matches joined by "\n"
//
// Error paths (all surface as ToolResult::error):
//   - Missing / invalid "url" argument
//   - Sidecar disabled / failed to start
//   - Transport / HTTP error from ScraplingClient
//   - Sidecar-level is_error in FetchResponse / SelectResponse
//
// Truncation:
//   When the assembled body exceeds webfetch_max_bytes_, it is truncated and
//   a notice appended: "\n\n[Note: response truncated at <N> bytes]"
//
// Blueprint contract: batbox::tools::WebFetchTool (blueprints rows 16656–16658)

#include <batbox/tools/WebFetchTool.hpp>

#include <batbox/sidecar/ScraplingProto.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>

#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace batbox::tools {

// =============================================================================
// Construction
// =============================================================================

WebFetchTool::WebFetchTool(batbox::sidecar::SidecarManager& sidecar,
                           int webfetch_timeout_sec,
                           int webfetch_max_bytes)
    : sidecar_(sidecar)
    , webfetch_timeout_sec_(webfetch_timeout_sec > 0 ? webfetch_timeout_sec : 30)
    , webfetch_max_bytes_(webfetch_max_bytes > 0 ? webfetch_max_bytes : 5'242'880)
{}

// =============================================================================
// ITool identity
// =============================================================================

std::string_view WebFetchTool::name() const {
    return "WebFetch";
}

std::string_view WebFetchTool::description() const {
    return "Fetch a web page via the Scrapling sidecar and return its Markdown content; "
           "optionally extract specific elements with a CSS selector.";
}

Json WebFetchTool::schema_json() const {
    return Json{
        {"name",        "WebFetch"},
        {"description",
         "Fetch a web page via the Scrapling sidecar and return its Markdown content; "
         "optionally extract specific elements with a CSS selector."},
        {"parameters", Json{
            {"type",       "object"},
            {"properties", Json{
                {"url", Json{
                    {"type",        "string"},
                    {"description",
                     "HTTP/HTTPS URL to fetch."}
                }},
                {"css_selector", Json{
                    {"type",        "string"},
                    {"description",
                     "Optional CSS selector.  When provided, the sidecar extracts "
                     "matching elements and returns their text content joined by newlines "
                     "instead of the full-page Markdown."}
                }},
                {"prompt", Json{
                    {"type",        "string"},
                    {"description",
                     "Optional hint describing what you are looking for; stored in the "
                     "structured payload for context but not sent to the sidecar."}
                }}
            }},
            {"required", Json::array({"url"})}
        }}
    };
}

// =============================================================================
// run()
// =============================================================================

ToolResult WebFetchTool::run(const Json& args, ToolContext& ctx) {
    // ------------------------------------------------------------------
    // 0. Cancellation check before any work.
    // ------------------------------------------------------------------
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    // ------------------------------------------------------------------
    // 1. Extract and validate required argument: url.
    // ------------------------------------------------------------------
    if (!args.contains("url") || !args.at("url").is_string()) {
        return ToolResult::error("WebFetch: missing or non-string 'url' argument");
    }
    const std::string url = args.at("url").get<std::string>();

    if (url.empty()) {
        return ToolResult::error("WebFetch: 'url' must not be empty");
    }

    // ------------------------------------------------------------------
    // 2. Extract optional css_selector argument.
    // ------------------------------------------------------------------
    std::string css_selector;
    bool has_selector = false;
    if (args.contains("css_selector") && args.at("css_selector").is_string()) {
        css_selector = args.at("css_selector").get<std::string>();
        if (!css_selector.empty()) {
            has_selector = true;
        }
    }

    // ------------------------------------------------------------------
    // 3. Extract optional prompt (informational only; not sent to sidecar).
    // ------------------------------------------------------------------
    std::string prompt_hint;
    if (args.contains("prompt") && args.at("prompt").is_string()) {
        prompt_hint = args.at("prompt").get<std::string>();
    }

    // ------------------------------------------------------------------
    // 4. Cancellation check before network I/O.
    // ------------------------------------------------------------------
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    // ------------------------------------------------------------------
    // 5. Dispatch through SidecarManager.
    //
    // Two paths:
    //   a) css_selector present → POST /select  (SelectRequest / SelectResponse)
    //   b) no css_selector      → POST /fetch   (FetchRequest / FetchResponse)
    //
    // SidecarManager::request<Req,Resp> calls ensure_started internally.
    // ------------------------------------------------------------------
    if (has_selector) {
        // ------------------------------------------------------------------
        // Path A: CSS selector → /select
        // ------------------------------------------------------------------
        batbox::sidecar::proto::SelectRequest req;
        req.url      = url;
        req.selector = css_selector;
        req.timeout  = static_cast<double>(webfetch_timeout_sec_);

        auto [child_src, child_tok] = ctx.cancel_token.child();
        (void)child_src;

        auto result = sidecar_.request<batbox::sidecar::proto::SelectRequest,
                                       batbox::sidecar::proto::SelectResponse>(
            "/select", req, std::move(child_tok));

        // ------------------------------------------------------------------
        // 6a. Cancellation check after I/O.
        // ------------------------------------------------------------------
        if (ctx.is_cancelled()) {
            return ToolResult::error("cancelled");
        }

        if (!result.has_value()) {
            return ToolResult::error("WebFetch: sidecar error: " + result.error());
        }

        const auto& resp = result.value();

        if (resp.is_error) {
            return ToolResult::error("WebFetch: " + resp.error_message);
        }

        // Join matches with newlines.
        std::ostringstream body_stream;
        for (std::size_t i = 0; i < resp.matches.size(); ++i) {
            if (i > 0) body_stream << '\n';
            body_stream << resp.matches[i];
        }
        std::string body = body_stream.str();
        body = truncate_if_needed(std::move(body));

        Json payload = Json{
            {"url",      url},
            {"selector", css_selector},
            {"count",    resp.count}
        };
        if (!prompt_hint.empty()) {
            payload["prompt"] = prompt_hint;
        }

        return ToolResult::ok(std::move(body), std::move(payload));

    } else {
        // ------------------------------------------------------------------
        // Path B: Full page fetch → /fetch
        // ------------------------------------------------------------------
        batbox::sidecar::proto::FetchRequest req;
        req.url       = url;
        req.timeout   = static_cast<double>(webfetch_timeout_sec_);
        req.max_bytes = webfetch_max_bytes_;

        auto [child_src, child_tok] = ctx.cancel_token.child();
        (void)child_src;

        auto result = sidecar_.request<batbox::sidecar::proto::FetchRequest,
                                       batbox::sidecar::proto::FetchResponse>(
            "/fetch", req, std::move(child_tok));

        // ------------------------------------------------------------------
        // 6b. Cancellation check after I/O.
        // ------------------------------------------------------------------
        if (ctx.is_cancelled()) {
            return ToolResult::error("cancelled");
        }

        if (!result.has_value()) {
            return ToolResult::error("WebFetch: sidecar error: " + result.error());
        }

        const auto& resp = result.value();

        if (resp.is_error) {
            return ToolResult::error("WebFetch: " + resp.error_message);
        }

        std::string body = resp.markdown;
        body = truncate_if_needed(std::move(body));

        Json payload = Json{
            {"url",            url},
            {"status_code",    resp.status_code},
            {"content_type",   resp.content_type},
            {"content_length", resp.content_length},
            {"truncated",      resp.truncated}
        };
        if (!prompt_hint.empty()) {
            payload["prompt"] = prompt_hint;
        }

        return ToolResult::ok(std::move(body), std::move(payload));
    }
}

// =============================================================================
// Private helpers
// =============================================================================

std::string WebFetchTool::truncate_if_needed(std::string body) const {
    if (static_cast<int>(body.size()) <= webfetch_max_bytes_) {
        return body;
    }
    body.resize(static_cast<std::size_t>(webfetch_max_bytes_));
    body += "\n\n[Note: response truncated at ";
    body += std::to_string(webfetch_max_bytes_);
    body += " bytes]";
    return body;
}

} // namespace batbox::tools
