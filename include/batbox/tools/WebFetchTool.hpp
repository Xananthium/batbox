// include/batbox/tools/WebFetchTool.hpp
//
// batbox::tools::WebFetchTool — fetch a web page via the Scrapling sidecar
// and return its Markdown content as a ToolResult.
//
// Tool name : "WebFetch"
// is_read_only()          : true  (fetching a URL has no local side-effects;
//                                   allowed in Plan mode like Read)
// requires_confirmation() : false (network read, no mutation)
//
// Arguments (JSON args):
//   url          (string, required)  — HTTP/HTTPS URL to fetch.
//                                      Must match at least one pattern in the
//                                      ToolContext::allowed_tools URL allow-list
//                                      when the allow-list is present; if it
//                                      doesn't, the tool returns is_error=true.
//   css_selector (string, optional)  — If provided, POST to the Scrapling
//                                      /select endpoint instead of /fetch and
//                                      return the matched text fragments as a
//                                      newline-joined string.
//   prompt       (string, optional)  — Informational hint for the model; stored
//                                      in the structured payload but not sent to
//                                      the sidecar.
//
// Configuration (injected at construction):
//   sidecar          — SidecarManager& used to send requests; must outlive
//                      this tool.
//   webfetch_timeout_sec — per-request timeout; read from
//                          Config::search.webfetch_timeout_sec.
//   webfetch_max_bytes   — byte cap applied after the response arrives; read
//                          from Config::search.webfetch_max_bytes.
//                          When the returned Markdown exceeds this cap the
//                          body is truncated and a notice appended.
//
// Response handling:
//   sidecar is_error=true  → ToolResult::error("<sidecar error_message>")
//   transport/sidecar Err  → ToolResult::error("<Err message>")
//   sidecar disabled       → ToolResult::error("WebFetch: sidecar is disabled …")
//   truncated              → body += "\n\n[Note: response truncated at <N> bytes]"
//   success                → ToolResult::ok(<markdown>)
//
// Blueprint contract: batbox::tools::WebFetchTool
//   (blueprints table rows 16656–16658)

#pragma once

#include <batbox/sidecar/SidecarManager.hpp>
#include <batbox/tools/ITool.hpp>

#include <string_view>

namespace batbox::tools {

// =============================================================================
// WebFetchTool
// =============================================================================

class WebFetchTool final : public ITool {
public:
    // -------------------------------------------------------------------------
    // Construction
    //
    // sidecar              — reference to the shared SidecarManager; must
    //                        outlive this tool.
    // webfetch_timeout_sec — per-request timeout in seconds.  Typically
    //                        Config::search.webfetch_timeout_sec.
    // webfetch_max_bytes   — byte cap on the returned Markdown body.  Bodies
    //                        larger than this are truncated with a notice.
    //                        Typically Config::search.webfetch_max_bytes.
    // -------------------------------------------------------------------------
    explicit WebFetchTool(batbox::sidecar::SidecarManager& sidecar,
                          int webfetch_timeout_sec = 30,
                          int webfetch_max_bytes   = 5'242'880);

    // -------------------------------------------------------------------------
    // ITool interface
    // -------------------------------------------------------------------------

    /// Tool name: "WebFetch".
    [[nodiscard]] std::string_view name() const override;

    /// One-sentence description for the model's tool schema.
    [[nodiscard]] std::string_view description() const override;

    /// OpenAI tools[*].function JSON object for this tool.
    [[nodiscard]] Json schema_json() const override;

    /// Execute the web fetch.
    ///
    /// args must contain:
    ///   "url"          — string (required)
    ///   "css_selector" — string (optional); routes to /select instead of /fetch
    ///   "prompt"       — string (optional); stored in structured_payload only
    ///
    /// Calls SidecarManager::ensure_started, then either:
    ///   - POST /fetch  (no css_selector) → FetchResponse.markdown
    ///   - POST /select (css_selector)    → SelectResponse.matches joined by "\n"
    ///
    /// Truncates to webfetch_max_bytes_ if the body exceeds the cap.
    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override;

    /// Returns true — fetching a URL does not mutate local state.
    [[nodiscard]] bool is_read_only() const override { return true; }

    /// Returns false — no confirmation prompt needed for a read-only fetch.
    [[nodiscard]] bool requires_confirmation() const override { return false; }

private:
    // -------------------------------------------------------------------------
    // State (set once at construction, then read-only)
    // -------------------------------------------------------------------------
    batbox::sidecar::SidecarManager& sidecar_;
    int                               webfetch_timeout_sec_;
    int                               webfetch_max_bytes_;

    // -------------------------------------------------------------------------
    // truncate_if_needed(body)
    //
    // If body.size() > webfetch_max_bytes_, truncates body to webfetch_max_bytes_
    // and appends a notice.  Returns the (possibly modified) string.
    // -------------------------------------------------------------------------
    [[nodiscard]] std::string truncate_if_needed(std::string body) const;
};

} // namespace batbox::tools
