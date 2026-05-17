// src/inference/Client.cpp
// ---------------------------------------------------------------------------
// batbox::inference::Client implementation.
//
// chat() — non-streaming POST /v1/chat/completions (stream=false).
// stream_chat() — streaming POST /v1/chat/completions (stream=true, SSE).
//
// Streaming POST shape (OpenAI-compatible):
//   Each SSE event:
//     data: {"id":"...","choices":[{"index":0,"delta":{...},"finish_reason":null|"stop"}]}
//   Final content event carries "finish_reason":"stop".
//   Final usage event (when stream_options.include_usage=true):
//     data: {"choices":[{"index":0,"delta":{},"finish_reason":"stop"}],"usage":{...}}
//   Terminator:
//     data: [DONE]
//
// Retry policy for stream_chat (before first token only):
//   HTTP 429 or 5xx → retry up to 3 times with jittered exponential backoff.
//   Backoff base values: 250ms, 1s, 4s (each ±20% jitter).
//   After ANY token has been delivered, no retry — surface immediately.
//
// Cancellation:
//   CancelToken ct is checked in the cpr ProgressCallback; returning false
//   from that callback causes libcurl to abort the transfer.
//
// Provider quirk handling (CPP 4.7):
//   BATBOX_PROVIDER_HINT (config.api.provider_hint):
//     openai     — default OpenAI semantics; no transforms.
//     vllm       — strip stream_options from request body.
//     together   — standard OpenAI compat; no transforms.
//     ollama     — strip stream_options; strip Authorization header.
//     anthropic  — standard OpenAI compat (LiteLLM proxy); no transforms.
//     groq       — standard OpenAI compat; no transforms.
//     mistral    — standard OpenAI compat; no transforms.
//     lm-studio  — standard OpenAI compat; no transforms.
//     llama-cpp  — strip stream_options from request body.
//     auto       — detect provider from base_url.
//   Unknown values: log warning, fall back to openai semantics.
// ---------------------------------------------------------------------------

#include <batbox/inference/Client.hpp>

#include <batbox/core/Json.hpp>
#include <batbox/core/Logging.hpp>
#include <batbox/inference/ChatRequest.hpp>
#include <batbox/inference/ChatResponse.hpp>
#include <batbox/inference/SseParser.hpp>

#include <cpr/cpr.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <random>
#include <string>
#include <thread>

namespace batbox::inference {

// ---------------------------------------------------------------------------
// Internal helpers (file-local)
// ---------------------------------------------------------------------------
namespace {

/// Apply ±20% uniform jitter to a base duration.
std::chrono::milliseconds jitter(std::chrono::milliseconds base) {
    // Use a function-local static rng; mt19937 is thread-safe to construct but
    // not to use concurrently — Client instances are per-thread so this is safe.
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_real_distribution<double> dist(0.8, 1.2);
    const double factor = dist(rng);
    return std::chrono::milliseconds(
        static_cast<long long>(static_cast<double>(base.count()) * factor));
}

/// Backoff delays for up to 3 retry attempts (before jitter).
constexpr std::chrono::milliseconds kRetryBackoff[3] = {
    std::chrono::milliseconds{250},
    std::chrono::milliseconds{1000},
    std::chrono::milliseconds{4000},
};

/// Returns true if the HTTP status warrants a retry (before first token).
bool is_retriable_status(long status) noexcept {
    return status == 429 || (status >= 500 && status < 600);
}

// ---------------------------------------------------------------------------
// Provider quirk handling
// ---------------------------------------------------------------------------

/// Resolved provider values (post-normalization and auto-detection).
/// These are the canonical string literals used throughout this file.
///   "openai"    — default OpenAI semantics
///   "vllm"      — vLLM (strip stream_options)
///   "together"  — Together AI (standard compat)
///   "ollama"    — Ollama (strip stream_options + Authorization)
///   "anthropic" — Anthropic via LiteLLM (standard compat)
///   "groq"      — Groq (standard compat)
///   "mistral"   — Mistral AI (standard compat)
///   "lm-studio" — LM Studio (standard compat)
///   "llama-cpp" — llama.cpp server (strip stream_options)

/// Lowercase-fold a string in-place.
std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// Detect provider from base_url when hint is "auto" or empty.
/// Priority order: most-specific domains first, local-port heuristics last.
/// Falls back to "openai" when no pattern matches.
std::string detect_provider_from_url(const std::string& base_url) {
    const std::string url = to_lower(base_url);
    if (url.find("together.ai") != std::string::npos
            || url.find("together.xyz") != std::string::npos) {
        return "together";
    }
    if (url.find("groq.com") != std::string::npos) {
        return "groq";
    }
    if (url.find("mistral.ai") != std::string::npos) {
        return "mistral";
    }
    if (url.find("anthropic.com") != std::string::npos
            || url.find("litellm") != std::string::npos) {
        return "anthropic";
    }
    if (url.find("11434") != std::string::npos
            || url.find("ollama") != std::string::npos) {
        return "ollama";
    }
    if (url.find("lmstudio") != std::string::npos
            || url.find(":1234/") != std::string::npos
            || url.find(":1234") == url.size() - 5) {
        return "lm-studio";
    }
    if (url.find("llama") != std::string::npos) {
        return "llama-cpp";
    }
    if (url.find("vllm") != std::string::npos) {
        return "vllm";
    }
    return "openai";
}

/// Resolve the provider hint string to a canonical lowercase provider name.
///
/// Rules:
///   empty or "auto" → detect from base_url via detect_provider_from_url()
///   known value      → return normalised (lowercased) value
///   unknown value    → log warning, return "openai"
///
/// @param hint     The raw provider_hint value from config (may be empty).
/// @param base_url The API base URL (used for auto-detection only).
/// @returns        One of: openai | vllm | together | ollama | anthropic |
///                         groq | mistral | lm-studio | llama-cpp
std::string resolve_provider_hint(const std::string& hint,
                                  const std::string& base_url) {
    const std::string norm = to_lower(hint);

    if (norm.empty() || norm == "auto") {
        return detect_provider_from_url(base_url);
    }

    // Validate against the known set.
    static const std::string kKnown[] = {
        "openai", "vllm", "together", "ollama",
        "anthropic", "groq", "mistral", "lm-studio", "llama-cpp"
    };
    for (const auto& known : kKnown) {
        if (norm == known) {
            return norm;
        }
    }

    // Unknown value — warn and fall back.
    auto lg = batbox::log::get("inference.client");
    lg->warn(
        "BATBOX_PROVIDER_HINT='{}' is not a recognised provider; "
        "falling back to openai semantics. "
        "Valid values: openai|vllm|together|ollama|anthropic|groq|mistral|lm-studio|llama-cpp|auto",
        hint);
    return "openai";
}

/// Apply provider-specific pre-request transformations to the JSON body and
/// HTTP headers.
///
/// Transforms applied per provider:
///   vllm       — erase "stream_options" from body (vLLM rejects unknown fields)
///   ollama     — erase "stream_options" from body; erase "Authorization" header
///   llama-cpp  — erase "stream_options" from body (not supported by llama.cpp server)
///   all others — no transforms (standard OpenAI-compatible behaviour)
///
/// @param provider  Resolved provider string (output of resolve_provider_hint).
/// @param body      Mutable JSON object representing the request body.
/// @param headers   Mutable cpr::Header map (key=value string pairs).
void apply_provider_quirks(const std::string& provider,
                           batbox::Json&       body,
                           cpr::Header&        headers) {
    if (provider == "vllm") {
        // vLLM does not recognise stream_options; sending it causes a 422.
        body.erase("stream_options");
    } else if (provider == "ollama") {
        // Ollama runs locally without auth; strip stream_options (not supported).
        body.erase("stream_options");
        headers.erase("Authorization");
    } else if (provider == "llama-cpp") {
        // llama.cpp server does not support stream_options.include_usage.
        body.erase("stream_options");
    }
    // together, groq, mistral, anthropic, lm-studio, openai: no transforms.
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Client::Client(const batbox::config::Config& cfg)
    : cfg_(cfg) {}

// ---------------------------------------------------------------------------
// completions_url()
// ---------------------------------------------------------------------------

std::string Client::completions_url() const {
    std::string url = cfg_.api.base_url;
    // Trim trailing slash to avoid double slashes.
    if (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    url += "/chat/completions";
    return url;
}

// ---------------------------------------------------------------------------
// chat() — non-streaming POST
// ---------------------------------------------------------------------------

Result<ChatResponse> Client::chat(const ChatRequest& req) {
    // Serialise the request; force stream=false for the non-streaming path.
    ChatRequest wire_req = req;
    wire_req.stream = false;
    wire_req.stream_options_include_usage = std::nullopt;  // not used without streaming

    batbox::Json body_json = wire_req;

    // --- Provider quirk pre-request transforms ---
    const std::string provider = resolve_provider_hint(
        cfg_.api.provider_hint, cfg_.api.base_url);

    cpr::Header headers{
        {"Content-Type",  "application/json"},
        {"Authorization", "Bearer " + cfg_.api.api_key}
    };
    apply_provider_quirks(provider, body_json, headers);

    const std::string body_str = body_json.dump();
    const std::string url = completions_url();

    // Timeout: config value is in seconds; cpr::Timeout takes milliseconds.
    const std::int32_t timeout_ms =
        static_cast<std::int32_t>(cfg_.api.request_timeout_sec) * 1000;

    // Fire the request.
    cpr::Response http = cpr::Post(
        cpr::Url{url},
        headers,
        cpr::Body{body_str},
        cpr::Timeout{timeout_ms}
    );

    // Transport-level error (DNS, TLS, timeout, connection refused).
    if (http.error) {
        return batbox::Err("transport: " + http.error.message);
    }

    // HTTP-level error.
    if (http.status_code < 200 || http.status_code >= 300) {
        const std::string excerpt =
            http.text.size() > 200 ? http.text.substr(0, 200) : http.text;
        return batbox::Err(
            "http " + std::to_string(http.status_code) + ": " + excerpt);
    }

    // Parse the response body.
    auto parse_result = batbox::parse(http.text);
    if (!parse_result) {
        return batbox::Err("parse: " + parse_result.error());
    }
    const batbox::Json& root = parse_result.value();

    // Flatten the OpenAI response envelope into the ChatResponse flat shape
    // expected by from_json(const Json&, ChatResponse&).
    try {
        const auto& choices = root.at("choices");
        if (!choices.is_array() || choices.empty()) {
            return batbox::Err("parse: choices array missing or empty");
        }
        const auto& choice = choices[0];
        const auto& message = choice.at("message");

        batbox::Json flat = batbox::Json::object();
        flat["id"]    = root.value("id",    std::string{});
        flat["model"] = root.value("model", std::string{});

        // finish_reason lives at choices[0].finish_reason
        flat["finish_reason"] = choice.value("finish_reason", std::string{"stop"});

        // content: may be null (when tool_calls present)
        if (message.contains("content") && message["content"].is_string()) {
            flat["content"] = message["content"];
        } else {
            flat["content"] = nullptr;
        }

        // tool_calls: present when finish_reason == "tool_calls"
        if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
            flat["tool_calls"] = message["tool_calls"];
        }

        // usage at top level
        if (root.contains("usage") && root["usage"].is_object()) {
            flat["usage"] = root["usage"];
        }

        ChatResponse resp = flat.get<ChatResponse>();
        return resp;

    } catch (const std::exception& e) {
        return batbox::Err(std::string{"parse: "} + e.what());
    }
}

// ---------------------------------------------------------------------------
// stream_chat() — SSE streaming POST
// ---------------------------------------------------------------------------

Result<UsageDelta> Client::stream_chat(
    const ChatRequest&                           req,
    std::function<void(const StreamDelta&)>      on_delta,
    CancelToken                                  ct)
{
    auto lg = batbox::log::get("inference.client");

    // Serialise the request; force stream=true and request usage in final chunk.
    ChatRequest wire_req = req;
    wire_req.stream = true;
    wire_req.stream_options_include_usage = true;

    batbox::Json body_json = wire_req;

    // --- Provider quirk pre-request transforms ---
    // Resolve once; applied on every retry attempt via the mutable body_json
    // and a fresh headers copy inside the retry loop.
    const std::string provider = resolve_provider_hint(
        cfg_.api.provider_hint, cfg_.api.base_url);

    const std::string url = completions_url();

    // Timeout: config value is in seconds; cpr::Timeout takes milliseconds.
    const std::int32_t timeout_ms =
        static_cast<std::int32_t>(cfg_.api.request_timeout_sec) * 1000;

    // Retry loop — max 3 retries for 429/5xx before first token.
    constexpr int kMaxRetries = 3;

    for (int attempt = 0; attempt <= kMaxRetries; ++attempt) {

        // --- State captured per attempt ---

        // Whether at least one delta has been delivered to on_delta.
        bool token_received = false;

        // Whether the server has started streaming any delta of any kind
        // (content, reasoning_content, or tool_calls).  Used as the retry gate
        // so that reasoning models (which emit reasoning_content before content)
        // are not retried mid-stream.  Kept separate from token_received which
        // is content-only and used for telemetry.
        bool stream_started = false;

        // Whether the server delivered a finish_reason string ("stop",
        // "length", "tool_calls", "content_filter") in any chunk.  A non-null
        // finish_reason means the model explicitly told us it finished — even
        // if it produced no content (e.g. finish_reason=length after burning
        // all tokens on reasoning_content).  We treat that as a valid (if
        // empty) completion.  Used by the empty-stream guard below.
        bool final_finish_reason_seen = false;

        // Accumulated usage from the final streaming chunk.
        UsageDelta final_usage{};

        // Error message from inside the WriteCallback (surfaced after Post()).
        std::string stream_error;

        // SSE parser — fresh on each retry attempt.
        SseParser sse_parser;

        // Build the body string and headers for this attempt.
        // apply_provider_quirks operates on a copy of the JSON so that
        // original body_json is unchanged for subsequent retry attempts.
        batbox::Json attempt_body = body_json;
        cpr::Header attempt_headers{
            {"Content-Type",  "application/json"},
            {"Authorization", "Bearer " + cfg_.api.api_key}
        };
        apply_provider_quirks(provider, attempt_body, attempt_headers);
        const std::string body_str = attempt_body.dump();

        // --- cpr WriteCallback ---
        // Receives raw bytes from libcurl as they arrive.  Feeds them to the
        // SseParser and dispatches complete events.
        //
        // Returns false to abort the transfer on SSE parse error.  The
        // status_code check below ensures we don't process body bytes when the
        // server returned an error status.

        auto write_cb = [&](std::string_view data, intptr_t /*userdata*/) -> bool {
            auto parse_result = sse_parser.feed(data);
            if (!parse_result) {
                stream_error = "sse: " + parse_result.error();
                lg->error("stream_chat: SseParser error: {}", parse_result.error());
                return false; // abort curl
            }

            for (const SseEvent& ev : parse_result.value()) {
                // TUI-T17b: trace-level log per SSE event for diagnosis.
                lg->trace("sse: event={} data_bytes={} is_done={}",
                          ev.event.empty() ? "(default)" : ev.event,
                          ev.data.size(),
                          ev.is_done);

                if (ev.is_done) {
                    // [DONE] sentinel — stream is cleanly complete.
                    break;
                }

                // TUI-T17: Surface SSE error events.
                // LM Studio and other OpenAI-compat servers signal request
                // rejection mid-stream (e.g. context-length overflow) by
                // emitting an SSE event of the form:
                //     event: error
                //     data: {"error":{"message":"...","type":"...","code":...}}
                // over HTTP 200, then closing the socket without [DONE].
                // Without this branch the event would be parsed as a normal
                // chunk, fail the choices/usage check, and be silently
                // skipped — leaving T12's generic guard to fire with a
                // misleading "try increasing BATBOX_MAX_TOKENS" suggestion.
                if (ev.event == "error" || ev.event == "fatal_error") {
                    std::string server_msg;
                    auto parsed = batbox::parse(ev.data);
                    if (parsed && parsed->is_object()) {
                        const auto& obj = parsed.value();
                        if (obj.contains("error") && obj["error"].is_object()
                                && obj["error"].contains("message")
                                && obj["error"]["message"].is_string()) {
                            server_msg = obj["error"]["message"].get<std::string>();
                        } else if (obj.contains("message")
                                && obj["message"].is_string()) {
                            server_msg = obj["message"].get<std::string>();
                        }
                    }
                    if (server_msg.empty()) {
                        server_msg = ev.data; // fall back to raw payload
                    }
                    stream_error = "server: " + server_msg;
                    lg->error("stream_chat: server emitted SSE error event: {}",
                              server_msg);
                    return false; // abort curl; stream_error surfaced as Err at ~line 560
                }

                // Non-empty event: field that is not "error" / "fatal_error" —
                // likely a "ping" keepalive or other non-data event.  Skip silently.
                if (!ev.event.empty()) {
                    continue;
                }

                // Parse the SSE event data as an OpenAI streaming chunk.
                // Shape: {"id":"...", "choices":[{"index":0,"delta":{...},"finish_reason":null}], "usage":{...}}
                try {
                    auto chunk_result = batbox::parse(ev.data);
                    if (!chunk_result) {
                        // Malformed chunk JSON — log and skip (don't abort; be lenient).
                        lg->warn("stream_chat: malformed chunk JSON: {}", ev.data);
                        continue;
                    }
                    const batbox::Json& chunk = chunk_result.value();

                    // TUI-T17: Detect inline server errors — some servers emit
                    // no event: name but embed the error in the data: JSON's
                    // top-level "error" field (e.g. HTTP 200 + data: {"error":{...}}).
                    if (chunk.contains("error") && chunk["error"].is_object()) {
                        std::string server_msg;
                        if (chunk["error"].contains("message")
                                && chunk["error"]["message"].is_string()) {
                            server_msg =
                                chunk["error"]["message"].get<std::string>();
                        } else {
                            server_msg = chunk["error"].dump();
                        }
                        stream_error = "server: " + server_msg;
                        lg->error("stream_chat: server error in stream data: {}",
                                  server_msg);
                        return false; // abort curl
                    }

                    // Extract choices[0].delta → StreamDelta.
                    if (!chunk.contains("choices") || !chunk["choices"].is_array()
                            || chunk["choices"].empty()) {
                        // Some providers emit a usage-only chunk with no choices;
                        // handle top-level usage then continue.
                        if (chunk.contains("usage") && chunk["usage"].is_object()) {
                            final_usage = chunk["usage"].get<UsageDelta>();
                        }
                        continue;
                    }

                    const batbox::Json& choice = chunk["choices"][0];
                    const batbox::Json& delta_obj =
                        choice.contains("delta") ? choice["delta"]
                                                 : batbox::Json::object();

                    // Build StreamDelta by parsing the delta object directly.
                    StreamDelta delta = delta_obj.get<StreamDelta>();

                    // finish_reason lives at choices[0], not inside delta.
                    // Overwrite whatever from_json put there (it won't find it).
                    if (choice.contains("finish_reason")
                            && choice["finish_reason"].is_string()) {
                        delta.finish_reason = choice["finish_reason"].get<std::string>();
                        // Any non-null finish_reason means the model signalled
                        // completion.  Record it for the empty-stream guard.
                        final_finish_reason_seen = true;
                    }
                    // Ollama quirk: missing finish_reason is fine — leave nullopt.

                    // Top-level usage (final chunk, stream_options.include_usage=true).
                    if (chunk.contains("usage") && chunk["usage"].is_object()) {
                        final_usage = chunk["usage"].get<UsageDelta>();
                        delta.usage = final_usage;
                    }

                    // Track whether any content has been received (telemetry).
                    if (delta.content.has_value() && !delta.content->empty()) {
                        token_received = true;
                    }
                    if (delta.tool_calls.has_value() && !delta.tool_calls->empty()) {
                        token_received = true;
                    }

                    // Track whether the server has started sending ANY delta
                    // (including reasoning_content from reasoning models).
                    // This is the retry gate: once the server has started
                    // streaming we do not retry on error.
                    if (!stream_started) {
                        if ((delta.content.has_value()          && !delta.content->empty())
                         || (delta.reasoning_content.has_value() && !delta.reasoning_content->empty())
                         || (delta.tool_calls.has_value()        && !delta.tool_calls->empty())) {
                            stream_started = true;
                        }
                    }

                    // Deliver the delta to the caller.
                    on_delta(delta);

                } catch (const std::exception& e) {
                    // Parsing exception for this chunk — log and skip.
                    lg->warn("stream_chat: chunk parse exception: {}", e.what());
                    continue;
                }
            }

            return true; // continue transfer
        };

        // --- cpr ProgressCallback ---
        // Called periodically by libcurl (at least once per network event).
        // Return false to abort the transfer when the CancelToken fires.
        //
        // CancelToken ct is captured by reference; it lives for the full
        // duration of stream_chat().

        auto progress_cb = [&ct](cpr::cpr_pf_arg_t /*dltotal*/,
                                  cpr::cpr_pf_arg_t /*dlnow*/,
                                  cpr::cpr_pf_arg_t /*ultotal*/,
                                  cpr::cpr_pf_arg_t /*ulnow*/,
                                  intptr_t         /*userdata*/) -> bool {
            // Return false to abort when cancelled.
            return !ct.stop_requested();
        };

        // --- Fire the streaming POST ---

        cpr::Session session;
        session.SetUrl(cpr::Url{url});
        session.SetHeader(attempt_headers);
        session.SetBody(cpr::Body{body_str});
        session.SetTimeout(cpr::Timeout{timeout_ms});
        session.SetWriteCallback(cpr::WriteCallback{write_cb});
        session.SetProgressCallback(cpr::ProgressCallback{progress_cb});

        // B2: Idle-stream timeout via CURLOPT_LOW_SPEED_LIMIT + CURLOPT_LOW_SPEED_TIME.
        //
        // cfg_.api.stream_idle_timeout_sec is resolved ONCE at Config::load() time
        // from BATBOX_STREAM_IDLE_TIMEOUT_SEC (default 60).  Setting limit=1 byte/sec
        // with time=N means: abort the transfer if fewer than 1 byte/sec is received
        // for N consecutive seconds.  This catches a stalled upstream mid-stream
        // (not just connection establishment failures which SetTimeout handles).
        //
        // When stream_idle_timeout_sec == 0, the feature is disabled (no LowSpeed set).
        if (cfg_.api.stream_idle_timeout_sec > 0) {
            session.SetLowSpeed(cpr::LowSpeed{
                /*limit=*/1,
                /*time=*/std::chrono::seconds(cfg_.api.stream_idle_timeout_sec)
            });
        }

        cpr::Response http = session.Post();

        // --- Check for cancellation ---
        if (ct.stop_requested()) {
            return batbox::Err(std::string{"cancelled"});
        }

        // --- Check for SSE parse error surfaced from WriteCallback ---
        // This check comes BEFORE the transport-error check because returning
        // false from write_cb causes libcurl to set http.error with
        // "Failure writing output to destination" — an intentional abort that
        // masks the real server-supplied message stored in stream_error.
        if (!stream_error.empty()) {
            return batbox::Err(stream_error);
        }

        // --- Check for transport errors ---
        if (http.error) {
            // If cancelled via ProgressCallback, libcurl sets an ABORTED_BY_CALLBACK error.
            if (ct.stop_requested()) {
                return batbox::Err(std::string{"cancelled"});
            }
            return batbox::Err("transport: " + http.error.message);
        }

        // --- Check HTTP status ---
        if (http.status_code < 200 || http.status_code >= 300) {
            const std::string excerpt =
                http.text.size() > 200 ? http.text.substr(0, 200) : http.text;
            const std::string err_msg =
                "http " + std::to_string(http.status_code) + ": " + excerpt;

            // Retry only if the server has not yet started streaming and the
            // status is retriable.  Using stream_started (not token_received)
            // so that reasoning models — which emit reasoning_content before
            // any content — are not retried after the reasoning phase begins.
            if (!stream_started && is_retriable_status(http.status_code)
                    && attempt < kMaxRetries) {
                const auto delay = jitter(kRetryBackoff[attempt]);
                lg->warn(
                    "stream_chat: attempt {}/{} failed with {} — retrying in {}ms",
                    attempt + 1, kMaxRetries,
                    http.status_code,
                    delay.count());
                std::this_thread::sleep_for(delay);
                continue; // next attempt
            }

            return batbox::Err(err_msg);
        }

        // --- Empty-stream guard ---
        // The server returned 2xx and curl reported a clean transfer, but we
        // never received a content token, a tool_call fragment, or a
        // finish_reason chunk.  This happens when LM Studio (and similar
        // OpenAI-compat servers) close the SSE socket without a [DONE]
        // sentinel after emitting only reasoning_content chunks, OR when an
        // upstream timeout cuts the stream mid-reasoning.  Returning Ok here
        // causes the caller to build an empty assistant message — invisible
        // to the user as a silent success (--print exits 0 with no output;
        // TUI shows the user's message but no reply).
        //
        // Distinction:
        //   token_received=true  OR  final_finish_reason_seen=true
        //       → model either delivered content or explicitly signalled done
        //       → return Ok(usage)  [even if content is empty, e.g. length truncation]
        //   token_received=false AND final_finish_reason_seen=false
        //       → server closed connection without any content or finish signal
        //       → return Err so the caller surfaces it; --print exits non-zero
        if (!token_received && !final_finish_reason_seen) {
            lg->warn("stream_chat: 2xx response but no content or finish_reason "
                     "delivered — server may have truncated mid-reasoning");
            return batbox::Err(std::string{
                "stream ended without content (server may have truncated "
                "during reasoning phase — try increasing BATBOX_MAX_TOKENS "
                "or the model's context length)"
            });
        }

        // --- Stream completed successfully ---
        return final_usage;

    } // end retry loop

    // Should be unreachable — the loop always returns.
    return batbox::Err(std::string{"stream_chat: retry loop exhausted"});
}

} // namespace batbox::inference
