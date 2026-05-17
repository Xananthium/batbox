// src/tools/RemoteTriggerTool.cpp
//
// batbox::tools::RemoteTriggerTool — implementation.
//
// POST a JSON payload to a user-configured webhook endpoint.
// URL is validated against the allowed_urls glob list before any network I/O.

#include <batbox/tools/RemoteTriggerTool.hpp>
#include <batbox/permissions/PatternMatcher.hpp>

#include <cpr/cpr.h>

#include <algorithm>
#include <sstream>
#include <string>
#include <string_view>

namespace batbox::tools {

// =============================================================================
// Constants
// =============================================================================

static constexpr int    kDefaultTimeoutMs     = 30'000;
static constexpr size_t kErrorBodyExcerptBytes = 512;

// =============================================================================
// Construction
// =============================================================================

RemoteTriggerTool::RemoteTriggerTool(
        std::vector<std::string>          allowed_urls,
        std::map<std::string,std::string> default_headers,
        int                               timeout_ms)
    : allowed_urls_(std::move(allowed_urls))
    , default_headers_(std::move(default_headers))
    , timeout_ms_(timeout_ms > 0 ? timeout_ms : kDefaultTimeoutMs)
{}

// =============================================================================
// ITool interface
// =============================================================================

std::string_view RemoteTriggerTool::name() const {
    return "RemoteTrigger";
}

std::string_view RemoteTriggerTool::description() const {
    return "POST a JSON payload to a user-configured webhook URL and return the response body.";
}

Json RemoteTriggerTool::schema_json() const {
    return Json{
        {"name",        "RemoteTrigger"},
        {"description", "POST a JSON payload to a user-configured webhook URL and return the response body."},
        {"parameters", Json{
            {"type",       "object"},
            {"properties", Json{
                {"url", Json{
                    {"type",        "string"},
                    {"description", "Target webhook URL; must match an entry in settings.json remote_trigger.allowed_urls."}
                }},
                {"payload", Json{
                    {"description", "JSON value sent as the request body (application/json)."}
                }},
                {"headers", Json{
                    {"type",                 "object"},
                    {"description",          "Optional extra HTTP headers merged on top of configured defaults."},
                    {"additionalProperties", Json{{"type","string"}}}
                }}
            }},
            {"required", Json::array({"url", "payload"})}
        }}
    };
}

// =============================================================================
// run()
// =============================================================================

ToolResult RemoteTriggerTool::run(const Json& args, ToolContext& ctx) {
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
        return ToolResult::error("RemoteTrigger: missing or non-string 'url' argument");
    }
    const std::string url = args.at("url").get<std::string>();

    if (url.empty()) {
        return ToolResult::error("RemoteTrigger: 'url' must not be empty");
    }

    // ------------------------------------------------------------------
    // 2. Extract required argument: payload — serialize to JSON string.
    // ------------------------------------------------------------------
    if (!args.contains("payload")) {
        return ToolResult::error("RemoteTrigger: missing 'payload' argument");
    }
    std::string body_str;
    try {
        body_str = args.at("payload").dump();
    } catch (const std::exception& ex) {
        return ToolResult::error(std::string("RemoteTrigger: failed to serialize payload: ") + ex.what());
    }

    // ------------------------------------------------------------------
    // 3. URL allowlist check — reject anything not explicitly allowed.
    // ------------------------------------------------------------------
    if (!url_is_allowed(url)) {
        return ToolResult::error(
            "RemoteTrigger: URL not in allowed list. "
            "Add a glob pattern to settings.json remote_trigger.allowed_urls.");
    }

    // ------------------------------------------------------------------
    // 4. Build merged headers: default_headers_ < per-call headers arg.
    // ------------------------------------------------------------------
    cpr::Header req_headers;

    // Start with configured defaults (e.g. auth token).
    for (const auto& [k, v] : default_headers_) {
        req_headers[k] = v;
    }

    // Always set Content-Type; per-call headers can override.
    req_headers["Content-Type"] = "application/json";

    // Overlay per-call headers if provided.
    if (args.contains("headers") && args.at("headers").is_object()) {
        for (const auto& [k, v] : args.at("headers").items()) {
            if (v.is_string()) {
                req_headers[k] = v.get<std::string>();
            }
        }
    }

    // ------------------------------------------------------------------
    // 5. Cancellation check before network I/O.
    // ------------------------------------------------------------------
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    // ------------------------------------------------------------------
    // 6. Issue the POST via cpr.
    // ------------------------------------------------------------------
    cpr::Response resp;
    try {
        resp = cpr::Post(
            cpr::Url{url},
            cpr::Body{body_str},
            std::move(req_headers),
            cpr::Timeout{timeout_ms_}
        );
    } catch (const std::exception& ex) {
        return ToolResult::error(std::string("RemoteTrigger: exception during POST: ") + ex.what());
    }

    // ------------------------------------------------------------------
    // 7. Cancellation check after network I/O returns.
    // ------------------------------------------------------------------
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    // ------------------------------------------------------------------
    // 8. Handle network-level errors (DNS failure, timeout, TLS error…).
    // ------------------------------------------------------------------
    if (resp.error.code != cpr::ErrorCode::OK) {
        return ToolResult::error(
            "RemoteTrigger: network error: " + resp.error.message);
    }

    // ------------------------------------------------------------------
    // 9. Handle HTTP-level errors (non-2xx status).
    // ------------------------------------------------------------------
    if (resp.status_code < 200 || resp.status_code >= 300) {
        // Include up to kErrorBodyExcerptBytes of the response body in the
        // error message so the model can diagnose the failure.
        std::string excerpt = resp.text.substr(
            0, std::min(resp.text.size(), kErrorBodyExcerptBytes));

        std::ostringstream oss;
        oss << "RemoteTrigger: HTTP " << resp.status_code << ": " << excerpt;
        if (resp.text.size() > kErrorBodyExcerptBytes) {
            oss << "... (" << resp.text.size() << " bytes total)";
        }
        return ToolResult::error(oss.str());
    }

    // ------------------------------------------------------------------
    // 10. Success — return the full response body.
    // ------------------------------------------------------------------
    return ToolResult::ok(resp.text);
}

// =============================================================================
// Private helpers
// =============================================================================

bool RemoteTriggerTool::url_is_allowed(const std::string& url) const noexcept {
    for (const auto& pattern : allowed_urls_) {
        if (permissions::glob_match(pattern, url)) {
            return true;
        }
    }
    return false;
}

} // namespace batbox::tools
