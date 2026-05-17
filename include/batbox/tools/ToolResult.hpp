// include/batbox/tools/ToolResult.hpp
//
// batbox::tools::ToolResult — result type returned by every ITool::run() call.
//
// A ToolResult carries:
//   body             — UTF-8 string surfaced to the model as the tool message
//                      content.  May be plain text, a compact JSON dump, a
//                      unified diff, or an error description.
//   is_error         — when true the model receives a tool message flagged as
//                      an error so it can self-correct.
//   structured_payload — optional parsed JSON for callers that want richer
//                      structured data beyond the text body (e.g. diff cards,
//                      agent panels).  Never sent directly to the inference
//                      layer; TUI and orchestrators may inspect it.
//
// Construction helpers:
//   ToolResult::ok(body)              — is_error = false, no payload
//   ToolResult::ok(body, payload)     — is_error = false, structured payload
//   ToolResult::error(body)           — is_error = true,  no payload
//   ToolResult::error(body, payload)  — is_error = true,  structured payload
//
// Blueprint contract: batbox::tools::ToolResult (blueprints table row 16618)

#pragma once

#include <batbox/core/Json.hpp>

#include <optional>
#include <string>
#include <utility>

namespace batbox::tools {

// =============================================================================
// ToolResult
// =============================================================================

struct ToolResult {
    // -------------------------------------------------------------------------
    // Data members — exactly as locked in the blueprint contract.
    // -------------------------------------------------------------------------

    /// Text body fed to the model as the content of the tool message.
    /// Required; never empty on a well-formed result (though technically valid).
    std::string body;

    /// True when the tool encountered an error.  The inference layer wraps this
    /// result in a tool message with the "is_error" flag so the model can
    /// self-correct rather than silently accepting incorrect output.
    bool is_error = false;

    /// Optional structured payload for consumers beyond the inference layer
    /// (TUI diff cards, sub-agent panels, etc.).  Not transmitted to the model.
    std::optional<Json> structured_payload;

    // -------------------------------------------------------------------------
    // Constructors
    // -------------------------------------------------------------------------

    /// Default-constructs an empty, non-error result.
    ToolResult() = default;

    /// Construct directly from all three fields (aggregate-style).
    ToolResult(std::string body_, bool is_error_, std::optional<Json> payload_)
        : body(std::move(body_))
        , is_error(is_error_)
        , structured_payload(std::move(payload_)) {}

    // -------------------------------------------------------------------------
    // Named construction helpers — preferred over raw constructors at call-sites.
    // -------------------------------------------------------------------------

    /// Produce a successful result with text body and no structured payload.
    [[nodiscard]] static ToolResult ok(std::string body) {
        return ToolResult{std::move(body), false, std::nullopt};
    }

    /// Produce a successful result with text body and a structured payload.
    [[nodiscard]] static ToolResult ok(std::string body, Json payload) {
        return ToolResult{std::move(body), false, std::move(payload)};
    }

    /// Produce an error result with a descriptive text body.
    [[nodiscard]] static ToolResult error(std::string body) {
        return ToolResult{std::move(body), true, std::nullopt};
    }

    /// Produce an error result with a descriptive text body and diagnostic payload.
    [[nodiscard]] static ToolResult error(std::string body, Json payload) {
        return ToolResult{std::move(body), true, std::move(payload)};
    }

    // -------------------------------------------------------------------------
    // Equality (used in tests)
    // -------------------------------------------------------------------------
    [[nodiscard]] friend bool operator==(const ToolResult& a,
                                         const ToolResult& b) noexcept {
        return a.body == b.body
            && a.is_error == b.is_error
            && a.structured_payload == b.structured_payload;
    }
    [[nodiscard]] friend bool operator!=(const ToolResult& a,
                                         const ToolResult& b) noexcept {
        return !(a == b);
    }
};

} // namespace batbox::tools
