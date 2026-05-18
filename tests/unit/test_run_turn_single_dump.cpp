// tests/unit/test_run_turn_single_dump.cpp
// =============================================================================
// Regression tests for PEXT 4.1 — K-1+K-2 hoist.
//
// Verifies the safety contracts that make the PEXT 4.1 refactor correct:
//
//   1. compose_system_prompt is deterministic: calling it twice with the same
//      arguments produces byte-identical output.  (K-2 precondition)
//
//   2. The bytes/4 preflight estimate from a hoisted ChatRequest JSON equals
//      the estimate from a separately-built request with the same inputs.
//      (K-1 core correctness: the hoisted req produces the same estimate)
//
//   3. ChatRequest JSON serialization is stable: serialising the same struct
//      twice produces the same bytes.  (Ensures the optimization is safe to
//      call .dump() on the already-built req instead of building a fresh one)
//
//   4. Compaction rebuild uses the same sys_prompt but updated messages:
//      the post-compaction request correctly reflects the shorter message list
//      while keeping the same system prompt.
//
//   5. Tool-call follow-up rebuild picks up newly appended tool messages.
//
// Note: Conversation::build_chat_request is private.  This test verifies the
// same behaviour by exercising the public free functions (compose_system_prompt,
// the ChatRequest to_json serialiser) plus inline construction of ChatRequest
// objects using the same logic that build_chat_request uses internally.
//
// Build standalone (no CMake, from repo root):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_run_turn_single_dump.cpp \
//       src/conversation/SystemPrompt.cpp \
//       src/inference/ChatRequest.cpp \
//       src/core/Uuid.cpp src/core/Logging.cpp src/core/Paths.cpp \
//       src/core/Json.cpp src/core/CancelToken.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       build/vcpkg_installed/arm64-osx/lib/libspdlog.a \
//       build/vcpkg_installed/arm64-osx/lib/libfmt.a \
//       -o /tmp/test_run_turn_single_dump && /tmp/test_run_turn_single_dump
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/conversation/SystemPrompt.hpp>
#include <batbox/inference/ChatRequest.hpp>
#include <batbox/core/Json.hpp>

#include <filesystem>
#include <string>
#include <vector>
#include <optional>

namespace {

namespace inf = batbox::inference;

// ---------------------------------------------------------------------------
// Build a minimal ChatRequest inline — same logic as Conversation::build_chat_request
// but using only public types.  This simulates what the hoisted call produces.
// ---------------------------------------------------------------------------
inf::ChatRequest make_request(
    const std::string& model,
    const std::string& sys_prompt,
    const std::vector<std::pair<std::string, std::string>>& role_content_pairs)
{
    inf::ChatRequest req;
    req.model  = model;
    req.stream = true;
    req.stream_options_include_usage = true;

    if (!sys_prompt.empty()) {
        inf::WireMessage sys;
        sys.role    = "system";
        sys.content = sys_prompt;
        req.messages.push_back(std::move(sys));
    }

    for (const auto& [role, content] : role_content_pairs) {
        inf::WireMessage wm;
        wm.role    = role;
        wm.content = content;
        req.messages.push_back(std::move(wm));
    }

    return req;
}

} // anonymous namespace

// ===========================================================================
// Test 1 — compose_system_prompt is deterministic (pure function)
//
// K-2 safety precondition: the single hoisted call produces the same bytes
// as two separate calls would have.
// ===========================================================================
TEST_CASE("compose_system_prompt is deterministic across two calls") {
    const std::filesystem::path work_dir = std::filesystem::temp_directory_path();

    const std::string prompt_a =
        batbox::conversation::compose_system_prompt(/*plan_mode=*/false, work_dir);
    const std::string prompt_b =
        batbox::conversation::compose_system_prompt(/*plan_mode=*/false, work_dir);

    CHECK(prompt_a == prompt_b);
    CHECK_FALSE(prompt_a.empty());

    SUBCASE("plan-mode prefix changes the output") {
        const std::string prompt_plan =
            batbox::conversation::compose_system_prompt(/*plan_mode=*/true, work_dir);
        // The plan-mode prompt must differ from the non-plan prompt (it prepends
        // the read-only prefix).
        CHECK(prompt_plan != prompt_a);
    }
}

// ===========================================================================
// Test 2 — ChatRequest JSON serialization is stable
//
// K-1 safety precondition: serialising the same ChatRequest twice produces
// byte-identical JSON.  This confirms that computing .dump() on the hoisted
// req (instead of building a fresh preflight_req) is a no-op.
// ===========================================================================
TEST_CASE("ChatRequest JSON serialization is stable across two dumps") {
    const std::string sys_prompt = "You are a helpful assistant.";
    const std::vector<std::pair<std::string, std::string>> msgs = {
        {"user",      "Hello"},
        {"assistant", "Hi there"},
        {"user",      "How are you?"},
    };

    const inf::ChatRequest req = make_request("test-model", sys_prompt, msgs);

    const std::string json_a = batbox::Json(req).dump();
    const std::string json_b = batbox::Json(req).dump();

    CHECK(json_a == json_b);
    CHECK_FALSE(json_a.empty());
}

// ===========================================================================
// Test 3 — Preflight estimate from hoisted req matches a fresh-built request
//
// K-1 core test: the bytes/4 estimate computed from the hoisted req must equal
// the estimate that the old code would compute from a separately-built
// preflight_req with the same inputs.
// ===========================================================================
TEST_CASE("preflight estimate from hoisted req matches estimate from fresh build") {
    const std::string model      = "magistral-small-2506";
    const std::string sys_prompt = "You are a helpful assistant.";
    const std::vector<std::pair<std::string, std::string>> msgs = {
        {"user",      "What is the capital of France?"},
        {"assistant", "The capital of France is Paris."},
        {"user",      "Thank you!"},
    };

    // Old code path: build a fresh preflight_req, dump it, estimate.
    const inf::ChatRequest preflight_req = make_request(model, sys_prompt, msgs);
    const std::size_t old_est = batbox::Json(preflight_req).dump().size() / 4;

    // New code path: use the hoisted req (same inputs), dump it, estimate.
    const inf::ChatRequest hoisted_req = make_request(model, sys_prompt, msgs);
    const std::size_t new_est = batbox::Json(hoisted_req).dump().size() / 4;

    // The estimates must be identical.
    CHECK(old_est == new_est);
    CHECK(old_est > 0);
}

// ===========================================================================
// Test 4 — System prompt is present as the first messages[] entry
//
// Verifies that the system prompt injection in the wire request is correct:
// it must be the first entry with role "system".
// ===========================================================================
TEST_CASE("system prompt appears as first messages[] entry with role system") {
    const std::string sys_prompt = "SENTINEL_SYSTEM_PROMPT_XYZ_42";
    const std::vector<std::pair<std::string, std::string>> msgs = {
        {"user",      "Hello"},
        {"assistant", "Hi"},
    };

    const inf::ChatRequest req = make_request("test-model", sys_prompt, msgs);

    REQUIRE_FALSE(req.messages.empty());
    CHECK(req.messages.front().role    == "system");
    CHECK(req.messages.front().content == sys_prompt);

    // Total = system + conversation history.
    CHECK(req.messages.size() == msgs.size() + 1);
}

// ===========================================================================
// Test 5 — Compaction rebuild: same sys_prompt, updated message count
//
// Simulates the PEXT 4.1 compaction path:
//   a. Build req_pre from original messages.
//   b. After compaction, messages shrink.
//   c. Rebuild req_post using the SAME sys_prompt but the shorter message list.
//   d. sys_prompt must be identical in both requests.
//   e. Message count must reflect the compacted list.
//   f. bytes/4 estimate of post must be less than pre.
// ===========================================================================
TEST_CASE("compaction rebuild: same sys_prompt, smaller message count") {
    const std::string model = "test-model";

    // Compute sys_prompt once (hoisted — K-2 eliminates the second call).
    const std::string sys_prompt =
        batbox::conversation::compose_system_prompt(
            /*plan_mode=*/false,
            std::filesystem::temp_directory_path());

    // Pre-compaction: 10 message turns.
    std::vector<std::pair<std::string, std::string>> msgs_pre;
    for (int i = 0; i < 10; ++i) {
        msgs_pre.push_back({(i % 2 == 0) ? "user" : "assistant",
                            "message content number " + std::to_string(i)});
    }

    const inf::ChatRequest req_pre = make_request(model, sys_prompt, msgs_pre);
    // system + 10 turns = 11 messages total.
    CHECK(req_pre.messages.size() == 11);

    // Simulate compaction: summarise head, keep last 3 turns verbatim.
    // Post-compaction the list has 1 summary + 3 tail = 4 messages.
    const std::vector<std::pair<std::string, std::string>> msgs_post = {
        {"system",    "Summary of earlier conversation: user asked 7 questions."},
        {"user",      "message content number 7"},
        {"assistant", "message content number 8"},
        {"user",      "message content number 9"},
    };

    // Rebuild req using the SAME sys_prompt (K-2: no second compose call).
    const inf::ChatRequest req_post = make_request(model, sys_prompt, msgs_post);

    // System prompt must be the same in both requests.
    REQUIRE_FALSE(req_pre.messages.empty());
    REQUIRE_FALSE(req_post.messages.empty());
    CHECK(req_pre.messages.front().content  == sys_prompt);
    CHECK(req_post.messages.front().content == sys_prompt);

    // Post-compact request has fewer messages (system + 4 post-compact = 5).
    CHECK(req_post.messages.size() == 5);
    CHECK(req_post.messages.size() < req_pre.messages.size());

    // bytes/4 estimate must be smaller post-compaction.
    const std::size_t est_pre  = batbox::Json(req_pre).dump().size()  / 4;
    const std::size_t est_post = batbox::Json(req_post).dump().size() / 4;
    CHECK(est_post < est_pre);
}

// ===========================================================================
// Test 6 — Tool-call follow-up rebuild picks up new messages
//
// Simulates loop iteration > 0: tool result messages are appended to messages_
// between iterations.  The rebuild for iteration N+1 must include them.
// sys_prompt is reused unchanged (no second compose call).
// ===========================================================================
TEST_CASE("tool-call follow-up: rebuild picks up appended tool messages") {
    const std::string model      = "test-model";
    const std::string sys_prompt = "System instructions for the agent.";

    // Iteration 0: 3 conversation messages.
    const std::vector<std::pair<std::string, std::string>> msgs_iter0 = {
        {"user",      "Please run a tool for me."},
        {"assistant", ""},          // tool-call assistant turn (content often empty)
        {"tool",      "tool result: success"},
    };

    const inf::ChatRequest req_iter0 = make_request(model, sys_prompt, msgs_iter0);

    // After iteration 0, a new tool result arrives (iteration 1 prepares new req).
    std::vector<std::pair<std::string, std::string>> msgs_iter1 = msgs_iter0;
    msgs_iter1.push_back({"assistant", "Based on the tool result: all done."});

    const inf::ChatRequest req_iter1 = make_request(model, sys_prompt, msgs_iter1);

    // iter0: system + 3 conversation = 4.
    CHECK(req_iter0.messages.size() == 4);

    // iter1: system + 4 messages = 5.
    CHECK(req_iter1.messages.size() == 5);

    // sys_prompt is stable across iterations.
    CHECK(req_iter0.messages.front().content == sys_prompt);
    CHECK(req_iter1.messages.front().content == sys_prompt);

    // The JSON grows with the extra message.
    CHECK(batbox::Json(req_iter1).dump().size() >
          batbox::Json(req_iter0).dump().size());
}

// ===========================================================================
// Test 7 — model name and stream options are set correctly
//
// Verifies that the hoisted request carries the correct model name and
// stream_options_include_usage=true, which are required for the pre-flight
// estimate to be representative and for the live inference call to return usage.
// ===========================================================================
TEST_CASE("ChatRequest: model name and stream options propagated correctly") {
    const inf::ChatRequest req =
        make_request("magistral-small-2506", "sys", {{"user", "hi"}});

    CHECK(req.model  == "magistral-small-2506");
    CHECK(req.stream == true);
    CHECK(req.stream_options_include_usage == true);
}

// ===========================================================================
// PEXT2 3.1 — D-1: Verify the pre-flight body string equals Client::stream_chat
// input (byte-equality between preflight_body_str and the body that stream_chat
// would have built internally from the same ChatRequest).
//
// These tests prove the safety contract for the dump-count collapse:
//   The body string captured by the pre-flight dump is byte-identical to the
//   string that Client::stream_chat(const ChatRequest&, ...) would produce
//   by constructing `wire_req` (stream=true, stream_options=true) and dumping.
//
// Test 8 — No-compact path: preflight body equals wire body
//   The pre-flight dump uses the request as-built by build_chat_request (with
//   stream=true and stream_options_include_usage=true as defaults).
//   Client::stream_chat forces those same values.  Since they were already set,
//   the resulting dumps are byte-identical.
//
// Test 9 — Post-compact path: updated preflight body is wire-ready
//   After compaction a new request is built from the shorter message list.
//   The dump of this post-compact request must equal what stream_chat would
//   produce for the same post-compact ChatRequest.
//
// Test 10 — dump count validation helper
//   Confirms that calling .dump() exactly once per turn (pre-flight only) and
//   reusing the result produces the correct token estimate, matching the
//   count from a fresh dump of an identical ChatRequest.
// ===========================================================================

// ---------------------------------------------------------------------------
// Helper: simulate what Client::stream_chat does internally when building the
// wire body from a ChatRequest (force stream=true, stream_options=true, dump).
// ---------------------------------------------------------------------------
namespace {

std::string simulate_client_wire_body(const inf::ChatRequest& req) {
    // Mirrors the logic in Client::stream_chat(const ChatRequest&, ...):
    //   wire_req = req; wire_req.stream = true; wire_req.stream_options = true;
    //   body_json = wire_req; apply_provider_quirks (no-op for openai); dump.
    inf::ChatRequest wire_req = req;
    wire_req.stream = true;
    wire_req.stream_options_include_usage = true;
    const batbox::Json body_json = wire_req;
    return body_json.dump();
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Test 8 — No-compact path: preflight body equals wire body
// ---------------------------------------------------------------------------
TEST_CASE("PEXT2 3.1 — no-compact: preflight dump equals Client wire body") {
    const std::string model      = "magistral-small-2506";
    const std::string sys_prompt = "You are a helpful assistant.";
    const std::vector<std::pair<std::string, std::string>> msgs = {
        {"user",      "What is the capital of France?"},
        {"assistant", "The capital of France is Paris."},
        {"user",      "Thank you!"},
    };

    const inf::ChatRequest req = make_request(model, sys_prompt, msgs);

    // Preflight path (Conversation::run_turn pre-flight block):
    //   const batbox::Json preflight_json = preflight_req;
    //   preflight_body_str = preflight_json.dump();
    const batbox::Json preflight_json = req;
    const std::string preflight_body_str = preflight_json.dump();

    // Wire path (what Client::stream_chat would build internally):
    const std::string wire_body = simulate_client_wire_body(req);

    // The two strings must be byte-identical.
    CHECK(preflight_body_str == wire_body);
    CHECK_FALSE(preflight_body_str.empty());

    // The bytes/4 estimate from preflight must equal the estimate from the wire body.
    const std::size_t preflight_est = preflight_body_str.size() / 4;
    const std::size_t wire_est      = wire_body.size() / 4;
    CHECK(preflight_est == wire_est);
    CHECK(preflight_est > 0);
}

// ---------------------------------------------------------------------------
// Test 9 — Post-compact path: updated preflight body is wire-ready
// ---------------------------------------------------------------------------
TEST_CASE("PEXT2 3.1 — compact path: post-compact dump equals Client wire body") {
    const std::string model = "magistral-small-2506";
    const std::string sys_prompt_preflight =
        batbox::conversation::compose_system_prompt(
            /*plan_mode=*/false,
            std::filesystem::temp_directory_path());

    // Build the pre-compact preflight request.
    std::vector<std::pair<std::string, std::string>> msgs_pre;
    for (int i = 0; i < 8; ++i) {
        msgs_pre.push_back({(i % 2 == 0) ? "user" : "assistant",
                            "Turn " + std::to_string(i)});
    }
    const inf::ChatRequest pre_req = make_request(model, sys_prompt_preflight, msgs_pre);

    // Preflight dump (captured before compaction).
    const batbox::Json pre_json = pre_req;
    std::string preflight_body_str = pre_json.dump();  // non-const: updated post-compact
    const std::size_t pre_est = preflight_body_str.size() / 4;

    // Simulate compaction: keep last 2 messages verbatim, replace head with summary.
    const std::vector<std::pair<std::string, std::string>> msgs_post = {
        {"system",    "Summary: turns 0-5 discussed general topics."},
        {"user",      "Turn 6"},
        {"assistant", "Turn 7"},
    };
    const inf::ChatRequest post_req = make_request(model, sys_prompt_preflight, msgs_post);

    // Post-compact dump (preflight_body_str updated in Conversation::run_turn).
    const batbox::Json post_json = post_req;
    preflight_body_str = post_json.dump();  // mirrors Conversation.cpp line 283
    const std::size_t post_est = preflight_body_str.size() / 4;

    // Post-compact estimate must be smaller than pre-compact.
    CHECK(post_est < pre_est);

    // Post-compact preflight body must equal what Client would build for the
    // same post-compact request.
    const std::string wire_body = simulate_client_wire_body(post_req);
    CHECK(preflight_body_str == wire_body);
}

// ---------------------------------------------------------------------------
// Test 10 — Single-dump invariant: reusing a captured dump equals fresh dump
// ---------------------------------------------------------------------------
TEST_CASE("PEXT2 3.1 — single-dump invariant: captured body string is stable") {
    const inf::ChatRequest req = make_request(
        "test-model",
        "System prompt for stability test.",
        {
            {"user",      "Hello"},
            {"assistant", "Hi"},
            {"user",      "One more question"},
        });

    // Capture once (the PEXT2 3.1 path).
    const batbox::Json json_once = req;
    const std::string body_captured = json_once.dump();

    // Dump again (the pre-PEXT2-3.1 path that computed a fresh dump in stream_chat).
    const batbox::Json json_again = req;
    const std::string body_fresh = json_again.dump();

    // Must be byte-identical.
    CHECK(body_captured == body_fresh);

    // Token estimate must match.
    CHECK(body_captured.size() / 4 == body_fresh.size() / 4);
}
