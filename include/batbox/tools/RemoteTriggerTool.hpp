// include/batbox/tools/RemoteTriggerTool.hpp
//
// batbox::tools::RemoteTriggerTool — POST a JSON payload to a configured
// webhook endpoint and return the response body as a ToolResult.
//
// Security model:
//   Users MUST explicitly add the target URL to the
//   settings.json `remote_trigger.allowed_urls` glob list before the tool
//   will fire.  Any URL that does not match at least one entry in the list
//   is rejected with ToolResult::error.  This prevents the model from
//   exfiltrating data or triggering arbitrary HTTP endpoints.
//
// Args schema (tool_call):
//   url     : string   — the target URL (must match an allowed_urls glob)
//   payload : any      — JSON value to POST as application/json body
//   headers : object?  — optional map<string,string> of extra request headers;
//                        merged on top of the defaults set in the constructor
//
// Config injection (constructor):
//   allowed_urls   : list of glob patterns from settings.json
//   default_headers: map<string,string> of headers always included (e.g. auth)
//   timeout_ms     : request timeout in milliseconds (default 30 000)
//
// Response handling:
//   HTTP 2xx → ToolResult::ok(response_body)
//   HTTP non-2xx → ToolResult::error("HTTP <N>: <first 512 bytes of body>")
//   Network error → ToolResult::error("network error: <cpr error message>")
//   Cancellation → ToolResult::error("cancelled")
//
// Blueprint contract: batbox::tools::RemoteTriggerTool
//   (blueprints table: task CPP 5.22, class row)

#pragma once

#include <batbox/tools/ITool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>
#include <batbox/core/Json.hpp>

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace batbox::tools {

// =============================================================================
// RemoteTriggerTool
// =============================================================================

class RemoteTriggerTool final : public ITool {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    /// Construct a RemoteTriggerTool.
    ///
    /// @param allowed_urls    Glob patterns from settings.json
    ///                        `remote_trigger.allowed_urls`.  At least one
    ///                        pattern must match the requested URL or the tool
    ///                        returns an error.  An empty list blocks every URL.
    ///
    /// @param default_headers HTTP headers always included in every request,
    ///                        e.g. {"Authorization": "Bearer <token>"}.
    ///                        Per-call `headers` arg entries override these.
    ///
    /// @param timeout_ms      Request timeout in milliseconds.  Defaults to
    ///                        30 000 (30 seconds).  Values ≤ 0 are replaced
    ///                        with the default.
    explicit RemoteTriggerTool(
        std::vector<std::string>       allowed_urls    = {},
        std::map<std::string,std::string> default_headers = {},
        int                            timeout_ms      = 30'000);

    // -------------------------------------------------------------------------
    // ITool interface
    // -------------------------------------------------------------------------

    [[nodiscard]] std::string_view name()        const override;
    [[nodiscard]] std::string_view description() const override;
    [[nodiscard]] Json             schema_json() const override;

    /// Execute the RemoteTrigger: validate URL, build request, POST, return body.
    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override;

    /// Not read-only — a POST to a remote endpoint is a side-effecting operation.
    [[nodiscard]] bool is_read_only()          const override { return false; }

    /// Requires confirmation (network egress to a configured webhook).
    [[nodiscard]] bool requires_confirmation() const override { return true;  }

private:
    // -------------------------------------------------------------------------
    // Implementation helpers
    // -------------------------------------------------------------------------

    /// Return true if `url` matches at least one pattern in allowed_urls_.
    [[nodiscard]] bool url_is_allowed(const std::string& url) const noexcept;

    // -------------------------------------------------------------------------
    // State (set once at construction, then read-only)
    // -------------------------------------------------------------------------
    std::vector<std::string>          allowed_urls_;
    std::map<std::string,std::string> default_headers_;
    int                               timeout_ms_;
};

} // namespace batbox::tools
