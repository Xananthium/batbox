// include/batbox/inference/Client.hpp
// ---------------------------------------------------------------------------
// batbox::inference::Client — OpenAI-compatible Chat Completions HTTP client.
//
// Scope:
//   Client::chat(req)                 — non-streaming POST (CPP 4.5)
//   Client::stream_chat(req, cb, ct)  — SSE streaming POST (CPP 4.6)
//
// chat():
//   POST /v1/chat/completions with stream=false.
//   Returns Result<ChatResponse>.
//
// stream_chat():
//   POST /v1/chat/completions with stream=true.
//   Calls on_delta for every SSE chunk's delta.
//   CancelToken ct — checked in cpr ProgressCallback; aborts curl when fired.
//   Returns Result<UsageDelta> — usage from the final SSE chunk.
//   Retries on HTTP 429/5xx before the first token with jittered backoff:
//     attempt 1: 250ms ±20%, attempt 2: 1s ±20%, attempt 3: 4s ±20%.
//   After the first token is received, errors are surfaced immediately (no retry).
//
// Timeout driven by BATBOX_REQUEST_TIMEOUT_SEC (config.api.request_timeout_sec).
// Bearer auth from BATBOX_API_KEY (config.api.api_key).
// base_url from BATBOX_API_BASE_URL (config.api.base_url).
//
// Construction:
//   Client client{config};
//
// Error semantics:
//   cpr transport error  → Err("transport: <curl error message>")
//   HTTP 4xx/5xx         → Err("http <status>: <first 200 chars of body>")
//   JSON parse failure   → Err("parse: <parser message>")
//   SSE parse failure    → Err("sse: <parser message>")
//   cancelled            → Err("cancelled")
//
// Thread safety:
//   Client instances are NOT shared across threads.  Construct one per
//   thread or guard sharing with an external mutex.
// ---------------------------------------------------------------------------

#pragma once

#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/config/Config.hpp>
#include <batbox/inference/ChatRequest.hpp>
#include <batbox/inference/ChatResponse.hpp>

#include <functional>
#include <string>

namespace batbox::inference {

// ============================================================================
// Client — cpr-based OpenAI chat completions HTTP client
// ============================================================================

/// HTTP client for POST /v1/chat/completions.
///
/// Constructed with a const-ref to a batbox::config::Config; the config is
/// read at call time so hot-reloaded configs are picked up without recreating
/// the client.
class Client {
public:
    /// Construct a Client that reads API settings from @p cfg.
    explicit Client(const batbox::config::Config& cfg);

    // -------------------------------------------------------------------------
    // chat() — non-streaming POST
    // -------------------------------------------------------------------------

    /// Non-streaming chat completion.
    ///
    /// Forces stream=false in the serialised request body regardless of the
    /// value set in @p req.  Timeout is BATBOX_REQUEST_TIMEOUT_SEC (seconds),
    /// converted to milliseconds for cpr.  Authentication is Bearer
    /// BATBOX_API_KEY.
    ///
    /// On success returns a fully-populated ChatResponse.
    /// On failure returns Err with a human-readable message:
    ///   "transport: <curl message>"  — network/TLS/DNS failure
    ///   "http <code>: <body excerpt>" — server returned 4xx/5xx
    ///   "parse: <what>"              — response body was not valid JSON or
    ///                                  did not conform to the expected shape
    [[nodiscard]] Result<ChatResponse> chat(const ChatRequest& req);

    // -------------------------------------------------------------------------
    // stream_chat() — SSE streaming POST
    // -------------------------------------------------------------------------

    /// Streaming chat completion via Server-Sent Events.
    ///
    /// Forces stream=true and stream_options.include_usage=true in the
    /// serialised request body.  Fires @p on_delta once for each complete SSE
    /// chunk received.  The final chunk carries a non-null StreamDelta::usage.
    ///
    /// @param req       Request body (stream/stream_options_include_usage forced).
    /// @param on_delta  Callback invoked synchronously on the calling thread for
    ///                  each received StreamDelta.  Must not throw.
    /// @param ct        Cancellation token.  When ct.stop_requested() returns true
    ///                  the cpr ProgressCallback returns false, aborting curl.
    ///
    /// Retry policy (before first token only):
    ///   HTTP 429 or 5xx on initial connection → retry up to 3 times.
    ///   Backoff durations with ±20% jitter: 250ms, 1s, 4s.
    ///   After any token has been delivered to on_delta, errors are surfaced
    ///   immediately without retry.
    ///
    /// Provider quirk handling:
    ///   Missing finish_reason (Ollama) → tolerated, not an error.
    ///   CRLF line endings (LM Studio)  → handled transparently by SseParser.
    ///
    /// Returns:
    ///   Ok(UsageDelta)        — token usage from the final streaming chunk.
    ///   Err("transport: …")   — cpr/curl network error.
    ///   Err("http <N>: …")    — HTTP error with no retry remaining.
    ///   Err("sse: …")         — SseParser buffer overflow or malformed stream.
    ///   Err("cancelled")      — CancelToken was fired before stream completed.
    [[nodiscard]] Result<UsageDelta> stream_chat(
        const ChatRequest&                           req,
        std::function<void(const StreamDelta&)>      on_delta,
        CancelToken                                  ct);

private:
    const batbox::config::Config& cfg_;

    // Build the completions endpoint URL from cfg_.api.base_url.
    [[nodiscard]] std::string completions_url() const;
};

} // namespace batbox::inference
