#pragma once

// ---------------------------------------------------------------------------
// include/batbox/sidecar/ScraplingClient.hpp
//
// HTTP client to the Python Scrapling sidecar FastAPI server.
//
// Responsibilities:
//   - POST JSON bodies to 127.0.0.1:<port>/<endpoint>
//   - Honour the BATBOX_WEBFETCH_TIMEOUT_SEC configuration
//   - Support cooperative cancellation via CancelToken (maps to
//     cpr's SetCancellationParam atomic-bool mechanism)
//   - Translate HTTP-level and sidecar-level errors into Result<T, std::string>
//
// Endpoint map:
//   fetch(FetchRequest)   → POST /fetch   → FetchResponse
//   search(SearchRequest) → POST /search  → SearchResponse
//   select(SelectRequest) → POST /select  → SelectResponse
//   healthz()             → GET  /healthz → bool
//   shutdown()            → POST /shutdown (best-effort, no error returned)
//
// Cancellation:
//   CancelToken is a move-only handle backed by std::stop_token.
//   The client bridges it to cpr via a shared_ptr<std::atomic_bool> that is
//   set to true when the token fires.  An on_cancel callback is registered
//   so that token cancellation propagates into the in-flight cpr request.
//
// Error categories returned as Err(std::string):
//   - cpr transport error  (cpr::Error::code != 0)
//   - HTTP 4xx / 5xx       ("sidecar error <status>: <ErrorResponse.detail>")
//   - JSON parse failure   ("json parse error: <what>")
//   - Cancellation         ("cancelled")
//
// Namespace: batbox::sidecar
// ---------------------------------------------------------------------------

#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/sidecar/ScraplingProto.hpp>

#include <cstdint>
#include <string>

namespace batbox::sidecar {

// ---------------------------------------------------------------------------
// ScraplingClient
//
// Owns a base URL derived from port, and a default timeout in milliseconds.
// Every public method is synchronous and blocks the calling thread for the
// duration of the HTTP round-trip (or until cancellation / timeout fires).
//
// Thread-safety: each method call creates its own cpr::Session internally;
// the client object itself carries no mutable per-request state, so multiple
// threads MAY call different methods concurrently.  Concurrent calls sharing
// the SAME CancelToken are safe — the on_cancel callback is thread-safe.
// ---------------------------------------------------------------------------
class ScraplingClient {
public:
    // -----------------------------------------------------------------------
    // Constructor
    //
    // port        — TCP port the sidecar is listening on (e.g. 31337)
    // timeout_sec — per-request timeout in seconds; honours
    //               BATBOX_WEBFETCH_TIMEOUT_SEC when callers pass the config
    //               value here.  Default is 30 s to match the proto defaults.
    // -----------------------------------------------------------------------
    explicit ScraplingClient(uint16_t port, int timeout_sec = 30);

    // Non-copyable; the base URL string is cheap to copy but keeping the
    // class move-only signals that it owns a logical connection context.
    ScraplingClient(const ScraplingClient&) = delete;
    ScraplingClient& operator=(const ScraplingClient&) = delete;
    ScraplingClient(ScraplingClient&&) noexcept = default;
    ScraplingClient& operator=(ScraplingClient&&) noexcept = default;

    ~ScraplingClient() = default;

    // -----------------------------------------------------------------------
    // fetch — POST /fetch
    //
    // Sends req serialised as JSON and deserialises the response body into a
    // FetchResponse.  Returns Err when the transport fails, the server returns
    // a non-200 status, the body is not valid JSON, or ct is cancelled.
    // -----------------------------------------------------------------------
    [[nodiscard]] Result<proto::FetchResponse, std::string>
    fetch(const proto::FetchRequest& req, CancelToken ct);

    // -----------------------------------------------------------------------
    // search — POST /search
    //
    // Same contract as fetch() but for the /search endpoint.
    // -----------------------------------------------------------------------
    [[nodiscard]] Result<proto::SearchResponse, std::string>
    search(const proto::SearchRequest& req, CancelToken ct);

    // -----------------------------------------------------------------------
    // select — POST /select
    //
    // Same contract as fetch() but for the /select endpoint.
    // -----------------------------------------------------------------------
    [[nodiscard]] Result<proto::SelectResponse, std::string>
    select(const proto::SelectRequest& req, CancelToken ct);

    // -----------------------------------------------------------------------
    // healthz — GET /healthz
    //
    // Returns true when the sidecar responds with HTTP 200 within 500 ms.
    // Returns false on any error (transport failure, non-200, timeout).
    // No CancelToken parameter — the 500 ms connect+read timeout is the
    // implied cancellation boundary for health-checks.
    // -----------------------------------------------------------------------
    [[nodiscard]] bool healthz();

    // -----------------------------------------------------------------------
    // shutdown — POST /shutdown
    //
    // Sends a best-effort shutdown request to the sidecar.  Errors are
    // silently ignored because the process is expected to terminate shortly
    // after; callers should not depend on this call succeeding.
    // -----------------------------------------------------------------------
    void shutdown();

private:
    std::string base_url_;   // "http://127.0.0.1:<port>"
    int         timeout_ms_; // per-request timeout in milliseconds

    // -----------------------------------------------------------------------
    // post_json — shared implementation for fetch / search / select
    //
    // Serialises `body` to a compact JSON string, POSTs it to
    // base_url_ + endpoint, then returns the raw response body string on
    // success or an Err string on any failure.
    //
    // The CancelToken is bridged to cpr via a shared_ptr<std::atomic_bool>;
    // when ct fires, the bool is set to true and cpr aborts the in-flight
    // transfer.
    // -----------------------------------------------------------------------
    [[nodiscard]] Result<std::string, std::string>
    post_json(const std::string& endpoint,
              const batbox::Json& body,
              CancelToken         ct);
};

} // namespace batbox::sidecar
