// tests/unit/test_reasoning_accumulator.cpp
// =============================================================================
// S10 — ReasoningAccumulator: the unified isolated reasoning channel.
//
// Coverage (DIS-975 acceptance criterion 5):
//   - Visible output is reasoning-free for the STRUCTURED-field path
//     (delta.reasoning_content).
//   - Visible output is reasoning-free for the INLINE-tag path
//     (`<think>…</think>` inside delta.content).
//   - The full isolated reasoning text is retrievable by a consumer.
//   - Mixed case: a stream that uses BOTH forms isolates both, each exactly once
//     (no double-count), and visible stays clean.
//
// StreamDelta is constructed by direct field assignment (no JSON round-trip),
// the same approach test_client_reasoning.cpp uses for its make_*_delta helpers.
//
// Build (standalone, no CMake — from repo root):
//   c++ -std=c++20 -I include \
//       tests/unit/test_reasoning_accumulator.cpp \
//       src/inference/ReasoningAccumulator.cpp \
//       src/inference/ThinkSplitter.cpp \
//       build/vcpkg_installed/x64-linux/lib/libsimdjson.a \
//       -o /tmp/test_ra && /tmp/test_ra
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/inference/ReasoningAccumulator.hpp>
#include <batbox/inference/ChatResponse.hpp>

#include <string>
#include <vector>

using namespace batbox::inference;

// -----------------------------------------------------------------------------
// StreamDelta builders (field assignment — no wire parsing)
// -----------------------------------------------------------------------------
static StreamDelta reasoning_delta(const std::string& text) {
    StreamDelta sd;
    sd.reasoning_content = text;
    return sd;
}
static StreamDelta content_delta(const std::string& text) {
    StreamDelta sd;
    sd.content = text;
    return sd;
}
static StreamDelta both_delta(const std::string& reasoning, const std::string& content) {
    StreamDelta sd;
    sd.reasoning_content = reasoning;
    sd.content           = content;
    return sd;
}

/// Drive a whole stream through a fresh accumulator and return the visible text
/// the consumer would have seen (accumulate() returns + finish()).
static std::string drive(ReasoningAccumulator& acc,
                         const std::vector<StreamDelta>& stream) {
    std::string visible;
    for (const auto& d : stream) visible += acc.accumulate(d);
    visible += acc.finish();
    return visible;
}

// =============================================================================
// Structured-field path
// =============================================================================
TEST_SUITE("ReasoningAccumulator — structured field path (AC5)") {

    TEST_CASE("reasoning_content is isolated; visible stays empty") {
        ReasoningAccumulator acc;
        std::string visible = drive(acc, {
            reasoning_delta("Let me think step by step."),
            reasoning_delta(" Therefore 42."),
            content_delta("The answer is 42."),
        });
        CHECK(visible          == "The answer is 42.");
        CHECK(acc.visible()    == "The answer is 42.");
        CHECK(acc.reasoning()  == "Let me think step by step. Therefore 42.");
        CHECK(acc.has_reasoning());
    }

    TEST_CASE("empty reasoning_content contributes nothing") {
        ReasoningAccumulator acc;
        drive(acc, { reasoning_delta(""), content_delta("hi") });
        CHECK(acc.reasoning().empty());
        CHECK(acc.visible() == "hi");
    }
}

// =============================================================================
// Inline-tag path
// =============================================================================
TEST_SUITE("ReasoningAccumulator — inline tag path (AC5)") {

    TEST_CASE("inline <think> is stripped from visible and isolated") {
        ReasoningAccumulator acc;
        std::string visible = drive(acc, {
            content_delta("Sure. <think>they want brevity</think>"),
            content_delta("Here it is."),
        });
        CHECK(visible         == "Sure. Here it is.");
        CHECK(acc.reasoning() == "they want brevity");
    }

    TEST_CASE("inline block split across deltas: still clean and isolated") {
        ReasoningAccumulator acc;
        std::string visible = drive(acc, {
            content_delta("a<thi"),
            content_delta("nk>secret</thi"),
            content_delta("nk>b"),
        });
        CHECK(visible         == "ab");
        CHECK(acc.reasoning() == "secret");
    }

    TEST_CASE("unclosed inline block at end: tail becomes reasoning, no hang") {
        ReasoningAccumulator acc;
        std::string visible = drive(acc, {
            content_delta("visible <think>cut off"),
        });
        CHECK(visible         == "visible ");
        CHECK(acc.reasoning() == "cut off");
    }

    TEST_CASE("none() provider: inline tags pass through as visible") {
        ReasoningAccumulator acc(ReasoningTags::none());
        std::string visible = drive(acc, {
            content_delta("a<think>x</think>b"),
        });
        CHECK(visible         == "a<think>x</think>b");
        CHECK(acc.reasoning().empty());
    }
}

// =============================================================================
// Mixed / no-double-count
// =============================================================================
TEST_SUITE("ReasoningAccumulator — mixed sources, no double-count (AC5)") {

    TEST_CASE("a single delta carrying BOTH forms isolates each exactly once") {
        ReasoningAccumulator acc;
        // Structured "S" + content that itself contains an inline block.
        std::string visible = drive(acc, {
            both_delta("S", "vis1<think>I1</think>vis2"),
        });
        CHECK(visible         == "vis1vis2");
        // Reasoning = structured "S" then inline "I1", each present once.
        CHECK(acc.reasoning() == "SI1");
    }

    TEST_CASE("interleaved structured and inline across a stream") {
        ReasoningAccumulator acc;
        std::string visible = drive(acc, {
            reasoning_delta("struct-a"),
            content_delta("hello <think>inline-b</think>"),
            reasoning_delta("struct-c"),
            content_delta(" world"),
        });
        CHECK(visible         == "hello  world");
        CHECK(acc.reasoning() == "struct-ainline-bstruct-c");
    }

    TEST_CASE("visible is reasoning-free regardless of form (the core guarantee)") {
        ReasoningAccumulator acc;
        std::string visible = drive(acc, {
            reasoning_delta("never visible 1"),
            content_delta("<think>never visible 2</think>"),
            content_delta("ONLY THIS"),
            reasoning_delta("never visible 3"),
        });
        CHECK(visible == "ONLY THIS");
        CHECK(visible.find("never visible") == std::string::npos);
    }

    TEST_CASE("no reasoning at all: plain content streams straight through") {
        ReasoningAccumulator acc;
        std::string visible = drive(acc, {
            content_delta("just "),
            content_delta("text"),
        });
        CHECK(visible == "just text");
        CHECK_FALSE(acc.has_reasoning());
    }
}
