// tests/unit/test_overflow_detection.cpp
// =============================================================================
// S5 (DIS-983) AC1 — cross-provider context-overflow detection.
//
// is_overflow_error() normalises the loud "the prompt exceeds the model's
// context window" errors that OpenAI-compatible endpoints return into ONE typed
// signal, so Conversation can react (compact + retry once).  The Client surfaces
// every non-2xx as "http <code>: <body-excerpt>", so this is matching over that
// text.  The decisive properties:
//   - genuine overflow signatures across openai/vllm/groq/kimi/deepseek and
//     anthropic-shaped gateways → true
//   - rate-limit / auth / generic validation / transport errors → false
//     ("a non-overflow error stays a normal error")
//   - matching is case-insensitive
//
// Build standalone (from repo root, x64-linux triplet):
//   c++ -std=c++20 -I include -I build/vcpkg_installed/x64-linux/include \
//       tests/unit/test_overflow_detection.cpp \
//       src/inference/Client.cpp src/inference/ChatRequest.cpp \
//       src/inference/SseParser.cpp \
//       src/core/Uuid.cpp src/core/CancelToken.cpp src/core/Logging.cpp \
//       src/core/Json.cpp \
//       build/vcpkg_installed/x64-linux/lib/libcpr.a \
//       build/vcpkg_installed/x64-linux/lib/libcurl.a \
//       build/vcpkg_installed/x64-linux/lib/libsimdjson.a \
//       build/vcpkg_installed/x64-linux/lib/libspdlog.a \
//       build/vcpkg_installed/x64-linux/lib/libfmt.a \
//       build/vcpkg_installed/x64-linux/lib/libssl.a \
//       build/vcpkg_installed/x64-linux/lib/libcrypto.a \
//       build/vcpkg_installed/x64-linux/lib/libz.a -lpthread -ldl \
//       -o /tmp/test_overflow_detection && /tmp/test_overflow_detection
//   (Uuid.cpp must be the DIS-969-fixed copy.)
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/inference/Client.hpp>

using batbox::inference::is_overflow_error;

TEST_SUITE("S5 is_overflow_error") {

    // -----------------------------------------------------------------------
    // Positive matrix — genuine overflow across the providers batbox drives.
    // Each string is shaped like the Client's "http <code>: <body-excerpt>".
    // -----------------------------------------------------------------------
    TEST_CASE("openai-family context_length_exceeded code") {
        CHECK(is_overflow_error(
            "http 400: {\"error\":{\"message\":\"This model's maximum context "
            "length is 8192 tokens, however you requested 9000 tokens.\","
            "\"code\":\"context_length_exceeded\"}}"));
    }
    TEST_CASE("vllm maximum context length / reduce the length phrasing") {
        CHECK(is_overflow_error(
            "http 400: This model's maximum context length is 4096 tokens. "
            "Please reduce the length of the messages."));
    }
    TEST_CASE("groq context_length_exceeded") {
        CHECK(is_overflow_error(
            "http 400: {\"error\":{\"code\":\"context_length_exceeded\"}}"));
    }
    TEST_CASE("kimi/deepseek maximum context phrasing") {
        CHECK(is_overflow_error(
            "http 400: input exceeds the model's maximum context"));
    }
    TEST_CASE("anthropic-shaped gateway: prompt is too long") {
        CHECK(is_overflow_error("http 400: prompt is too long: 250000 tokens"));
    }
    TEST_CASE("anthropic-shaped: max_tokens exceed context limit") {
        CHECK(is_overflow_error(
            "http 400: input length and max_tokens exceed context limit"));
    }
    TEST_CASE("generic context window phrasing") {
        CHECK(is_overflow_error("http 400: requested tokens exceed context window"));
    }
    TEST_CASE("case-insensitive match") {
        CHECK(is_overflow_error("HTTP 400: CONTEXT_LENGTH_EXCEEDED"));
        CHECK(is_overflow_error("http 400: Maximum Context Length Is 8192"));
    }

    // -----------------------------------------------------------------------
    // Negative matrix — non-overflow errors stay normal errors.
    // -----------------------------------------------------------------------
    TEST_CASE("rate limit is not overflow") {
        CHECK_FALSE(is_overflow_error(
            "http 429: {\"error\":{\"message\":\"Rate limit reached\","
            "\"code\":\"rate_limit_exceeded\"}}"));
    }
    TEST_CASE("auth failure is not overflow") {
        CHECK_FALSE(is_overflow_error(
            "http 401: {\"error\":{\"message\":\"Invalid API key\"}}"));
    }
    TEST_CASE("generic 400 validation is not overflow") {
        CHECK_FALSE(is_overflow_error(
            "http 400: {\"error\":{\"message\":\"Unknown parameter: foo\"}}"));
    }
    TEST_CASE("server error is not overflow") {
        CHECK_FALSE(is_overflow_error("http 500: internal server error"));
    }
    TEST_CASE("transport error is not overflow") {
        CHECK_FALSE(is_overflow_error("transport: Could not resolve host"));
    }
    TEST_CASE("empty and unrelated strings are not overflow") {
        CHECK_FALSE(is_overflow_error(""));
        CHECK_FALSE(is_overflow_error("stream ended without content"));
        CHECK_FALSE(is_overflow_error("cancelled"));
    }
}
