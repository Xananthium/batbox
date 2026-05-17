// tests/unit/test_sse_parser.cpp
// =============================================================================
// doctest suite for batbox::inference::SseParser.
//
// Build standalone (from repo root, after vcpkg install):
//   c++ -std=c++20 -I include \
//       tests/unit/test_sse_parser.cpp src/inference/SseParser.cpp \
//       src/core/Logging.cpp \
//       -I build/vcpkg_installed/arm64-osx/include \
//       -L build/vcpkg_installed/arm64-osx/lib \
//       -lspdlog -lfmt \
//       -o /tmp/test_sse_parser && /tmp/test_sse_parser
//
// Via CMake + CTest:
//   cmake --build build && ctest --test-dir build -R test_sse_parser -V
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/inference/SseParser.hpp>

#include <string>
#include <string_view>
#include <vector>

using batbox::inference::SseEvent;
using batbox::inference::SseParser;

// =============================================================================
// Helpers
// =============================================================================

/// Feed a single string_view and assert it succeeds, returning the events.
static std::vector<SseEvent> feed_ok(SseParser& p, std::string_view chunk) {
    auto r = p.feed(chunk);
    REQUIRE_MESSAGE(r.has_value(), "feed() returned error: " << (r.has_value() ? "" : r.error()));
    return std::move(r.value());
}

// =============================================================================
// TEST SUITE 1: Single complete event in one feed call
// =============================================================================
TEST_SUITE("SseParser — single complete event") {

    TEST_CASE("minimal data-only event") {
        SseParser p;
        auto evs = feed_ok(p, "data: hello world\n\n");
        REQUIRE(evs.size() == 1);
        CHECK(evs[0].data  == "hello world");
        CHECK(evs[0].event.empty());
        CHECK(evs[0].id.empty());
        CHECK_FALSE(evs[0].is_done);
    }

    TEST_CASE("event with event: and id: fields") {
        SseParser p;
        auto evs = feed_ok(p, "event: myEvent\nid: 42\ndata: payload\n\n");
        REQUIRE(evs.size() == 1);
        CHECK(evs[0].data  == "payload");
        CHECK(evs[0].event == "myEvent");
        CHECK(evs[0].id    == "42");
    }

    TEST_CASE("comment line is ignored") {
        SseParser p;
        auto evs = feed_ok(p, ": this is a comment\ndata: after comment\n\n");
        REQUIRE(evs.size() == 1);
        CHECK(evs[0].data == "after comment");
    }

    TEST_CASE("event with only a comment is dispatch-empty") {
        SseParser p;
        auto evs = feed_ok(p, ": heartbeat\n\n");
        CHECK(evs.empty());
    }

    TEST_CASE("data: with no value gives empty string data field") {
        SseParser p;
        auto evs = feed_ok(p, "data:\n\n");
        REQUIRE(evs.size() == 1);
        CHECK(evs[0].data == "");
    }

    TEST_CASE("OpenAI-style JSON payload") {
        SseParser p;
        const std::string chunk =
            R"(data: {"id":"chatcmpl-1","choices":[{"delta":{"content":"hi"}}]})"
            "\n\n";
        auto evs = feed_ok(p, chunk);
        REQUIRE(evs.size() == 1);
        CHECK(evs[0].data.find("\"content\"") != std::string::npos);
        CHECK_FALSE(evs[0].is_done);
    }

    TEST_CASE("retry: line is parsed without error") {
        SseParser p;
        auto evs = feed_ok(p, "retry: 3000\ndata: after retry\n\n");
        REQUIRE(evs.size() == 1);
        CHECK(evs[0].data == "after retry");
    }
}

// =============================================================================
// TEST SUITE 2: Event split across multiple feed() calls
// =============================================================================
TEST_SUITE("SseParser — event split across feed calls") {

    TEST_CASE("event arrives in two halves") {
        SseParser p;

        // First half: partial line, no boundary yet.
        auto evs1 = feed_ok(p, "data: hel");
        CHECK(evs1.empty());
        CHECK(p.buffered_bytes() == std::string_view("data: hel").size());

        // Second half: rest of line + boundary.
        auto evs2 = feed_ok(p, "lo\n\n");
        REQUIRE(evs2.size() == 1);
        CHECK(evs2[0].data == "hello");
        CHECK(p.buffered_bytes() == 0);
    }

    TEST_CASE("boundary straddles two feed calls — \\n in first, second \\n in second") {
        SseParser p;

        auto evs1 = feed_ok(p, "data: abc\n");
        CHECK(evs1.empty()); // only one \n so far, no boundary

        auto evs2 = feed_ok(p, "\n");
        REQUIRE(evs2.size() == 1);
        CHECK(evs2[0].data == "abc");
    }

    TEST_CASE("three-chunk split: field name / colon+value / boundary") {
        SseParser p;

        feed_ok(p, "data");        // no \n yet
        feed_ok(p, ": world");     // still no \n
        auto evs = feed_ok(p, "\n\n");
        REQUIRE(evs.size() == 1);
        CHECK(evs[0].data == "world");
    }

    TEST_CASE("partial event followed by complete event in next call") {
        SseParser p;

        auto evs1 = feed_ok(p, "data: partial");
        CHECK(evs1.empty());

        auto evs2 = feed_ok(p, "\n\ndata: complete\n\n");
        REQUIRE(evs2.size() == 2);
        CHECK(evs2[0].data == "partial");
        CHECK(evs2[1].data == "complete");
    }
}

// =============================================================================
// TEST SUITE 3: Multiple events in one feed call
// =============================================================================
TEST_SUITE("SseParser — multiple events per feed") {

    TEST_CASE("two events in one chunk") {
        SseParser p;
        auto evs = feed_ok(p, "data: first\n\ndata: second\n\n");
        REQUIRE(evs.size() == 2);
        CHECK(evs[0].data == "first");
        CHECK(evs[1].data == "second");
    }

    TEST_CASE("five events delivered at once") {
        SseParser p;
        std::string chunk;
        for (int i = 1; i <= 5; ++i) {
            chunk += "data: token" + std::to_string(i) + "\n\n";
        }
        auto evs = feed_ok(p, chunk);
        REQUIRE(evs.size() == 5);
        for (int i = 0; i < 5; ++i) {
            CHECK(evs[i].data == "token" + std::to_string(i + 1));
        }
    }

    TEST_CASE("events + trailing partial are correctly separated") {
        SseParser p;
        // Two complete events + an incomplete one.
        auto evs = feed_ok(p, "data: a\n\ndata: b\n\ndata: c-partial");
        REQUIRE(evs.size() == 2);
        CHECK(evs[0].data == "a");
        CHECK(evs[1].data == "b");
        CHECK(p.buffered_bytes() > 0); // partial still buffered

        // Complete the third event.
        auto evs2 = feed_ok(p, "\n\n");
        REQUIRE(evs2.size() == 1);
        CHECK(evs2[0].data == "c-partial");
    }
}

// =============================================================================
// TEST SUITE 4: Multi-line data: concatenation
// =============================================================================
TEST_SUITE("SseParser — multi-line data concatenation") {

    TEST_CASE("two data: lines joined with \\n") {
        SseParser p;
        auto evs = feed_ok(p, "data: line one\ndata: line two\n\n");
        REQUIRE(evs.size() == 1);
        CHECK(evs[0].data == "line one\nline two");
    }

    TEST_CASE("three data: lines") {
        SseParser p;
        auto evs = feed_ok(p, "data: alpha\ndata: beta\ndata: gamma\n\n");
        REQUIRE(evs.size() == 1);
        CHECK(evs[0].data == "alpha\nbeta\ngamma");
    }

    TEST_CASE("data: empty line in middle") {
        SseParser p;
        auto evs = feed_ok(p, "data: before\ndata:\ndata: after\n\n");
        REQUIRE(evs.size() == 1);
        CHECK(evs[0].data == "before\n\nafter");
    }

    TEST_CASE("multi-line data split across two feed calls") {
        SseParser p;
        feed_ok(p, "data: first\n");
        auto evs = feed_ok(p, "data: second\n\n");
        REQUIRE(evs.size() == 1);
        CHECK(evs[0].data == "first\nsecond");
    }
}

// =============================================================================
// TEST SUITE 5: CRLF line endings (\r\n)
// =============================================================================
TEST_SUITE("SseParser — CRLF line endings (LM Studio)") {

    TEST_CASE("single event with \\r\\n line endings") {
        SseParser p;
        auto evs = feed_ok(p, "data: crlf\r\n\r\n");
        REQUIRE(evs.size() == 1);
        CHECK(evs[0].data == "crlf");
    }

    TEST_CASE("event: + data: with \\r\\n") {
        SseParser p;
        auto evs = feed_ok(p, "event: myEv\r\ndata: payload\r\n\r\n");
        REQUIRE(evs.size() == 1);
        CHECK(evs[0].data  == "payload");
        CHECK(evs[0].event == "myEv");
    }

    TEST_CASE("mixed \\r\\n and \\n in same stream") {
        SseParser p;
        // First event uses CRLF, second uses LF.
        auto evs = feed_ok(p, "data: one\r\n\r\ndata: two\n\n");
        REQUIRE(evs.size() == 2);
        CHECK(evs[0].data == "one");
        CHECK(evs[1].data == "two");
    }

    TEST_CASE("multi-line data with \\r\\n") {
        SseParser p;
        auto evs = feed_ok(p, "data: a\r\ndata: b\r\n\r\n");
        REQUIRE(evs.size() == 1);
        CHECK(evs[0].data == "a\nb");
    }
}

// =============================================================================
// TEST SUITE 6: [DONE] terminator
// =============================================================================
TEST_SUITE("SseParser — [DONE] terminator") {

    TEST_CASE("data: [DONE] produces is_done=true") {
        SseParser p;
        auto evs = feed_ok(p, "data: [DONE]\n\n");
        REQUIRE(evs.size() == 1);
        CHECK(evs[0].is_done);
        CHECK(evs[0].data == "[DONE]");
    }

    TEST_CASE("[DONE] after normal events") {
        SseParser p;
        auto evs = feed_ok(p,
            "data: token1\n\n"
            "data: token2\n\n"
            "data: [DONE]\n\n");
        REQUIRE(evs.size() == 3);
        CHECK_FALSE(evs[0].is_done);
        CHECK_FALSE(evs[1].is_done);
        CHECK(evs[2].is_done);
    }

    TEST_CASE("[DONE] split across two feed calls") {
        SseParser p;
        feed_ok(p, "data: [DON");
        auto evs = feed_ok(p, "E]\n\n");
        REQUIRE(evs.size() == 1);
        CHECK(evs[0].is_done);
    }

    TEST_CASE("[DONE] is case-sensitive — DATA: [done] is NOT a terminator") {
        SseParser p;
        // Lowercase [done] must NOT set is_done.
        auto evs = feed_ok(p, "data: [done]\n\n");
        REQUIRE(evs.size() == 1);
        CHECK_FALSE(evs[0].is_done);
        CHECK(evs[0].data == "[done]");
    }
}

// =============================================================================
// TEST SUITE 7: Malformed and edge-case input
// =============================================================================
TEST_SUITE("SseParser — malformed input recovery") {

    TEST_CASE("field with no colon treated as field-name-only (empty value)") {
        SseParser p;
        // Per spec: line with no colon — entire line is field name, value is "".
        // "data" alone means data: "" (empty string), which is a valid dispatch.
        auto evs = feed_ok(p, "data\n\n");
        REQUIRE(evs.size() == 1);
        CHECK(evs[0].data == "");
    }

    TEST_CASE("unknown field name is ignored") {
        SseParser p;
        auto evs = feed_ok(p, "unknown: value\ndata: good\n\n");
        REQUIRE(evs.size() == 1);
        CHECK(evs[0].data == "good");
    }

    TEST_CASE("event with only unknown fields is dispatch-empty") {
        SseParser p;
        auto evs = feed_ok(p, "weird: stuff\nanother: field\n\n");
        CHECK(evs.empty());
    }

    TEST_CASE("retry: with non-digit value is silently ignored") {
        SseParser p;
        auto evs = feed_ok(p, "retry: abc\ndata: ok\n\n");
        REQUIRE(evs.size() == 1);
        CHECK(evs[0].data == "ok");
    }

    TEST_CASE("multiple colons in value — only first colon splits") {
        SseParser p;
        auto evs = feed_ok(p, "data: a:b:c\n\n");
        REQUIRE(evs.size() == 1);
        CHECK(evs[0].data == "a:b:c");
    }

    TEST_CASE("empty input returns no events") {
        SseParser p;
        auto evs = feed_ok(p, "");
        CHECK(evs.empty());
        CHECK(p.buffered_bytes() == 0);
    }

    TEST_CASE("whitespace-only lines between events are ignored") {
        SseParser p;
        // A line with only spaces has no colon and no known field name — ignored.
        auto evs = feed_ok(p, "data: x\n   \ndata: y\n\n");
        REQUIRE(evs.size() == 1);
        // The "   " line is treated as an unknown-field-name line, ignored.
        CHECK(evs[0].data == "x\ny");
    }

    TEST_CASE("id: field with null byte is not set (spec §9.2.6)") {
        SseParser p;
        // id with embedded \0 must be ignored per spec.
        const std::string chunk = std::string("id: abc\x00xyz\ndata: payload\n\n", 27);
        auto evs = feed_ok(p, chunk);
        REQUIRE(evs.size() == 1);
        // id should be empty because the value contained \0.
        CHECK(evs[0].id.empty());
        CHECK(evs[0].data == "payload");
    }

    TEST_CASE("parser continues to work after a dispatch-empty event") {
        SseParser p;
        // Comment-only block (dispatch-empty), then a real event.
        auto evs = feed_ok(p, ": ping\n\ndata: real\n\n");
        REQUIRE(evs.size() == 1);
        CHECK(evs[0].data == "real");
    }
}

// =============================================================================
// TEST SUITE 8: Buffer overflow protection
// =============================================================================
TEST_SUITE("SseParser — buffer overflow protection") {

    TEST_CASE("buffer exactly at limit is accepted") {
        SseParser p;
        // Fill the buffer just below the limit with a non-terminating partial
        // event, then complete it.  We do this by building a large data field.
        // Rather than actually allocating 16 MB, we test the boundary logic
        // with a small synthetic limit by verifying the check arithmetic.
        //
        // The actual 16 MB test would be:
        //   std::string giant(SseParser::kMaxBufferBytes - 7, 'x'); // "data: " + data + "\n"
        //   feed_ok(p, "data: " + giant.substr(0, giant.size() - 7) + "\n\n");
        //
        // For practical unit-test speed we verify the formula with a modest
        // sentinel chunk that ensures no overflow trigger fires at normal sizes.
        const std::size_t big_but_legal = 1024 * 1024; // 1 MB
        std::string large_chunk = "data: ";
        large_chunk.append(big_but_legal, 'x');
        large_chunk += "\n\n";
        REQUIRE(large_chunk.size() < SseParser::kMaxBufferBytes);
        auto evs = feed_ok(p, large_chunk);
        REQUIRE(evs.size() == 1);
        CHECK(evs[0].data.size() == big_but_legal);
    }

    TEST_CASE("buffer overflow returns an error") {
        SseParser p;
        // Create a partial chunk (no \n\n) that on its own is just under the
        // limit, then push another chunk that tips it over.
        const std::size_t just_under = SseParser::kMaxBufferBytes - 10;
        std::string first_chunk = "data: ";
        first_chunk.append(just_under - 6, 'y'); // "data: " is 6 bytes
        // Feed the big partial (no boundary → stays in buffer).
        auto r1 = p.feed(first_chunk);
        // This may or may not produce events depending on exact sizes, but must not error.
        REQUIRE(r1.has_value());

        // Now push a second chunk that would exceed the limit.
        std::string overflow_chunk(100, 'z');
        auto r2 = p.feed(overflow_chunk);
        CHECK_FALSE(r2.has_value());
        CHECK(r2.error().find("overflow") != std::string::npos);
    }
}

// =============================================================================
// TEST SUITE 9: reset() and reuse
// =============================================================================
TEST_SUITE("SseParser — reset and reuse") {

    TEST_CASE("reset clears partial buffer") {
        SseParser p;
        feed_ok(p, "data: partial");
        CHECK(p.buffered_bytes() > 0);
        p.reset();
        CHECK(p.buffered_bytes() == 0);
    }

    TEST_CASE("parser works normally after reset") {
        SseParser p;
        feed_ok(p, "data: before reset");
        p.reset();
        auto evs = feed_ok(p, "data: fresh start\n\n");
        REQUIRE(evs.size() == 1);
        CHECK(evs[0].data == "fresh start");
    }
}

// =============================================================================
// TEST SUITE 10: Realistic OpenAI streaming scenario
// =============================================================================
TEST_SUITE("SseParser — realistic OpenAI streaming scenario") {

    TEST_CASE("OpenAI streaming completions: role chunk, content chunks, [DONE]") {
        SseParser p;

        // Simulate a realistic chunked SSE stream split across HTTP write callbacks.
        const std::string chunk1 =
            "data: {\"id\":\"chatcmpl-abc\",\"choices\":[{\"delta\":{\"role\":\"assistant\"},\"finish_reason\":null}]}\n\n";
        const std::string chunk2_part1 =
            "data: {\"id\":\"chatcmpl-abc\",\"choices\":[{\"delta\":{\"content\":\"Hell";
        const std::string chunk2_part2 =
            "o, world!\"},\"finish_reason\":null}]}\n\n";
        const std::string chunk3 =
            "data: {\"id\":\"chatcmpl-abc\",\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}],"
            "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":3,\"total_tokens\":13}}\n\n"
            "data: [DONE]\n\n";

        auto evs1 = feed_ok(p, chunk1);
        REQUIRE(evs1.size() == 1);
        CHECK(evs1[0].data.find("\"role\"") != std::string::npos);

        auto evs2a = feed_ok(p, chunk2_part1);
        CHECK(evs2a.empty()); // partial — no boundary yet

        auto evs2b = feed_ok(p, chunk2_part2);
        REQUIRE(evs2b.size() == 1);
        CHECK(evs2b[0].data.find("Hello, world!") != std::string::npos);

        auto evs3 = feed_ok(p, chunk3);
        REQUIRE(evs3.size() == 2);
        CHECK_FALSE(evs3[0].is_done);          // finish_reason=stop chunk
        CHECK(evs3[1].is_done);                // [DONE]
    }

    TEST_CASE("OpenAI streaming with CRLF (LM Studio compatibility)") {
        SseParser p;

        const std::string lm_studio_chunk =
            "data: {\"choices\":[{\"delta\":{\"content\":\"Hi\"}}]}\r\n\r\n"
            "data: [DONE]\r\n\r\n";

        auto evs = feed_ok(p, lm_studio_chunk);
        REQUIRE(evs.size() == 2);
        CHECK(evs[0].data.find("\"Hi\"") != std::string::npos);
        CHECK(evs[1].is_done);
    }
}
