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
//
// PEXT2 3.1 — single dump per turn:
//   stream_chat(const ChatRequest&, ...) applies provider quirks and serialises
//   the request body exactly ONCE before the retry loop, then delegates to
//   stream_chat_impl().
//
//   Conversation::run_turn (iteration 0) captures the preflight token-estimate
//   body string and calls stream_chat(std::string, std::string_view, ...) with
//   the pre-built body and the mutex-snapshotted api_key.  This eliminates the
//   second dump that previously occurred inside the retry loop on every attempt.
//
//   stream_chat_impl() contains the retry loop and all SSE processing; both
//   stream_chat overloads converge here.
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
#include <string_view>
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
std::string resolve_provider_hint(const std::string& hint,
                                  const std::string& base_url) {
    const std::string norm = to_lower(hint);

    if (norm.empty() || norm == "auto") {
        return detect_provider_from_url(base_url);
    }

    static const std::string kKnown[] = {
        "openai", "vllm", "together", "ollama",
        "anthropic", "groq", "mistral", "lm-studio", "llama-cpp"
    };
    for (const auto& known : kKnown) {
        if (norm == known) {
            return norm;
        }
    }

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
void apply_provider_quirks(const std::string& provider,
                           batbox::Json&       body,
                           cpr::Header&        headers) {
    if (provider == "vllm") {
        body.erase("stream_options");
    } else if (provider == "ollama") {
        body.erase("stream_options");
        headers.erase("Authorization");
    } else if (provider == "llama-cpp") {
        body.erase("stream_options");
    }
    // together, groq, mistral, anthropic, lm-studio, openai: no transforms.
}

/// Returns true when the resolved provider requires no body mutations.
/// For no-quirk providers the preflight body is byte-identical to the wire body.
bool provider_needs_quirks(const std::string& provider) noexcept {
    return provider == "vllm" || provider == "ollama" || provider == "llama-cpp";
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// is_overflow_error — cross-provider context-overflow detection (S5, DIS-983)
// ---------------------------------------------------------------------------
//
// Ported in shape from opencode provider/error.ts:isOverflow.  The Client
// surfaces every non-2xx response as the string "http <code>: <body-excerpt>"
// (see chat()/stream_chat below), so the overflow signal lives in that excerpt.
// We lower-fold once and match a curated set of overflow signatures shared
// across the OpenAI-compatible endpoints batbox targets:
//
//   openai / groq / kimi / deepseek : "context_length_exceeded",
//                                      "maximum context length (is)"
//   vllm / llama-cpp                 : "maximum context length",
//                                      "please reduce the length of the messages"
//   anthropic-shaped gateways        : "prompt is too long",
//                                      "input length and max_tokens exceed context"
//   generic                          : "context window", "reduce the length",
//                                      "exceeds the context", "too many tokens"
//
// Deliberately conservative: a 429 rate-limit, a 401 auth error, or a generic
// 400 validation failure does NOT match, so it stays a normal error (AC1).
bool is_overflow_error(std::string_view error_message) noexcept {
    // Lower-fold into a local buffer (string_view → owned, ASCII fold only).
    std::string m;
    m.reserve(error_message.size());
    for (char c : error_message) {
        m.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }

    auto has = [&m](std::string_view needle) noexcept {
        return m.find(needle) != std::string::npos;
    };

    // Canonical OpenAI-family error code (most providers echo it verbatim).
    if (has("context_length_exceeded")) return true;
    if (has("context_window_exceeded")) return true;

    // Phrasing variants across providers.
    if (has("maximum context length"))  return true;   // openai/vllm
    if (has("maximum context"))         return true;   // "exceeds the model's maximum context"
    if (has("context window"))          return true;   // generic
    if (has("reduce the length"))       return true;   // openai/vllm "please reduce the length"
    if (has("exceeds the context"))     return true;
    if (has("exceed context"))          return true;
    if (has("exceeds context"))         return true;
    if (has("prompt is too long"))      return true;   // anthropic-shaped
    if (has("input is too long"))       return true;
    if (has("too many tokens"))         return true;
    if (has("exceed context limit"))    return true;   // anthropic "max_tokens exceed context limit"

    return false;
}

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
    ChatRequest wire_req = req;
    wire_req.stream = false;
    wire_req.stream_options_include_usage = std::nullopt;

    batbox::Json body_json = wire_req;

    const std::string provider = resolve_provider_hint(
        cfg_.api.provider_hint, cfg_.api.base_url);

    cpr::Header headers{
        {"Content-Type",  "application/json"},
        {"Authorization", "Bearer " + cfg_.api.api_key}
    };
    apply_provider_quirks(provider, body_json, headers);

    const std::string body_str = body_json.dump();
    const std::string url = completions_url();

    const std::int32_t timeout_ms =
        static_cast<std::int32_t>(cfg_.api.request_timeout_sec) * 1000;

    cpr::Response http = cpr::Post(
        cpr::Url{url},
        headers,
        cpr::Body{body_str},
        cpr::Timeout{timeout_ms}
    );

    if (http.error) {
        return batbox::Err("transport: " + http.error.message);
    }

    if (http.status_code < 200 || http.status_code >= 300) {
        const std::string excerpt =
            http.text.size() > 200 ? http.text.substr(0, 200) : http.text;
        return batbox::Err(
            "http " + std::to_string(http.status_code) + ": " + excerpt);
    }

    auto parse_result = batbox::parse(http.text);
    if (!parse_result) {
        return batbox::Err("parse: " + parse_result.error());
    }
    const batbox::Json& root = parse_result.value();

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
        flat["finish_reason"] = choice.value("finish_reason", std::string{"stop"});

        if (message.contains("content") && message["content"].is_string()) {
            flat["content"] = message["content"];
        } else {
            flat["content"] = nullptr;
        }

        if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
            flat["tool_calls"] = message["tool_calls"];
        }

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
// stream_chat() — ChatRequest overload
//
// PEXT2 3.1: Applies provider quirks and serialises to JSON exactly once
// before the retry loop, then delegates to stream_chat_impl().
//
// PEXT2 4.1 (D-3): For providers that require no body mutations (the common
// case: openai, together, groq, mistral, anthropic, lm-studio), the body is
// serialised using to_wire_string() which skips the intermediate nlohmann
// tree entirely — ~2000 fewer allocations per 50 KB request.  For quirk
// providers (vllm/ollama/llama-cpp) the nlohmann tree path is retained because
// apply_provider_quirks() needs a mutable Json object to erase fields.
// ---------------------------------------------------------------------------

Result<UsageDelta> Client::stream_chat(
    const ChatRequest&                           req,
    std::function<void(const StreamDelta&)>      on_delta,
    CancelToken                                  ct)
{
    // Force stream=true and request usage in the final streaming chunk.
    ChatRequest wire_req = req;
    wire_req.stream = true;
    wire_req.stream_options_include_usage = true;

    // Resolve provider once before deciding which serialisation path to use.
    const std::string provider = resolve_provider_hint(
        cfg_.api.provider_hint, cfg_.api.base_url);

    cpr::Header headers{
        {"Content-Type",  "application/json"},
        {"Authorization", "Bearer " + cfg_.api.api_key}
    };

    std::string body_str;

    if (provider_needs_quirks(provider)) {
        // Quirk path: build the nlohmann tree so apply_provider_quirks can
        // erase fields, then dump once.
        batbox::Json body_json = wire_req;
        apply_provider_quirks(provider, body_json, headers);
        body_str = body_json.dump();
    } else {
        // No-quirk path (PEXT2 4.1 D-3): bypass the nlohmann tree entirely.
        // to_wire_string() produces output byte-identical to body_json.dump()
        // without allocating ~2000 tree nodes.
        body_str.reserve(4096);
        to_wire_string(wire_req, body_str);
        // No body mutations needed; headers stay as-is.
    }

    // Determine the effective api_key to pass to stream_chat_impl.
    // apply_provider_quirks (quirk path only) erases Authorization for ollama.
    const bool auth_erased = (headers.find("Authorization") == headers.end());
    const std::string_view effective_api_key =
        auth_erased ? std::string_view{} : std::string_view{cfg_.api.api_key};

    const std::string url = completions_url();
    const std::int32_t timeout_ms =
        static_cast<std::int32_t>(cfg_.api.request_timeout_sec) * 1000;

    return stream_chat_impl(body_str, effective_api_key, url, timeout_ms,
                            std::move(on_delta), std::move(ct));
}

// ---------------------------------------------------------------------------
// stream_chat() — pre-serialised body overload (PEXT2 3.1)
//
// Called by Conversation::run_turn (iteration 0) with the body string already
// computed for the preflight token estimate and the mutex-snapshotted api_key.
//
// If the resolved provider requires body mutations (vllm/ollama/llama-cpp),
// the body string is parsed, mutated, and re-dumped ONCE before the retry
// loop.  For no-quirk providers (the common case: openai, together, groq,
// mistral, anthropic, lm-studio) the body is used verbatim — zero additional
// dumps.
// ---------------------------------------------------------------------------

Result<UsageDelta> Client::stream_chat(
    std::string                                  body_str,
    std::string_view                             api_key,
    std::function<void(const StreamDelta&)>      on_delta,
    CancelToken                                  ct)
{
    const std::string provider = resolve_provider_hint(
        cfg_.api.provider_hint, cfg_.api.base_url);

    // For quirk-requiring providers: parse the pre-built body, apply mutations,
    // redump once.  This path costs one extra parse + dump but avoids N dumps
    // over N retry attempts (the previous behaviour).
    if (provider_needs_quirks(provider)) {
        auto parsed = batbox::parse(body_str);
        if (!parsed) {
            return batbox::Err("internal: body_str is not valid JSON: " + parsed.error());
        }
        batbox::Json body_json = std::move(parsed.value());
        // Build a temporary headers map for apply_provider_quirks.  We only
        // use the headers to determine if Authorization should be erased; the
        // actual Authorization header is reconstructed in stream_chat_impl.
        cpr::Header tmp_headers{
            {"Content-Type",  "application/json"},
            {"Authorization", "Bearer " + std::string{api_key}}
        };
        apply_provider_quirks(provider, body_json, tmp_headers);
        body_str = body_json.dump();

        // For ollama, Authorization must be absent from the wire request.
        // stream_chat_impl builds its own headers from api_key — if ollama
        // erased it in tmp_headers, we need to pass an empty api_key so
        // stream_chat_impl's Authorization header is also absent.
        const bool auth_erased = (tmp_headers.find("Authorization") == tmp_headers.end());
        const std::string_view effective_api_key = auth_erased ? std::string_view{} : api_key;

        const std::string url = completions_url();
        const std::int32_t timeout_ms =
            static_cast<std::int32_t>(cfg_.api.request_timeout_sec) * 1000;
        return stream_chat_impl(body_str, effective_api_key, url, timeout_ms,
                                std::move(on_delta), std::move(ct));
    }

    // No-quirk path: body is wire-ready.  Zero additional dumps.
    const std::string url = completions_url();
    const std::int32_t timeout_ms =
        static_cast<std::int32_t>(cfg_.api.request_timeout_sec) * 1000;

    return stream_chat_impl(body_str, api_key, url, timeout_ms,
                            std::move(on_delta), std::move(ct));
}

// ---------------------------------------------------------------------------
// stream_chat_impl() — SSE streaming POST core implementation
//
// Both stream_chat overloads converge here after the body string has been
// prepared and (for the ChatRequest overload) quirks have been applied.
// The body_str is reused across all retry attempts — exactly one dump per call.
// ---------------------------------------------------------------------------

Result<UsageDelta> Client::stream_chat_impl(
    const std::string&                           body_str,
    std::string_view                             api_key,
    const std::string&                           url,
    std::int32_t                                 timeout_ms,
    std::function<void(const StreamDelta&)>      on_delta,
    CancelToken                                  ct)
{
    auto lg = batbox::log::get("inference.client");

    // Build headers from the caller-supplied api_key.
    //
    // When api_key is non-empty the Authorization header is always included.
    // When api_key is empty (ollama path — the quirk-applying string overload
    // passes an empty api_key when apply_provider_quirks erased Authorization)
    // the Authorization header is omitted entirely, matching what the original
    // ChatRequest overload produced via apply_provider_quirks.
    //
    // For Conversation::run_turn (pre-serialised-body overload) the api_key is
    // the mutex-snapshotted current_api_key, eliminating the D-8 data race.
    cpr::Header request_headers{{"Content-Type", "application/json"}};
    if (!api_key.empty()) {
        request_headers.insert({"Authorization", "Bearer " + std::string{api_key}});
    }

    // Retry loop — max 3 retries for 429/5xx before first token.
    constexpr int kMaxRetries = 3;

    for (int attempt = 0; attempt <= kMaxRetries; ++attempt) {

        bool token_received = false;
        bool stream_started = false;
        bool final_finish_reason_seen = false;
        UsageDelta final_usage{};
        std::string stream_error;

        // SSE parser — fresh on each retry attempt.
        SseParser sse_parser;

        // --- cpr WriteCallback ---
        // Receives raw bytes from libcurl as they arrive.  Feeds them to the
        // SseParser and dispatches complete events.

        auto write_cb = [&](std::string_view data, intptr_t /*userdata*/) -> bool {
            auto parse_result = sse_parser.feed(data);
            if (!parse_result) {
                stream_error = "sse: " + parse_result.error();
                lg->error("stream_chat: SseParser error: {}", parse_result.error());
                return false;
            }

            for (const SseEvent& ev : parse_result.value()) {
                lg->trace("sse: event={} data_bytes={} is_done={}",
                          ev.event.empty() ? "(default)" : ev.event,
                          ev.data.size(),
                          ev.is_done);

                if (ev.is_done) {
                    break;
                }

                // TUI-T17: Surface SSE error events.
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
                        server_msg = ev.data;
                    }
                    stream_error = "server: " + server_msg;
                    lg->error("stream_chat: server emitted SSE error event: {}",
                              server_msg);
                    return false;
                }

                if (!ev.event.empty()) {
                    continue;
                }

                try {
                    auto chunk_result = batbox::parse(ev.data);
                    if (!chunk_result) {
                        lg->warn("stream_chat: malformed chunk JSON: {}", ev.data);
                        continue;
                    }
                    const batbox::Json& chunk = chunk_result.value();

                    // TUI-T17: Detect inline server errors.
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
                        return false;
                    }

                    if (!chunk.contains("choices") || !chunk["choices"].is_array()
                            || chunk["choices"].empty()) {
                        if (chunk.contains("usage") && chunk["usage"].is_object()) {
                            final_usage = chunk["usage"].get<UsageDelta>();
                        }
                        continue;
                    }

                    const batbox::Json& choice = chunk["choices"][0];
                    const batbox::Json& delta_obj =
                        choice.contains("delta") ? choice["delta"]
                                                 : batbox::Json::object();

                    StreamDelta delta = delta_obj.get<StreamDelta>();

                    if (choice.contains("finish_reason")
                            && choice["finish_reason"].is_string()) {
                        delta.finish_reason = choice["finish_reason"].get<std::string>();
                        final_finish_reason_seen = true;
                    }

                    if (chunk.contains("usage") && chunk["usage"].is_object()) {
                        final_usage = chunk["usage"].get<UsageDelta>();
                        delta.usage = final_usage;
                    }

                    if (delta.content.has_value() && !delta.content->empty()) {
                        token_received = true;
                    }
                    if (delta.tool_calls.has_value() && !delta.tool_calls->empty()) {
                        token_received = true;
                    }

                    if (!stream_started) {
                        if ((delta.content.has_value()          && !delta.content->empty())
                         || (delta.reasoning_content.has_value() && !delta.reasoning_content->empty())
                         || (delta.tool_calls.has_value()        && !delta.tool_calls->empty())) {
                            stream_started = true;
                        }
                    }

                    on_delta(delta);

                } catch (const std::exception& e) {
                    lg->warn("stream_chat: chunk parse exception: {}", e.what());
                    continue;
                }
            }

            return true;
        };

        // --- cpr ProgressCallback ---
        auto progress_cb = [&ct](cpr::cpr_pf_arg_t /*dltotal*/,
                                  cpr::cpr_pf_arg_t /*dlnow*/,
                                  cpr::cpr_pf_arg_t /*ultotal*/,
                                  cpr::cpr_pf_arg_t /*ulnow*/,
                                  intptr_t         /*userdata*/) -> bool {
            return !ct.stop_requested();
        };

        // --- Fire the streaming POST ---

        cpr::Session session;
        session.SetUrl(cpr::Url{url});
        session.SetHeader(request_headers);
        session.SetBody(cpr::Body{body_str});
        session.SetTimeout(cpr::Timeout{timeout_ms});
        session.SetWriteCallback(cpr::WriteCallback{write_cb});
        session.SetProgressCallback(cpr::ProgressCallback{progress_cb});

        // B2: Idle-stream timeout via CURLOPT_LOW_SPEED_LIMIT + CURLOPT_LOW_SPEED_TIME.
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
        if (!stream_error.empty()) {
            return batbox::Err(stream_error);
        }

        // --- Check for transport errors ---
        if (http.error) {
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

            if (!stream_started && is_retriable_status(http.status_code)
                    && attempt < kMaxRetries) {
                const auto delay = jitter(kRetryBackoff[attempt]);
                lg->warn(
                    "stream_chat: attempt {}/{} failed with {} — retrying in {}ms",
                    attempt + 1, kMaxRetries,
                    http.status_code,
                    delay.count());
                std::this_thread::sleep_for(delay);
                continue;
            }

            return batbox::Err(err_msg);
        }

        // --- Empty-stream guard ---
        if (!token_received && !final_finish_reason_seen) {
            lg->warn("stream_chat: 2xx response but no content or finish_reason "
                     "delivered — server may have truncated mid-reasoning");
            return batbox::Err(std::string{
                "stream ended without content (server may have truncated "
                "during reasoning phase — try increasing BATBOX_MAX_TOKENS "
                "or the model's context length)"
            });
        }

        return final_usage;

    } // end retry loop

    return batbox::Err(std::string{"stream_chat: retry loop exhausted"});
}

} // namespace batbox::inference
