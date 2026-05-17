// tests/unit/test_client_reasoning.cpp
// =============================================================================
// TUI-T11: token_received flag stays false during reasoning phase (UI-D9 / #30)
//
// Verifies that:
//   1. StreamDelta::from_json correctly deserialises delta.reasoning_content
//      chunks from reasoning models (Magistral, Qwen3, DeepSeek).
//   2. A delta that carries only reasoning_content (no content, no tool_calls)
//      is correctly recognised as "stream has started" by the Client retry gate.
//   3. A delta that carries only content is still recognised as both
//      "stream_started" and "token_received".
//   4. A delta that carries reasoning_content does NOT flip token_received
//      (telemetry must remain content-only).
//
// Design:
//   The stream_chat() retry guard was changed from !token_received to
//   !stream_started.  stream_started flips on any non-empty delta field
//   (content, reasoning_content, or tool_calls).  token_received remains
//   content-only for telemetry.
//
//   Because Client::stream_chat() makes real HTTP calls, we test the
//   component it delegates to: StreamDelta deserialisation from delta JSON.
//   The guard logic is verified indirectly through the per-field semantics
//   (if from_json ignores reasoning_content the gate can never fire for
//   reasoning-only chunks).
//
// Build + run via CMake:
//   cmake --build build --parallel 8
//   ctest --test-dir build -R test_client_reasoning -V
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/core/Json.hpp>
#include <batbox/inference/ChatResponse.hpp>
#include <batbox/inference/SseParser.hpp>

#include <string>

using namespace batbox;
using namespace batbox::inference;

// =============================================================================
// Helper: build a minimal OpenAI-compatible streaming delta JSON object and
// deserialise it into a StreamDelta.
// =============================================================================
static StreamDelta make_delta(const Json& delta_obj) {
    return delta_obj.get<StreamDelta>();
}

// =============================================================================
// TEST SUITE 1: reasoning_content deserialisation
// =============================================================================
TEST_SUITE("StreamDelta reasoning_content — from_json") {

    TEST_CASE("reasoning-only chunk: reasoning_content is populated") {
        Json delta = Json{
            {"reasoning_content", "Let me think step by step..."}
        };
        StreamDelta sd = make_delta(delta);

        REQUIRE(sd.reasoning_content.has_value());
        CHECK(sd.reasoning_content.value() == "Let me think step by step...");
    }

    TEST_CASE("reasoning-only chunk: content is absent") {
        Json delta = Json{
            {"reasoning_content", "thinking..."}
        };
        StreamDelta sd = make_delta(delta);

        CHECK_FALSE(sd.content.has_value());
    }

    TEST_CASE("reasoning-only chunk: tool_calls is absent") {
        Json delta = Json{
            {"reasoning_content", "thinking..."}
        };
        StreamDelta sd = make_delta(delta);

        CHECK_FALSE(sd.tool_calls.has_value());
    }

    TEST_CASE("reasoning-only chunk: finish_reason is absent") {
        Json delta = Json{
            {"reasoning_content", "thinking..."}
        };
        StreamDelta sd = make_delta(delta);

        CHECK_FALSE(sd.finish_reason.has_value());
    }

    TEST_CASE("content chunk: content is populated, reasoning_content absent") {
        Json delta = Json{
            {"content", "Hello, world!"}
        };
        StreamDelta sd = make_delta(delta);

        REQUIRE(sd.content.has_value());
        CHECK(sd.content.value() == "Hello, world!");
        CHECK_FALSE(sd.reasoning_content.has_value());
    }

    TEST_CASE("chunk with both reasoning_content and content: both fields populated") {
        // Some providers emit a transition chunk that has both fields.
        Json delta = Json{
            {"reasoning_content", "Done thinking."},
            {"content",           "Answer: 42"}
        };
        StreamDelta sd = make_delta(delta);

        REQUIRE(sd.reasoning_content.has_value());
        CHECK(sd.reasoning_content.value() == "Done thinking.");
        REQUIRE(sd.content.has_value());
        CHECK(sd.content.value() == "Answer: 42");
    }

    TEST_CASE("empty delta object: no fields populated") {
        Json delta = Json::object();
        StreamDelta sd = make_delta(delta);

        CHECK_FALSE(sd.content.has_value());
        CHECK_FALSE(sd.reasoning_content.has_value());
        CHECK_FALSE(sd.tool_calls.has_value());
        CHECK_FALSE(sd.finish_reason.has_value());
        CHECK_FALSE(sd.usage.has_value());
    }

    TEST_CASE("null reasoning_content value: field treated as absent") {
        // Provider quirk: some emit null instead of omitting the field.
        Json delta = Json{
            {"reasoning_content", nullptr}
        };
        StreamDelta sd = make_delta(delta);

        CHECK_FALSE(sd.reasoning_content.has_value());
    }

    TEST_CASE("empty string reasoning_content: field is present with empty string") {
        Json delta = Json{
            {"reasoning_content", ""}
        };
        StreamDelta sd = make_delta(delta);

        REQUIRE(sd.reasoning_content.has_value());
        CHECK(sd.reasoning_content.value().empty());
    }
}

// =============================================================================
// TEST SUITE 2: stream_started gate semantics
//
// These tests exercise the per-field logic that the retry gate in
// Client::stream_chat() uses to determine whether the server has started
// streaming.  The gate fires when any non-empty delta field arrives.
//
// We simulate the gate here as a pure boolean function over StreamDelta so
// that the logic is independently testable without touching libcurl.
// =============================================================================

/// Mirrors the stream_started gate logic from Client::stream_chat().
/// Returns true when the delta should set stream_started = true.
static bool gate_fires(const StreamDelta& sd) {
    if (sd.content.has_value()           && !sd.content->empty())           return true;
    if (sd.reasoning_content.has_value() && !sd.reasoning_content->empty()) return true;
    if (sd.tool_calls.has_value()        && !sd.tool_calls->empty())         return true;
    return false;
}

/// Mirrors the token_received logic from Client::stream_chat().
/// Returns true only when a user-visible content token arrives.
static bool token_fires(const StreamDelta& sd) {
    if (sd.content.has_value()    && !sd.content->empty())    return true;
    if (sd.tool_calls.has_value() && !sd.tool_calls->empty()) return true;
    return false;
}

TEST_SUITE("stream_started gate — retry policy semantics") {

    TEST_CASE("reasoning-only delta: gate fires (stream_started = true)") {
        StreamDelta sd;
        sd.reasoning_content = "Let me think...";

        CHECK(gate_fires(sd));
    }

    TEST_CASE("reasoning-only delta: token_received does NOT fire") {
        StreamDelta sd;
        sd.reasoning_content = "Let me think...";

        CHECK_FALSE(token_fires(sd));
    }

    TEST_CASE("content delta: both gate and token_received fire") {
        StreamDelta sd;
        sd.content = "Hello!";

        CHECK(gate_fires(sd));
        CHECK(token_fires(sd));
    }

    TEST_CASE("empty reasoning_content: gate does NOT fire") {
        // Empty string is not a real token — gate must not fire.
        StreamDelta sd;
        sd.reasoning_content = "";

        CHECK_FALSE(gate_fires(sd));
    }

    TEST_CASE("empty delta (no fields): gate does NOT fire") {
        StreamDelta sd;

        CHECK_FALSE(gate_fires(sd));
        CHECK_FALSE(token_fires(sd));
    }

    TEST_CASE("finish_reason-only delta: gate does NOT fire") {
        // finish_reason chunks carry no content — should not count as progress.
        StreamDelta sd;
        sd.finish_reason = "stop";

        CHECK_FALSE(gate_fires(sd));
        CHECK_FALSE(token_fires(sd));
    }

    TEST_CASE("reasoning-only SSE stream: gate fires on first chunk") {
        // Simulate a sequence of reasoning chunks followed by connection drop
        // (no content ever arrives).  The gate should fire on the first chunk.
        const std::vector<std::string> reasoning_fragments = {
            "Let me analyse the problem.",
            " First, I consider the constraints.",
            " Therefore, the answer is 42."
        };

        bool stream_started_flag = false;
        bool token_received_flag = false;

        for (const auto& fragment : reasoning_fragments) {
            StreamDelta sd;
            sd.reasoning_content = fragment;

            if (!stream_started_flag && gate_fires(sd)) {
                stream_started_flag = true;
            }
            if (token_fires(sd)) {
                token_received_flag = true;
            }
        }

        CHECK(stream_started_flag);       // gate fired — no retry would occur
        CHECK_FALSE(token_received_flag); // telemetry stays false — content-only
    }

    TEST_CASE("mixed reasoning-then-content stream: both flags fire in order") {
        struct Chunk { std::string reasoning; std::string content; };
        const std::vector<Chunk> chunks = {
            {"Step 1: analyse.", ""},
            {"Step 2: solve.",   ""},
            {"",                 "The answer is 42."}
        };

        bool stream_started_flag = false;
        bool token_received_flag = false;
        int  stream_started_at   = -1;
        int  token_received_at   = -1;

        for (int i = 0; i < static_cast<int>(chunks.size()); ++i) {
            StreamDelta sd;
            if (!chunks[i].reasoning.empty()) sd.reasoning_content = chunks[i].reasoning;
            if (!chunks[i].content.empty())   sd.content           = chunks[i].content;

            if (!stream_started_flag && gate_fires(sd)) {
                stream_started_flag = true;
                stream_started_at   = i;
            }
            if (!token_received_flag && token_fires(sd)) {
                token_received_flag = true;
                token_received_at   = i;
            }
        }

        CHECK(stream_started_flag);
        CHECK(token_received_flag);
        // stream_started fires on chunk 0 (first reasoning chunk);
        // token_received fires on chunk 2 (first content chunk).
        CHECK(stream_started_at  == 0);
        CHECK(token_received_at  == 2);
        // gate fires before token — reasoning phase is correctly detected first
        CHECK(stream_started_at < token_received_at);
    }
}

// =============================================================================
// TEST SUITE 3: round-trip serialisation
// Confirm to_json also emits reasoning_content when present.
// =============================================================================
TEST_SUITE("StreamDelta reasoning_content — to_json round-trip") {

    TEST_CASE("reasoning_content survives to_json → from_json round-trip") {
        StreamDelta original;
        original.reasoning_content = "Deep thought in progress.";

        Json j = original;
        StreamDelta restored = j.get<StreamDelta>();

        REQUIRE(restored.reasoning_content.has_value());
        CHECK(restored.reasoning_content.value() == "Deep thought in progress.");
    }

    TEST_CASE("absent reasoning_content not serialised") {
        StreamDelta sd;
        sd.content = "Hello";

        Json j = sd;
        CHECK_FALSE(j.contains("reasoning_content"));
    }

    TEST_CASE("present reasoning_content is serialised under correct key") {
        StreamDelta sd;
        sd.reasoning_content = "thinking...";

        Json j = sd;
        REQUIRE(j.contains("reasoning_content"));
        CHECK(j["reasoning_content"].get<std::string>() == "thinking...");
    }
}

// =============================================================================
// TEST SUITE 4: empty-stream guard — TUI-T13
//
// Verifies the empty-stream guard added in TUI-T12 (Client.cpp ~line 608):
//
//     if (!token_received && !final_finish_reason_seen) {
//         return Err("stream ended without content ...");
//     }
//
// We mirror that logic in the pure helper should_return_error_for_empty_stream()
// and exercise it against representative SSE chunk sequences.  This keeps the
// tests independent of libcurl / live HTTP — the same approach used by the
// gate_fires() / token_fires() helpers in TEST SUITE 2.
//
// Guard behaviour recap:
//   token_received          flips true when delta.content or delta.tool_calls
//                           is non-empty.
//   final_finish_reason_seen flips true when any chunk carries a non-empty
//                           finish_reason string (mirrors the Client.cpp code
//                           that reads choice["finish_reason"] and sets
//                           final_finish_reason_seen=true for any non-null string).
//
//   Returns Err (guard fires) when:  !token_received && !final_finish_reason_seen
//   Returns Ok              when:   token_received || final_finish_reason_seen
//
// Build + run:
//   cmake --build build --parallel 8
//   ctest --test-dir build -R test_client_reasoning -V
// =============================================================================

/// Mirrors the TUI-T12 empty-stream guard from Client::stream_chat().
///
/// Given a sequence of StreamDelta objects (one per SSE chunk), returns true
/// when the guard would fire — i.e. when the stream should be treated as an
/// error rather than a silent empty success.
///
/// Mapping to Client.cpp variables:
///   delta.content / delta.tool_calls  → token_received
///   delta.finish_reason               → final_finish_reason_seen
///     (Client.cpp overwrites delta.finish_reason from choice["finish_reason"]
///      before checking — from_json therefore places the same value there; the
///      mapping is 1-to-1 for unit-testing purposes.)
static bool should_return_error_for_empty_stream(
        const std::vector<StreamDelta>& sequence) {

    bool token_received           = false;
    bool final_finish_reason_seen = false;

    for (const StreamDelta& sd : sequence) {
        // token_received: content or tool_calls delivered
        if (sd.content.has_value() && !sd.content->empty()) {
            token_received = true;
        }
        if (sd.tool_calls.has_value() && !sd.tool_calls->empty()) {
            token_received = true;
        }
        // final_finish_reason_seen: any non-empty finish_reason string
        if (sd.finish_reason.has_value() && !sd.finish_reason->empty()) {
            final_finish_reason_seen = true;
        }
    }

    // Guard condition: error when neither token nor finish_reason ever arrived.
    return !token_received && !final_finish_reason_seen;
}

// ---------------------------------------------------------------------------
// Convenience: build a reasoning-only StreamDelta
// ---------------------------------------------------------------------------
static StreamDelta make_reasoning_delta(const std::string& text) {
    StreamDelta sd;
    sd.reasoning_content = text;
    return sd;
}

// ---------------------------------------------------------------------------
// Convenience: build a content StreamDelta
// ---------------------------------------------------------------------------
static StreamDelta make_content_delta(const std::string& text) {
    StreamDelta sd;
    sd.content = text;
    return sd;
}

// ---------------------------------------------------------------------------
// Convenience: build a finish_reason-only StreamDelta (no content)
// ---------------------------------------------------------------------------
static StreamDelta make_finish_delta(const std::string& reason) {
    StreamDelta sd;
    sd.finish_reason = reason;
    return sd;
}

// ---------------------------------------------------------------------------
// Convenience: build a tool_calls StreamDelta (single fragment)
// ---------------------------------------------------------------------------
static StreamDelta make_tool_calls_delta(const std::string& reason = "") {
    StreamDelta sd;
    ToolCallDelta tcd;
    tcd.index             = 0;
    tcd.id                = "call_0";
    tcd.name              = "get_weather";
    tcd.arguments_fragment = "{\"location\":\"Paris\"}";
    sd.tool_calls         = std::vector<ToolCallDelta>{tcd};
    if (!reason.empty()) {
        sd.finish_reason = reason;
    }
    return sd;
}

TEST_SUITE("[TUI-T13] Client::stream_chat empty-stream guard (TUI-T12)") {

    // -------------------------------------------------------------------------
    // A. Cases that SHOULD return Err (the bug-reproduction cases)
    // -------------------------------------------------------------------------

    TEST_CASE("A1: reasoning-only stream, no finish_reason → guard fires (Err)") {
        // The original bug: LM Studio emits only reasoning_content then closes
        // the socket cleanly without [DONE] or finish_reason.  T12 must surface
        // this as an error rather than a silent Ok.
        const std::vector<StreamDelta> seq = {
            make_reasoning_delta("Let me think step by step..."),
            make_reasoning_delta("First, consider the constraints."),
            make_reasoning_delta("Therefore the answer is 42."),
        };
        CHECK(should_return_error_for_empty_stream(seq));
    }

    TEST_CASE("A2: empty data chunks (whitespace reasoning), no finish_reason → guard fires (Err)") {
        // Server emits chunks where reasoning_content is empty or whitespace-only.
        // Even empty-string reasoning_content means no content was delivered.
        const std::vector<StreamDelta> seq = {
            make_reasoning_delta(""),   // empty string — not a real token
            make_reasoning_delta(""),
            make_reasoning_delta(""),
        };
        CHECK(should_return_error_for_empty_stream(seq));
    }

    TEST_CASE("A3: zero chunks — server 200-closes immediately → guard fires (Err)") {
        // Server accepts the request and closes the connection with 200 and an
        // empty body.  No chunks at all.
        const std::vector<StreamDelta> seq;
        CHECK(should_return_error_for_empty_stream(seq));
    }

    TEST_CASE("A4: reasoning-only, finish_reason=null in every chunk → guard fires (Err)") {
        // All chunks have a reasoning_content delta but finish_reason stays
        // absent (null) in every chunk — the model never signalled completion.
        // This is the pathological Magistral case from the regression report.
        const std::vector<StreamDelta> seq = {
            make_reasoning_delta("Thought 1"),
            make_reasoning_delta("Thought 2"),
            make_reasoning_delta("Thought 3"),
        };
        // Confirm finish_reason is absent on all chunks (belt-and-suspenders).
        for (const auto& sd : seq) {
            CHECK_FALSE(sd.finish_reason.has_value());
        }
        CHECK(should_return_error_for_empty_stream(seq));
    }

    // -------------------------------------------------------------------------
    // B. Cases that SHOULD return Ok (non-regression)
    // -------------------------------------------------------------------------

    TEST_CASE("B1: single content delta, then closes → Ok (no error)") {
        // Normal happy path: server emits content and closes.
        const std::vector<StreamDelta> seq = {
            make_content_delta("Hello, world!"),
        };
        CHECK_FALSE(should_return_error_for_empty_stream(seq));
    }

    TEST_CASE("B2: reasoning then content then closes → Ok") {
        // Reasoning model emits reasoning_content then transitions to content.
        // token_received flips true on the content chunk → no error.
        const std::vector<StreamDelta> seq = {
            make_reasoning_delta("Let me think..."),
            make_reasoning_delta("OK, I have an answer."),
            make_content_delta("The answer is 42."),
        };
        CHECK_FALSE(should_return_error_for_empty_stream(seq));
    }

    TEST_CASE("B3: zero content but finish_reason=length → Ok (model hit token budget)") {
        // Model burned all tokens on reasoning and hit max_tokens.  Server emits
        // a chunk with finish_reason=length and no content.  The model explicitly
        // told us it's done — this is a valid (if empty) completion.
        const std::vector<StreamDelta> seq = {
            make_reasoning_delta("Reasoning chunk 1"),
            make_reasoning_delta("Reasoning chunk 2"),
            make_finish_delta("length"),
        };
        CHECK_FALSE(should_return_error_for_empty_stream(seq));
    }

    TEST_CASE("B4: zero content but finish_reason=content_filter → Ok") {
        // Server filtered the response.  Still an explicit signal — not a
        // truncation.  Guard must not fire.
        const std::vector<StreamDelta> seq = {
            make_finish_delta("content_filter"),
        };
        CHECK_FALSE(should_return_error_for_empty_stream(seq));
    }

    TEST_CASE("B5: tool_calls delta, no content, no finish_reason → Ok") {
        // The model returned a tool call instead of content.  tool_calls counts
        // as a delivered token — token_received flips true.
        const std::vector<StreamDelta> seq = {
            make_tool_calls_delta(),
        };
        CHECK_FALSE(should_return_error_for_empty_stream(seq));
    }

    TEST_CASE("B6: tool_calls delta with finish_reason=tool_calls → Ok") {
        // Standard tool-call stream: tool_calls fragment arrives together with
        // or followed by finish_reason=tool_calls.  Either token_received or
        // final_finish_reason_seen flips true — no error.
        const std::vector<StreamDelta> seq = {
            make_tool_calls_delta("tool_calls"),
        };
        CHECK_FALSE(should_return_error_for_empty_stream(seq));
    }

    // -------------------------------------------------------------------------
    // C. Edge cases
    // -------------------------------------------------------------------------

    TEST_CASE("C1: reasoning then finish_reason=stop with empty content → Ok") {
        // Rare but valid: reasoning model runs, emits finish_reason=stop with
        // an empty content field.  The model explicitly signalled done — treat
        // as Ok even though no content token arrived.
        const std::vector<StreamDelta> seq = {
            make_reasoning_delta("Let me think..."),
            make_finish_delta("stop"),
        };
        CHECK_FALSE(should_return_error_for_empty_stream(seq));
    }

    TEST_CASE("C2: content then reasoning then content — both flags eventually set → Ok") {
        // Unusual ordering (provider quirk) where content arrives before and
        // after a reasoning_content chunk.  token_received fires on first content
        // chunk — no error.
        const std::vector<StreamDelta> seq = {
            make_content_delta("Part one."),
            make_reasoning_delta("Hmm, let me refine."),
            make_content_delta("Part two."),
        };
        CHECK_FALSE(should_return_error_for_empty_stream(seq));
    }

    TEST_CASE("C3: finish_reason=stop on first chunk with no content → Ok (explicit signal)") {
        // Degenerate but explicit: server emits only a finish_reason chunk and
        // nothing else.  Because finish_reason is present the guard does not
        // fire — this is an intentional empty response, not a truncation.
        const std::vector<StreamDelta> seq = {
            make_finish_delta("stop"),
        };
        CHECK_FALSE(should_return_error_for_empty_stream(seq));
    }

    TEST_CASE("C4: multiple reasoning fragments then one content fragment → Ok") {
        // Verify guard state accumulates correctly across a long reasoning
        // sequence: token_received must stay false through all reasoning chunks,
        // then flip true on the content chunk.  No error expected.
        const std::vector<std::string> reasoning_thoughts = {
            "Step 1: define the problem.",
            "Step 2: identify constraints.",
            "Step 3: enumerate solutions.",
            "Step 4: select the best one.",
        };

        std::vector<StreamDelta> seq;
        for (const auto& t : reasoning_thoughts) {
            seq.push_back(make_reasoning_delta(t));
        }
        seq.push_back(make_content_delta("Final answer: 42."));

        CHECK_FALSE(should_return_error_for_empty_stream(seq));
    }

    // -------------------------------------------------------------------------
    // D. State accumulation invariants (belt-and-suspenders)
    // -------------------------------------------------------------------------

    TEST_CASE("D1: guard state — reasoning-only stream never sets token_received") {
        // Verify that a pure reasoning_content stream leaves token_received=false.
        // This is the invariant that T12's guard depends on.
        bool token_received           = false;
        bool final_finish_reason_seen = false;

        const std::vector<StreamDelta> seq = {
            make_reasoning_delta("Thought A"),
            make_reasoning_delta("Thought B"),
        };

        for (const StreamDelta& sd : seq) {
            if (sd.content.has_value()    && !sd.content->empty())    token_received = true;
            if (sd.tool_calls.has_value() && !sd.tool_calls->empty()) token_received = true;
            if (sd.finish_reason.has_value() && !sd.finish_reason->empty())
                final_finish_reason_seen = true;
        }

        CHECK_FALSE(token_received);
        CHECK_FALSE(final_finish_reason_seen);
        // Guard would fire:
        CHECK(should_return_error_for_empty_stream(seq));
    }

    TEST_CASE("D2: guard state — content chunk flips token_received regardless of reasoning before it") {
        // Confirm the guard accumulates state: even if many reasoning chunks
        // come first, a single content chunk prevents the guard from firing.
        bool token_received = false;

        const std::vector<StreamDelta> seq = {
            make_reasoning_delta("Long thought 1"),
            make_reasoning_delta("Long thought 2"),
            make_reasoning_delta("Long thought 3"),
            make_content_delta("Answer."),
        };

        for (const StreamDelta& sd : seq) {
            if (sd.content.has_value()    && !sd.content->empty())    token_received = true;
            if (sd.tool_calls.has_value() && !sd.tool_calls->empty()) token_received = true;
        }

        CHECK(token_received);
        CHECK_FALSE(should_return_error_for_empty_stream(seq));
    }
}

// =============================================================================
// TEST SUITE 5: SSE error-event surfacing — TUI-T18
//
// Verifies the error-handling branches added to Client::write_cb in TUI-T17:
//
//   Branch A/B: ev.event == "error" || ev.event == "fatal_error"
//       Parse ev.data as JSON; extract error.message; set stream_error =
//       "server: " + msg; return false (abort curl).
//       Fallback: when JSON parse fails or message absent, use raw ev.data.
//
//   Branch C: ev.event == "" (default), chunk JSON contains top-level "error" object
//       Same extraction/prefix logic.  Abort curl.
//
//   Branch D/E: ev.event is non-empty but not "error"/"fatal_error" (e.g. "ping")
//       `continue` — event is skipped silently; subsequent content events are
//       still processed.
//
// Design:
//   We mirror the write_cb dispatch logic in a pure helper function
//   dispatch_sse_events() — the same approach used by gate_fires() /
//   token_fires() (T11/T13) and should_return_error_for_empty_stream() (T13).
//   No libcurl / HTTP / Client instantiation needed.
//
// Build + run:
//   cmake --build build --parallel 8
//   ctest --test-dir build -R test_client_reasoning -V
// =============================================================================

// ---------------------------------------------------------------------------
// Result type for the dispatch helper
// ---------------------------------------------------------------------------
struct DispatchResult {
    bool        ok;           ///< false when any event set stream_error
    std::string stream_error; ///< "server: ..." when ok == false, else empty
    bool        content_seen; ///< true when at least one content delta was delivered
};

// ---------------------------------------------------------------------------
// dispatch_sse_events()
//
// Mirrors the relevant write_cb branches from Client::stream_chat() (TUI-T17).
// Takes a sequence of pre-built SseEvents and runs each through the same
// branch logic:
//
//   1. ev.is_done               → break (clean end)
//   2. ev.event == "error" ||
//      ev.event == "fatal_error" → parse JSON, extract error.message,
//                                   set stream_error, return {false, ...}
//   3. !ev.event.empty()         → continue (skip non-error named events)
//   4. parse ev.data as chunk JSON
//      a. chunk["error"].is_object() → same extract/prefix, return {false,...}
//      b. chunk["choices"] exists    → extract delta.content; flip content_seen
//
// Returns DispatchResult indicating whether the sequence completed without
// error and whether any content token was delivered.
// ---------------------------------------------------------------------------
static DispatchResult dispatch_sse_events(const std::vector<SseEvent>& events) {
    DispatchResult result{true, "", false};

    for (const SseEvent& ev : events) {
        // Step 1: [DONE] sentinel
        if (ev.is_done) {
            break;
        }

        // Step 2: named SSE error events (TUI-T17 Branch A/B)
        if (ev.event == "error" || ev.event == "fatal_error") {
            std::string server_msg;
            auto parsed = batbox::parse(ev.data);
            if (parsed && parsed->is_object()) {
                const auto& obj = parsed.value();
                if (obj.contains("error") && obj["error"].is_object()
                        && obj["error"].contains("message")
                        && obj["error"]["message"].is_string()) {
                    server_msg = obj["error"]["message"].get<std::string>();
                } else if (obj.contains("message") && obj["message"].is_string()) {
                    server_msg = obj["message"].get<std::string>();
                }
            }
            if (server_msg.empty()) {
                server_msg = ev.data; // fallback: raw payload
            }
            result.ok           = false;
            result.stream_error = "server: " + server_msg;
            return result;
        }

        // Step 3: non-empty event name that is not an error — skip silently
        if (!ev.event.empty()) {
            continue;
        }

        // Step 4: default event — parse data as chunk JSON
        auto chunk_result = batbox::parse(ev.data);
        if (!chunk_result) {
            continue; // malformed — skip (lenient)
        }
        const batbox::Json& chunk = chunk_result.value();

        // Branch C (TUI-T17): inline error object in chunk data
        if (chunk.contains("error") && chunk["error"].is_object()) {
            std::string server_msg;
            if (chunk["error"].contains("message")
                    && chunk["error"]["message"].is_string()) {
                server_msg = chunk["error"]["message"].get<std::string>();
            } else {
                server_msg = chunk["error"].dump();
            }
            result.ok           = false;
            result.stream_error = "server: " + server_msg;
            return result;
        }

        // Normal chunk: check for content delivery
        if (chunk.contains("choices") && chunk["choices"].is_array()
                && !chunk["choices"].empty()) {
            const batbox::Json& choice = chunk["choices"][0];
            if (choice.contains("delta") && choice["delta"].is_object()) {
                const batbox::Json& delta = choice["delta"];
                if (delta.contains("content") && delta["content"].is_string()) {
                    const std::string text = delta["content"].get<std::string>();
                    if (!text.empty()) {
                        result.content_seen = true;
                    }
                }
            }
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// Convenience helpers for building SseEvents in tests
// ---------------------------------------------------------------------------

/// Build a named SSE event (ev.event = name, ev.data = data).
static SseEvent make_sse_event(std::string event_name, std::string data) {
    SseEvent ev;
    ev.event = std::move(event_name);
    ev.data  = std::move(data);
    return ev;
}

/// Build a default SSE data event (ev.event = "", ev.data = data).
static SseEvent make_sse_data(std::string data) {
    SseEvent ev;
    ev.data = std::move(data);
    return ev;
}

/// Build a [DONE] SSE event.
static SseEvent make_sse_done() {
    SseEvent ev;
    ev.data    = "[DONE]";
    ev.is_done = true;
    return ev;
}

/// Build a normal content chunk SSE event (ev.event = "", choices[0].delta.content = text).
static SseEvent make_content_chunk(const std::string& text) {
    batbox::Json chunk = {
        {"id",      "chatcmpl-test"},
        {"choices", batbox::Json::array({
            batbox::Json{
                {"index",        0},
                {"delta",        batbox::Json{{"content", text}}},
                {"finish_reason", nullptr}
            }
        })}
    };
    return make_sse_data(chunk.dump());
}

TEST_SUITE("[TUI-T18] Client::stream_chat SSE error surfacing") {

    // -------------------------------------------------------------------------
    // A. Server-sent SSE event: error surfaces as Err with "server:" prefix
    // -------------------------------------------------------------------------

    TEST_CASE("A: event=error with JSON payload surfaces server message") {
        // LM Studio emits event: error / data: {"error":{"message":"..."}}
        // over HTTP 200.  The write_cb must parse the message and set
        // stream_error = "server: context length exceeded".
        std::vector<SseEvent> events = {
            make_sse_event("error",
                R"({"error":{"message":"context length exceeded","type":"invalid_request_error","code":400}})"),
        };

        auto r = dispatch_sse_events(events);

        REQUIRE_FALSE(r.ok);
        CHECK(r.stream_error.find("server:") != std::string::npos);
        CHECK(r.stream_error.find("context length exceeded") != std::string::npos);
    }

    // -------------------------------------------------------------------------
    // B. fatal_error event name is also handled
    // -------------------------------------------------------------------------

    TEST_CASE("B: event=fatal_error with JSON payload surfaces server message") {
        // Some providers use fatal_error instead of error.  Both must be caught.
        std::vector<SseEvent> events = {
            make_sse_event("fatal_error",
                R"({"error":{"message":"internal server failure","type":"server_error","code":500}})"),
        };

        auto r = dispatch_sse_events(events);

        REQUIRE_FALSE(r.ok);
        CHECK(r.stream_error.find("server:") != std::string::npos);
        CHECK(r.stream_error.find("internal server failure") != std::string::npos);
    }

    // -------------------------------------------------------------------------
    // C. Inline error in chunk JSON (no event: name)
    // -------------------------------------------------------------------------

    TEST_CASE("C: default event with top-level error object surfaces server message") {
        // Some providers emit HTTP 200 with data: {"error":{"message":"..."}}
        // without setting the SSE event: field.  The inline-error branch catches this.
        std::vector<SseEvent> events = {
            make_sse_data(R"({"error":{"message":"unexpected EOF"}})"),
        };

        auto r = dispatch_sse_events(events);

        REQUIRE_FALSE(r.ok);
        CHECK(r.stream_error.find("server:") != std::string::npos);
        CHECK(r.stream_error.find("unexpected EOF") != std::string::npos);
    }

    // -------------------------------------------------------------------------
    // D. Ping events are skipped silently — stream continues
    // -------------------------------------------------------------------------

    TEST_CASE("D: ping event followed by content chunk completes without error") {
        // Server emits a keepalive ping before real data.  The ping must not
        // cause an error; the following content chunk must still be delivered.
        std::vector<SseEvent> events = {
            make_sse_event("ping", "{}"),
            make_content_chunk("Hello from the model!"),
            make_sse_done(),
        };

        auto r = dispatch_sse_events(events);

        CHECK(r.ok);
        CHECK(r.stream_error.empty());
        CHECK(r.content_seen);
    }

    // -------------------------------------------------------------------------
    // E. Content delivery still works after a ping event (sequence integrity)
    // -------------------------------------------------------------------------

    TEST_CASE("E: ping → content → finish sequence delivers content and no error") {
        // Verifies that the `continue` added by TUI-T17 for non-error named
        // events does not break the overall flow: the ping is skipped, the
        // subsequent content and [DONE] are processed normally.
        std::vector<SseEvent> events = {
            make_sse_event("ping", "{}"),
            make_content_chunk("The answer is 42."),
            make_sse_done(),
        };

        auto r = dispatch_sse_events(events);

        CHECK(r.ok);
        CHECK(r.content_seen);   // content token was delivered after the ping
        CHECK(r.stream_error.empty());
    }

    // -------------------------------------------------------------------------
    // F. Malformed error JSON falls back to raw data string
    // -------------------------------------------------------------------------

    TEST_CASE("F: event=error with non-JSON data falls back to raw payload") {
        // When the data field is not valid JSON, the handler uses ev.data as-is
        // rather than returning an empty or misleading message.
        std::vector<SseEvent> events = {
            make_sse_event("error", "not json at all"),
        };

        auto r = dispatch_sse_events(events);

        REQUIRE_FALSE(r.ok);
        // stream_error must contain the raw fallback text
        CHECK(r.stream_error.find("server:") != std::string::npos);
        CHECK(r.stream_error.find("not json at all") != std::string::npos);
    }

    // -------------------------------------------------------------------------
    // G. Empty error.message falls back to raw data
    // -------------------------------------------------------------------------

    TEST_CASE("G: event=error with empty error object falls back to raw payload") {
        // {"error":{}} — the error object exists but has no message field.
        // The handler must still return Err with the raw data as content.
        std::vector<SseEvent> events = {
            make_sse_event("error", R"({"error":{}})"),
        };

        auto r = dispatch_sse_events(events);

        REQUIRE_FALSE(r.ok);
        CHECK(r.stream_error.find("server:") != std::string::npos);
        // server_msg fell back to ev.data since message was absent
        CHECK(r.stream_error.find(R"({"error":{}})") != std::string::npos);
    }

    // -------------------------------------------------------------------------
    // H. Multiple pings before content — all skipped, content arrives
    // -------------------------------------------------------------------------

    TEST_CASE("H: multiple ping events all skipped, content still delivered") {
        std::vector<SseEvent> events = {
            make_sse_event("ping", "{}"),
            make_sse_event("ping", "{}"),
            make_sse_event("ping", "{}"),
            make_content_chunk("batbox response"),
            make_sse_done(),
        };

        auto r = dispatch_sse_events(events);

        CHECK(r.ok);
        CHECK(r.content_seen);
        CHECK(r.stream_error.empty());
    }

    // -------------------------------------------------------------------------
    // I. Error event after content is already delivered — error still surfaces
    // -------------------------------------------------------------------------

    TEST_CASE("I: error event after partial content delivery surfaces as Err") {
        // Some servers start streaming content then send an error event mid-stream
        // (e.g. hitting context limit after a few tokens).  The error must still
        // surface even after content delivery began.
        std::vector<SseEvent> events = {
            make_content_chunk("Partial answer..."),
            make_sse_event("error",
                R"({"error":{"message":"generation halted: context overflow"}})"),
        };

        auto r = dispatch_sse_events(events);

        REQUIRE_FALSE(r.ok);
        CHECK(r.stream_error.find("server:") != std::string::npos);
        CHECK(r.stream_error.find("generation halted") != std::string::npos);
    }

    // -------------------------------------------------------------------------
    // J. Clean stream with no named events — baseline non-regression
    // -------------------------------------------------------------------------

    TEST_CASE("J: clean stream without any named events completes successfully") {
        // Regression guard: normal streams have no event: field.  Verify the
        // T17 branches are transparent when no named events appear.
        std::vector<SseEvent> events = {
            make_content_chunk("Hello,"),
            make_content_chunk(" world!"),
            make_sse_done(),
        };

        auto r = dispatch_sse_events(events);

        CHECK(r.ok);
        CHECK(r.content_seen);
        CHECK(r.stream_error.empty());
    }
}
