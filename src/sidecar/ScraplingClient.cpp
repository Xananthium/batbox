// ---------------------------------------------------------------------------
// src/sidecar/ScraplingClient.cpp
//
// Implementation of ScraplingClient — HTTP POST helpers to the Python
// Scrapling sidecar FastAPI server running on 127.0.0.1:<port>.
//
// Design notes:
//
//   Cancellation bridge:
//     CancelToken wraps std::stop_token (C++20 / batbox).  cpr uses a
//     shared_ptr<std::atomic_bool> (SetCancellationParam).  We allocate one
//     atomic_bool per request, register an on_cancel callback on the token
//     that sets the bool to true, and pass the shared_ptr to cpr.  When the
//     token fires (from any thread), cpr aborts the transfer on the next
//     CURLOPT_PROGRESSFUNCTION tick.
//
//   Error classification:
//     1. Pre-flight cancellation — token already set before Post() — returns
//        Err("cancelled") without touching the network.
//     2. cpr transport error     — cpr::ErrorCode != OK → Err(error message).
//     3. HTTP 4xx / 5xx          — try to parse ErrorResponse; fall back to
//        raw status code string.
//     4. JSON parse failure      — Err("json parse error: <what>").
//     5. Post-flight cancellation — status 0 with error ABORTED_BY_CALLBACK
//        → Err("cancelled").
//
//   Timeout:
//     A single cpr::Timeout covers the complete connect + transfer window.
//     healthz() uses a hard-coded 500 ms; all other methods use timeout_ms_
//     which is set in the constructor from the caller-supplied timeout_sec.
// ---------------------------------------------------------------------------

#include "batbox/sidecar/ScraplingClient.hpp"

#include <batbox/core/Json.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/sidecar/ScraplingProto.hpp>

#include <cpr/cpr.h>

#include <atomic>
#include <memory>
#include <string>

namespace batbox::sidecar {

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

ScraplingClient::ScraplingClient(uint16_t port, int timeout_sec)
    : base_url_("http://127.0.0.1:" + std::to_string(port))
    , timeout_ms_(timeout_sec * 1000)
{
}

// ---------------------------------------------------------------------------
// post_json — shared POST helper
// ---------------------------------------------------------------------------

Result<std::string, std::string>
ScraplingClient::post_json(const std::string& endpoint,
                           const batbox::Json& body,
                           CancelToken         ct)
{
    // Pre-flight cancellation check.
    if (ct.is_cancelled()) {
        return Err(std::string("cancelled"));
    }

    const std::string url      = base_url_ + endpoint;
    const std::string body_str = batbox::dump(body);

    // Allocate the cancellation flag shared between the on_cancel callback
    // and the cpr session.
    auto cancel_flag = std::make_shared<std::atomic_bool>(false);

    // Register an on_cancel callback so that when the CancelToken fires the
    // cpr in-flight transfer is aborted on its next progress tick.
    // The returned handle keeps the registration alive for the duration of
    // this call; it is destroyed (deregistering the callback) when we return.
    auto cancel_handle = ct.on_cancel([cancel_flag]() noexcept {
        cancel_flag->store(true, std::memory_order_relaxed);
    });

    cpr::Session session;
    session.SetUrl(cpr::Url{url});
    session.SetHeader(cpr::Header{
        {"Content-Type", "application/json"},
        {"Accept",       "application/json"},
        // Suppress Expect: 100-continue — the sidecar is a local loopback
        // server; 100-continue negotiation adds unnecessary latency and can
        // cause mock-server test failures when the server sends the final
        // response before curl sends the body.
        {"Expect", ""},
    });
    session.SetBody(cpr::Body{body_str});
    session.SetTimeout(cpr::Timeout{timeout_ms_});
    session.SetCancellationParam(cancel_flag);

    cpr::Response resp = session.Post();

    // Transport-level error (network unreachable, connection refused, etc.).
    if (resp.error.code != cpr::ErrorCode::OK) {
        // Distinguish an abort triggered by our cancel_flag from a genuine
        // transport failure so the caller gets a clean "cancelled" error.
        if (resp.error.code == cpr::ErrorCode::ABORTED_BY_CALLBACK ||
            cancel_flag->load(std::memory_order_relaxed)) {
            return Err(std::string("cancelled"));
        }
        return Err(resp.error.message);
    }

    // HTTP-level error (sidecar returned 4xx or 5xx).
    if (resp.status_code < 200 || resp.status_code >= 300) {
        // Attempt to parse the sidecar's structured ErrorResponse.
        auto parsed = batbox::parse(resp.text);
        if (parsed.has_value()) {
            try {
                auto err = proto::ErrorResponse::from_json(parsed.value());
                const std::string detail = err.detail.empty() ? err.error : err.detail;
                return Err("sidecar error " +
                           std::to_string(resp.status_code) + ": " + detail);
            } catch (...) {
                // ErrorResponse parse failed — fall through to raw status.
            }
        }
        return Err("sidecar error " + std::to_string(resp.status_code) +
                   ": " + resp.text);
    }

    return resp.text;
}

// ---------------------------------------------------------------------------
// fetch — POST /fetch
// ---------------------------------------------------------------------------

Result<proto::FetchResponse, std::string>
ScraplingClient::fetch(const proto::FetchRequest& req, CancelToken ct)
{
    auto raw = post_json("/fetch", req.to_json(), std::move(ct));
    if (!raw.has_value()) {
        return Err(raw.error());
    }

    auto parsed = batbox::parse(raw.value());
    if (!parsed.has_value()) {
        return Err("json parse error: " + parsed.error());
    }

    try {
        return proto::FetchResponse::from_json(parsed.value());
    } catch (const std::exception& e) {
        return Err(std::string("json parse error: ") + e.what());
    }
}

// ---------------------------------------------------------------------------
// search — POST /search
// ---------------------------------------------------------------------------

Result<proto::SearchResponse, std::string>
ScraplingClient::search(const proto::SearchRequest& req, CancelToken ct)
{
    auto raw = post_json("/search", req.to_json(), std::move(ct));
    if (!raw.has_value()) {
        return Err(raw.error());
    }

    auto parsed = batbox::parse(raw.value());
    if (!parsed.has_value()) {
        return Err("json parse error: " + parsed.error());
    }

    try {
        return proto::SearchResponse::from_json(parsed.value());
    } catch (const std::exception& e) {
        return Err(std::string("json parse error: ") + e.what());
    }
}

// ---------------------------------------------------------------------------
// select — POST /select
// ---------------------------------------------------------------------------

Result<proto::SelectResponse, std::string>
ScraplingClient::select(const proto::SelectRequest& req, CancelToken ct)
{
    auto raw = post_json("/select", req.to_json(), std::move(ct));
    if (!raw.has_value()) {
        return Err(raw.error());
    }

    auto parsed = batbox::parse(raw.value());
    if (!parsed.has_value()) {
        return Err("json parse error: " + parsed.error());
    }

    try {
        return proto::SelectResponse::from_json(parsed.value());
    } catch (const std::exception& e) {
        return Err(std::string("json parse error: ") + e.what());
    }
}

// ---------------------------------------------------------------------------
// healthz — GET /healthz  (500 ms hard timeout, no cancellation)
// ---------------------------------------------------------------------------

bool ScraplingClient::healthz()
{
    constexpr int kHealthzTimeoutMs = 500;

    cpr::Response resp = cpr::Get(
        cpr::Url{base_url_ + "/healthz"},
        cpr::Timeout{kHealthzTimeoutMs}
    );

    return (resp.error.code == cpr::ErrorCode::OK &&
            resp.status_code == 200);
}

// ---------------------------------------------------------------------------
// shutdown — POST /shutdown  (best-effort, errors silently ignored)
// ---------------------------------------------------------------------------

void ScraplingClient::shutdown()
{
    // Use a short timeout: if the sidecar is already gone we don't want to
    // block the calling thread waiting for a connection that will never arrive.
    constexpr int kShutdownTimeoutMs = 2000;

    try {
        cpr::Post(
            cpr::Url{base_url_ + "/shutdown"},
            cpr::Header{
                {"Content-Type", "application/json"},
                {"Accept",       "application/json"},
            },
            cpr::Body{"{}"},
            cpr::Timeout{kShutdownTimeoutMs}
        );
    } catch (...) {
        // Intentionally swallowed — the process may have already exited.
    }
}

} // namespace batbox::sidecar
