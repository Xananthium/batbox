// tests/unit/test_tool_call_accumulator.cpp
// =============================================================================
// doctest suite for batbox::inference::ToolCallAccumulator.
//
// Covers:
//   1. Single tool call across multiple SSE chunks — merged args + correct id/name
//   2. Two parallel tool calls (different indices) — both returned, index-ordered
//   3. Malformed args JSON — per-call error, other calls intact
//   4. set-once semantics for id and name (later deltas do not overwrite)
//   5. Empty arguments buffer — treated as empty-object, not a parse error
//   6. Name/id absent in all fragments — empty strings in output (not a crash)
//   7. arguments_fragment absent (nullopt) in a delta — no append, no crash
//
// Build standalone (from repo root, after vcpkg install):
//   c++ -std=c++20 -I include \
//       tests/unit/test_tool_call_accumulator.cpp \
//       src/inference/ToolCallAccumulator.cpp \
//       src/inference/ChatRequest.cpp \
//       src/core/Logging.cpp \
//       -I build/vcpkg_installed/arm64-osx/include \
//       -L build/vcpkg_installed/arm64-osx/lib \
//       -lspdlog -lfmt -lsimdjson \
//       -o /tmp/test_tca && /tmp/test_tca
//
// Via CMake + CTest:
//   cmake --build build && ctest --test-dir build -R test_tool_call_accumulator -V
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/inference/ToolCallAccumulator.hpp>
#include <batbox/inference/ChatResponse.hpp>

#include <string>
#include <vector>

using batbox::inference::ToolCallAccumulator;
using batbox::inference::ToolCallDelta;
using batbox::inference::ToolCall;

// =============================================================================
// Helpers
// =============================================================================

/// Build a ToolCallDelta with all fields set.
static ToolCallDelta make_delta(int index,
                                 std::optional<std::string> id,
                                 std::optional<std::string> name,
                                 std::optional<std::string> args_fragment) {
    ToolCallDelta d;
    d.index               = index;
    d.id                  = std::move(id);
    d.name                = std::move(name);
    d.arguments_fragment  = std::move(args_fragment);
    return d;
}

/// Build a delta carrying only an arguments fragment (typical middle/late chunk).
static ToolCallDelta args_chunk(int index, std::string fragment) {
    return make_delta(index, std::nullopt, std::nullopt, std::move(fragment));
}

// =============================================================================
// TEST SUITE 1: Single tool call accumulated across multiple SSE events
// =============================================================================
TEST_SUITE("ToolCallAccumulator — single tool call") {

    TEST_CASE("5 argument fragments merge into one call") {
        // Simulates: id + name arrive on chunk 1, then 4 more argument fragments.
        ToolCallAccumulator acc;

        acc.accumulate(make_delta(0, "call_abc", "get_weather", std::string("")));
        acc.accumulate(args_chunk(0, "{\"loc"));
        acc.accumulate(args_chunk(0, "ati"));
        acc.accumulate(args_chunk(0, "on\""));
        acc.accumulate(args_chunk(0, ":\"Paris\"}"));

        auto result = acc.finalize();
        REQUIRE(result.has_value());
        REQUIRE(result.value().size() == 1);

        const ToolCall& call = result.value()[0];
        CHECK(call.id   == "call_abc");
        CHECK(call.name == "get_weather");
        CHECK(call.parse_error.empty());
        REQUIRE(!call.arguments.is_null());
        CHECK(call.arguments.at("location") == "Paris");
    }

    TEST_CASE("single-chunk argument (vLLM: entire args in one fragment)") {
        ToolCallAccumulator acc;

        acc.accumulate(make_delta(0, "call_xyz", "list_files",
                                  std::string("{\"path\":\"/tmp\"}")));

        auto result = acc.finalize();
        REQUIRE(result.has_value());
        REQUIRE(result.value().size() == 1);

        const ToolCall& call = result.value()[0];
        CHECK(call.id   == "call_xyz");
        CHECK(call.name == "list_files");
        CHECK(call.parse_error.empty());
        CHECK(call.arguments.at("path") == "/tmp");
    }

    TEST_CASE("finalize with no accumulated deltas returns empty vector") {
        ToolCallAccumulator acc;
        auto result = acc.finalize();
        REQUIRE(result.has_value());
        CHECK(result.value().empty());
    }
}

// =============================================================================
// TEST SUITE 2: Two parallel tool calls (different indices)
// =============================================================================
TEST_SUITE("ToolCallAccumulator — parallel tool calls") {

    TEST_CASE("two calls at different indices returned in ascending index order") {
        ToolCallAccumulator acc;

        // Index 1 arrives first (out of order) to prove sorting.
        acc.accumulate(make_delta(1, "call_B", "write_file",
                                  std::string("{\"path\":\"/out.txt\",")));
        acc.accumulate(make_delta(0, "call_A", "read_file",
                                  std::string("{\"path\":\"/in.txt\"}")));
        acc.accumulate(args_chunk(1, "\"content\":\"hello\"}"));

        auto result = acc.finalize();
        REQUIRE(result.has_value());
        REQUIRE(result.value().size() == 2);

        // Index 0 should be first.
        CHECK(result.value()[0].id   == "call_A");
        CHECK(result.value()[0].name == "read_file");
        CHECK(result.value()[0].arguments.at("path") == "/in.txt");
        CHECK(result.value()[0].parse_error.empty());

        // Index 1 should be second.
        CHECK(result.value()[1].id   == "call_B");
        CHECK(result.value()[1].name == "write_file");
        CHECK(result.value()[1].arguments.at("path")    == "/out.txt");
        CHECK(result.value()[1].arguments.at("content") == "hello");
        CHECK(result.value()[1].parse_error.empty());
    }

    TEST_CASE("three parallel calls with interleaved fragments") {
        ToolCallAccumulator acc;

        acc.accumulate(make_delta(0, "c0", "tool_a", std::string("{\"x\":")));
        acc.accumulate(make_delta(1, "c1", "tool_b", std::string("{\"y\":")));
        acc.accumulate(make_delta(2, "c2", "tool_c", std::string("{\"z\":")));
        acc.accumulate(args_chunk(0, "1}"));
        acc.accumulate(args_chunk(2, "3}"));
        acc.accumulate(args_chunk(1, "2}"));

        auto result = acc.finalize();
        REQUIRE(result.has_value());
        REQUIRE(result.value().size() == 3);

        CHECK(result.value()[0].arguments.at("x") == 1);
        CHECK(result.value()[1].arguments.at("y") == 2);
        CHECK(result.value()[2].arguments.at("z") == 3);
    }
}

// =============================================================================
// TEST SUITE 3: Malformed arguments JSON — per-call error
// =============================================================================
TEST_SUITE("ToolCallAccumulator — malformed JSON handling") {

    TEST_CASE("one malformed call does not prevent other calls from succeeding") {
        ToolCallAccumulator acc;

        // Index 0: valid JSON.
        acc.accumulate(make_delta(0, "c0", "good_tool",
                                  std::string("{\"k\":\"v\"}")));
        // Index 1: deliberately broken JSON.
        acc.accumulate(make_delta(1, "c1", "bad_tool",
                                  std::string("{broken json}")));

        auto result = acc.finalize();
        REQUIRE(result.has_value());
        REQUIRE(result.value().size() == 2);

        // Index 0 must succeed.
        CHECK(result.value()[0].parse_error.empty());
        CHECK(result.value()[0].arguments.at("k") == "v");

        // Index 1 must carry the error.
        CHECK_FALSE(result.value()[1].parse_error.empty());
        CHECK(result.value()[1].arguments.is_null());
    }

    TEST_CASE("all calls malformed — each has parse_error set") {
        ToolCallAccumulator acc;

        acc.accumulate(make_delta(0, "c0", "t0", std::string("not json")));
        acc.accumulate(make_delta(1, "c1", "t1", std::string("{unclosed")));

        auto result = acc.finalize();
        REQUIRE(result.has_value());
        REQUIRE(result.value().size() == 2);

        CHECK_FALSE(result.value()[0].parse_error.empty());
        CHECK_FALSE(result.value()[1].parse_error.empty());
    }

    TEST_CASE("truncated JSON from a dropped final fragment is a parse error") {
        ToolCallAccumulator acc;

        acc.accumulate(make_delta(0, "c0", "tool", std::string("{\"key\":")));
        // Final fragment never arrives — arguments_buf is "{\"key\":"

        auto result = acc.finalize();
        REQUIRE(result.has_value());
        REQUIRE(result.value().size() == 1);
        CHECK_FALSE(result.value()[0].parse_error.empty());
    }
}

// =============================================================================
// TEST SUITE 4: set-once semantics for id and name
// =============================================================================
TEST_SUITE("ToolCallAccumulator — set-once id/name") {

    TEST_CASE("later delta with non-empty id does not overwrite first") {
        ToolCallAccumulator acc;

        // First delta sets id and name.
        acc.accumulate(make_delta(0, "original_id", "original_name",
                                  std::string("{\"a\":")));
        // Later delta also carries id/name (malformed stream or vLLM quirk).
        acc.accumulate(make_delta(0, "overwrite_id", "overwrite_name",
                                  std::string("1}")));

        auto result = acc.finalize();
        REQUIRE(result.has_value());
        REQUIRE(result.value().size() == 1);

        // First writer wins.
        CHECK(result.value()[0].id   == "original_id");
        CHECK(result.value()[0].name == "original_name");
        CHECK(result.value()[0].arguments.at("a") == 1);
    }

    TEST_CASE("id arrives late (second delta) because first carried empty id") {
        ToolCallAccumulator acc;

        // First delta has no id (nullopt) — name only.
        acc.accumulate(make_delta(0, std::nullopt, "my_tool",
                                  std::string("{\"n\":")));
        // Second delta delivers the id.
        acc.accumulate(make_delta(0, "late_id", std::nullopt,
                                  std::string("99}")));

        auto result = acc.finalize();
        REQUIRE(result.has_value());
        REQUIRE(result.value().size() == 1);

        CHECK(result.value()[0].id   == "late_id");
        CHECK(result.value()[0].name == "my_tool");
        CHECK(result.value()[0].arguments.at("n") == 99);
    }

    TEST_CASE("name never arrives — empty string in output, no crash") {
        ToolCallAccumulator acc;

        acc.accumulate(make_delta(0, "c0", std::nullopt,
                                  std::string("{\"x\":true}")));

        auto result = acc.finalize();
        REQUIRE(result.has_value());
        REQUIRE(result.value().size() == 1);

        CHECK(result.value()[0].id   == "c0");
        CHECK(result.value()[0].name.empty());
        CHECK(result.value()[0].parse_error.empty());
        CHECK(result.value()[0].arguments.at("x") == true);
    }

    TEST_CASE("id and name both never arrive — empty strings, valid args") {
        ToolCallAccumulator acc;

        acc.accumulate(make_delta(0, std::nullopt, std::nullopt,
                                  std::string("{\"y\":42}")));

        auto result = acc.finalize();
        REQUIRE(result.has_value());
        REQUIRE(result.value().size() == 1);

        CHECK(result.value()[0].id.empty());
        CHECK(result.value()[0].name.empty());
        CHECK(result.value()[0].parse_error.empty());
        CHECK(result.value()[0].arguments.at("y") == 42);
    }
}

// =============================================================================
// TEST SUITE 5: Empty / absent arguments_fragment
// =============================================================================
TEST_SUITE("ToolCallAccumulator — empty and absent argument fragments") {

    TEST_CASE("first delta has empty string fragment — treated as empty-object") {
        // Tools with no parameters send an empty arguments string.
        ToolCallAccumulator acc;

        acc.accumulate(make_delta(0, "c0", "no_params_tool", std::string("")));
        // No further argument fragments.

        auto result = acc.finalize();
        REQUIRE(result.has_value());
        REQUIRE(result.value().size() == 1);

        // Empty buffer → empty JSON object, no parse error.
        CHECK(result.value()[0].parse_error.empty());
        REQUIRE(result.value()[0].arguments.is_object());
        CHECK(result.value()[0].arguments.empty());
    }

    TEST_CASE("delta with nullopt arguments_fragment is accepted (no append)") {
        ToolCallAccumulator acc;

        // Some providers skip the arguments_fragment on the first delta entirely.
        acc.accumulate(make_delta(0, "c0", "ping", std::nullopt));
        acc.accumulate(args_chunk(0, "{}"));

        auto result = acc.finalize();
        REQUIRE(result.has_value());
        REQUIRE(result.value().size() == 1);

        CHECK(result.value()[0].parse_error.empty());
        REQUIRE(result.value()[0].arguments.is_object());
    }

    TEST_CASE("multiple empty-string fragments accumulate to empty buffer") {
        ToolCallAccumulator acc;

        acc.accumulate(make_delta(0, "c0", "tool", std::string("")));
        acc.accumulate(args_chunk(0, ""));
        acc.accumulate(args_chunk(0, ""));

        auto result = acc.finalize();
        REQUIRE(result.has_value());
        REQUIRE(result.value().size() == 1);

        CHECK(result.value()[0].parse_error.empty());
        CHECK(result.value()[0].arguments.is_object()); // empty-buffer → {}
    }
}

// =============================================================================
// TEST SUITE 6: Realistic OpenAI streaming tool_calls scenario
// =============================================================================
TEST_SUITE("ToolCallAccumulator — realistic OpenAI scenario") {

    TEST_CASE("weather query: role chunk + 5 tool_calls chunks + finish") {
        // Mirrors actual OpenAI streaming output for get_current_weather.
        // Chunk layout:
        //   chunk 1: index=0, id="call_abc123", name="get_current_weather", args=""
        //   chunk 2: index=0, args="{\"location\":"
        //   chunk 3: index=0, args="\" San Francisco,"
        //   chunk 4: index=0, args=" CA\", \"unit\":"
        //   chunk 5: index=0, args="\"celsius\"}"
        //   finish_reason="tool_calls" → finalize()

        ToolCallAccumulator acc;

        acc.accumulate(make_delta(0, "call_abc123", "get_current_weather",
                                  std::string("")));
        acc.accumulate(args_chunk(0, "{\"location\":"));
        acc.accumulate(args_chunk(0, "\" San Francisco,"));
        acc.accumulate(args_chunk(0, " CA\", \"unit\":"));
        acc.accumulate(args_chunk(0, "\"celsius\"}"));

        auto result = acc.finalize();
        REQUIRE(result.has_value());
        REQUIRE(result.value().size() == 1);

        const ToolCall& call = result.value()[0];
        CHECK(call.id   == "call_abc123");
        CHECK(call.name == "get_current_weather");
        CHECK(call.parse_error.empty());

        // The assembled JSON should parse correctly.
        REQUIRE(!call.arguments.is_null());
        CHECK(call.arguments.at("location").get<std::string>().find("San Francisco") != std::string::npos);
        CHECK(call.arguments.at("unit") == "celsius");
    }

    TEST_CASE("two simultaneous tool calls in one response") {
        // Model calls search_web AND read_url in parallel.
        ToolCallAccumulator acc;

        acc.accumulate(make_delta(0, "call_s1", "search_web",
                                  std::string("{\"query\":")));
        acc.accumulate(make_delta(1, "call_r1", "read_url",
                                  std::string("{\"url\":")));
        acc.accumulate(args_chunk(0, "\"batbox C++\"}"));
        acc.accumulate(args_chunk(1, "\"https://example.com\"}"));

        auto result = acc.finalize();
        REQUIRE(result.has_value());
        REQUIRE(result.value().size() == 2);

        CHECK(result.value()[0].name == "search_web");
        CHECK(result.value()[0].arguments.at("query") == "batbox C++");

        CHECK(result.value()[1].name == "read_url");
        CHECK(result.value()[1].arguments.at("url") == "https://example.com");
    }
}
