// tests/unit/test_think_splitter.cpp
// =============================================================================
// S10 — ThinkSplitter: inline `<think>…</think>` visible/reasoning separation.
//
// Coverage (DIS-975 acceptance criteria):
//   AC1  ThinkSplitter exists as a standalone, unit-testable component that
//        separates streamed content into visible vs. reasoning.
//   AC2  Cross-chunk-boundary correctness: the same logical stream chopped at
//        adversarial boundaries (mid-tag, byte-by-byte) yields identical
//        visible + reasoning output to the un-chopped case, and no partial
//        marker is ever emitted as visible.
//   AC3  Tag pair is configurable and sourced from the provider profile;
//        default `<think>`/`</think>`; a provider can declare "no inline tags".
//        Tested for two tag conventions + the none case.
//   AC4  Edge cases: no block / spanning / multiple / surrounding text /
//        unclosed / stray-close — each with defined, tested behaviour.
//
// Build (standalone, no CMake — from repo root):
//   c++ -std=c++20 -I include \
//       tests/unit/test_think_splitter.cpp \
//       src/inference/ThinkSplitter.cpp \
//       src/inference/ReasoningTagProfile.cpp \
//       -o /tmp/test_ts && /tmp/test_ts
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/inference/ThinkSplitter.hpp>

#include <string>
#include <vector>

using namespace batbox::inference;

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

/// Feed @p chunks through a fresh splitter (with tags @p tags) and return the
/// fully-accumulated {visible, reasoning} after finish().
static ThinkSplit run_chunks(const std::vector<std::string>& chunks,
                             ReasoningTags tags = ReasoningTags{}) {
    ThinkSplitter splitter(std::move(tags));
    ThinkSplit acc;
    for (const auto& c : chunks) {
        ThinkSplit s = splitter.push(c);
        acc.visible   += s.visible;
        acc.reasoning += s.reasoning;
    }
    ThinkSplit tail = splitter.finish();
    acc.visible   += tail.visible;
    acc.reasoning += tail.reasoning;
    return acc;
}

/// Split @p s into every-1-byte chunks.
static std::vector<std::string> byte_by_byte(const std::string& s) {
    std::vector<std::string> out;
    out.reserve(s.size());
    for (char c : s) out.emplace_back(1, c);
    return out;
}

// =============================================================================
// AC1 — basic separation
// =============================================================================
TEST_SUITE("ThinkSplitter — basic separation (AC1)") {

    TEST_CASE("no think block: pure passthrough to visible") {
        auto r = run_chunks({"Hello, world!"});
        CHECK(r.visible   == "Hello, world!");
        CHECK(r.reasoning == "");
    }

    TEST_CASE("single think block: inner is reasoning, tags consumed") {
        auto r = run_chunks({"<think>plan the answer</think>The answer is 42."});
        CHECK(r.visible   == "The answer is 42.");
        CHECK(r.reasoning == "plan the answer");
    }

    TEST_CASE("text before and after a block") {
        auto r = run_chunks({"Before. <think>hidden</think> After."});
        CHECK(r.visible   == "Before.  After.");
        CHECK(r.reasoning == "hidden");
    }

    TEST_CASE("empty input") {
        auto r = run_chunks({""});
        CHECK(r.visible.empty());
        CHECK(r.reasoning.empty());
    }
}

// =============================================================================
// AC4 — edge cases
// =============================================================================
TEST_SUITE("ThinkSplitter — edge cases (AC4)") {

    TEST_CASE("multiple think blocks in one turn") {
        auto r = run_chunks({"a<think>r1</think>b<think>r2</think>c"});
        CHECK(r.visible   == "abc");
        CHECK(r.reasoning == "r1r2");
    }

    TEST_CASE("unclosed think block at stream end: remainder is reasoning") {
        auto r = run_chunks({"visible<think>dangling reasoning with no close"});
        CHECK(r.visible   == "visible");
        CHECK(r.reasoning == "dangling reasoning with no close");
    }

    TEST_CASE("stray close tag with no opener: passes through as visible") {
        auto r = run_chunks({"plain </think> text"});
        CHECK(r.visible   == "plain </think> text");
        CHECK(r.reasoning == "");
    }

    TEST_CASE("reasoning text that itself contains the word think") {
        auto r = run_chunks({"<think>I think about thinking</think>done"});
        CHECK(r.visible   == "done");
        CHECK(r.reasoning == "I think about thinking");
    }

    TEST_CASE("lone '<' that is not a tag is preserved as visible") {
        auto r = run_chunks({"2 < 3 and 4 > 1"});
        CHECK(r.visible   == "2 < 3 and 4 > 1");
        CHECK(r.reasoning == "");
    }

    TEST_CASE("'<' inside reasoning that is not the close tag stays reasoning") {
        auto r = run_chunks({"<think>a<b and c<d</think>v"});
        CHECK(r.visible   == "v");
        CHECK(r.reasoning == "a<b and c<d");
    }

    TEST_CASE("partial open prefix at stream end flushes to visible") {
        // Stream ends mid-way through what could have become "<think>".
        auto r = run_chunks({"tail<thi"});
        CHECK(r.visible   == "tail<thi");
        CHECK(r.reasoning == "");
    }

    TEST_CASE("empty think block") {
        auto r = run_chunks({"x<think></think>y"});
        CHECK(r.visible   == "xy");
        CHECK(r.reasoning == "");
    }

    TEST_CASE("block at very start, nothing after") {
        auto r = run_chunks({"<think>only reasoning</think>"});
        CHECK(r.visible   == "");
        CHECK(r.reasoning == "only reasoning");
    }
}

// =============================================================================
// AC2 — cross-chunk-boundary correctness (the part Wren will hammer)
// =============================================================================
TEST_SUITE("ThinkSplitter — cross-chunk-boundary correctness (AC2)") {

    // The canonical chunkings we assert chunk-invariance over.
    static const std::vector<std::string> kStreams = {
        "Before<think>secret plan</think>After",
        "no tags at all here",
        "<think>leading</think>trailing",
        "a<think>one</think>b<think>two</think>c",
        "open but never closed <think>and then EOF",
        "ends with a lonely <thi",
        "stray </think> with no opener",
        "2 < 3, x<y, and <think>cmp r</think> ok",
    };

    TEST_CASE("byte-by-byte chopping matches whole-string for every stream") {
        for (const auto& s : kStreams) {
            CAPTURE(s);
            ThinkSplit whole = run_chunks({s});
            ThinkSplit per_byte = run_chunks(byte_by_byte(s));
            CHECK(whole.visible   == per_byte.visible);
            CHECK(whole.reasoning == per_byte.reasoning);
        }
    }

    TEST_CASE("every split point of a tag-bearing stream is invariant") {
        const std::string s = "Before<think>secret plan</think>After";
        ThinkSplit whole = run_chunks({s});
        for (std::size_t i = 0; i <= s.size(); ++i) {
            CAPTURE(i);
            std::vector<std::string> two = {s.substr(0, i), s.substr(i)};
            ThinkSplit split = run_chunks(two);
            CHECK(split.visible   == whole.visible);
            CHECK(split.reasoning == whole.reasoning);
        }
    }

    TEST_CASE("marker split mid-tag (\"<thi\" | \"nk>\") still separates") {
        auto r = run_chunks({"keep<thi", "nk>hide</thi", "nk>keep2"});
        CHECK(r.visible   == "keepkeep2");
        CHECK(r.reasoning == "hide");
    }

    TEST_CASE("no partial marker is ever returned as visible mid-stream") {
        // Feed exactly up to "<thin" — a strict prefix of "<think>".  None of it
        // may surface as visible yet, because it might still complete the tag.
        ThinkSplitter sp;
        ThinkSplit a = sp.push("ok<thin");
        CHECK(a.visible   == "ok");      // only the confirmed-visible "ok"
        CHECK(a.reasoning == "");
        // Complete the tag and the reasoning body — still nothing leaked.
        ThinkSplit b = sp.push("k>body</think>done");
        CHECK(b.visible   == "done");
        CHECK(b.reasoning == "body");
    }

    TEST_CASE("false-alarm prefix that diverges is released as visible") {
        // "<thinker" shares a prefix with "<think>" then diverges at 'e' vs '>'.
        auto r = run_chunks({"<thinker reads"});
        CHECK(r.visible   == "<thinker reads");
        CHECK(r.reasoning == "");
    }
}

// =============================================================================
// AC3 — configurable tag pair + provider profile (two conventions + none)
// =============================================================================
TEST_SUITE("ThinkSplitter — configurable tags (AC3)") {

    TEST_CASE("custom <reasoning> convention") {
        ReasoningTags tags{"<reasoning>", "</reasoning>"};
        auto r = run_chunks({"a<reasoning>r</reasoning>b"}, tags);
        CHECK(r.visible   == "ab");
        CHECK(r.reasoning == "r");
    }

    TEST_CASE("custom <thinking> convention (Anthropic-style)") {
        ReasoningTags tags{"<thinking>", "</thinking>"};
        auto r = run_chunks({"x<thinking>deep</thinking>y"}, tags);
        CHECK(r.visible   == "xy");
        CHECK(r.reasoning == "deep");
    }

    TEST_CASE("default convention does not catch a different tag") {
        // With the default <think> tags, a <reasoning> tag is just visible text.
        auto r = run_chunks({"a<reasoning>r</reasoning>b"});
        CHECK(r.visible   == "a<reasoning>r</reasoning>b");
        CHECK(r.reasoning == "");
    }

    TEST_CASE("none() disables splitting: everything is visible") {
        auto r = run_chunks({"a<think>r</think>b"}, ReasoningTags::none());
        CHECK(r.visible   == "a<think>r</think>b");
        CHECK(r.reasoning == "");
    }

    TEST_CASE("ReasoningTags::enabled reflects emptiness") {
        CHECK(ReasoningTags{}.enabled());
        CHECK_FALSE(ReasoningTags::none().enabled());
        CHECK_FALSE((ReasoningTags{"<think>", ""}).enabled());
        CHECK_FALSE((ReasoningTags{"", "</think>"}).enabled());
    }
}

TEST_SUITE("reasoning_tags_for_provider — provider profile (AC3)") {

    TEST_CASE("deepseek declares the default <think> convention") {
        auto t = reasoning_tags_for_provider("deepseek");
        CHECK(t.open  == "<think>");
        CHECK(t.close == "</think>");
        CHECK(t.enabled());
    }

    TEST_CASE("ollama also defaults to <think>") {
        auto t = reasoning_tags_for_provider("ollama");
        CHECK(t.open  == "<think>");
        CHECK(t.close == "</think>");
    }

    TEST_CASE("anthropic declares the <thinking> convention") {
        auto t = reasoning_tags_for_provider("anthropic");
        CHECK(t.open  == "<thinking>");
        CHECK(t.close == "</thinking>");
        CHECK(t.enabled());
    }

    TEST_CASE("openai declares NO inline tags (structured field only)") {
        auto t = reasoning_tags_for_provider("openai");
        CHECK_FALSE(t.enabled());
    }

    TEST_CASE("groq / mistral / together also declare none") {
        CHECK_FALSE(reasoning_tags_for_provider("groq").enabled());
        CHECK_FALSE(reasoning_tags_for_provider("mistral").enabled());
        CHECK_FALSE(reasoning_tags_for_provider("together").enabled());
    }

    TEST_CASE("provider name is case-insensitive") {
        CHECK_FALSE(reasoning_tags_for_provider("OpenAI").enabled());
        CHECK(reasoning_tags_for_provider("DeepSeek").enabled());
    }

    TEST_CASE("unknown provider falls back to the <think> default") {
        auto t = reasoning_tags_for_provider("some-new-local-thing");
        CHECK(t.open  == "<think>");
        CHECK(t.close == "</think>");
    }

    TEST_CASE("a splitter built from a provider profile splits accordingly") {
        // End-to-end: profile → ThinkSplitter → separation.
        auto r = run_chunks({"v<thinking>r</thinking>w"},
                            reasoning_tags_for_provider("anthropic"));
        CHECK(r.visible   == "vw");
        CHECK(r.reasoning == "r");
    }
}
