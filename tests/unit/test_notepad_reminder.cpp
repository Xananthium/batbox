// tests/unit/test_notepad_reminder.cpp
// =============================================================================
// Unit tests for batbox::conversation::NotepadReminder (DIS-981, S6).
//
// AC4 coverage: per-turn re-injection as a TAIL reminder, NOT in the cached
// system-prompt prefix:
//   (a) the pad surfaces to the model each turn — apply_notepad_reminder
//       appends a trailing message carrying the pad slice;
//   (b) the cached prefix (the leading system message) is unchanged by pad
//       mutations — different pad content changes ONLY the tail message, so the
//       KV/prefix cache over everything before it is preserved;
//   (c) an empty pad is a no-op — the request (and its cache) is untouched.
//
// Build standalone (from repo root, x64-linux triplet):
//   c++ -std=c++20 -I include -I build/vcpkg_installed/x64-linux/include \
//       tests/unit/test_notepad_reminder.cpp \
//       src/conversation/NotepadReminder.cpp \
//       -o /tmp/test_notepad_reminder && /tmp/test_notepad_reminder
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/conversation/NotepadReminder.hpp>
#include <batbox/inference/ChatRequest.hpp>

#include <string>

using batbox::conversation::apply_notepad_reminder;
using batbox::conversation::compose_notepad_reminder;
using batbox::inference::ChatRequest;
using batbox::inference::WireMessage;

namespace {
// A request shaped like build_chat_request output: a leading system message
// (the cached prefix) followed by conversation history.
ChatRequest make_request(const std::string& system_prompt) {
    ChatRequest req;
    WireMessage sys;  sys.role = "system";     sys.content = system_prompt;
    WireMessage usr;  usr.role = "user";       usr.content = "do the thing";
    req.messages.push_back(std::move(sys));
    req.messages.push_back(std::move(usr));
    return req;
}
} // namespace

TEST_SUITE("NotepadReminder") {

    // -----------------------------------------------------------------------
    // compose_notepad_reminder formats a delimited block; empty slice → "".
    // -----------------------------------------------------------------------
    TEST_CASE("compose wraps a non-empty slice; empty slice yields empty") {
        CHECK(compose_notepad_reminder("").empty());

        const std::string r = compose_notepad_reminder("- plan: ship S6");
        CHECK(r.find("<notepad>")        != std::string::npos);
        CHECK(r.find("</notepad>")       != std::string::npos);
        CHECK(r.find("- plan: ship S6")  != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // AC4(a): the pad surfaces as the FINAL (tail) message.
    // -----------------------------------------------------------------------
    TEST_CASE("reminder is appended as the tail message") {
        ChatRequest req = make_request("BASE SYSTEM PROMPT");
        const std::size_t before = req.messages.size();

        const bool injected = apply_notepad_reminder(req, "- found: the gold line");
        CHECK(injected);
        CHECK(req.messages.size() == before + 1);

        // It is the LAST message, and it carries the pad content.
        const WireMessage& tail = req.messages.back();
        CHECK(tail.role == "system");
        REQUIRE(tail.content.has_value());
        CHECK(tail.content->find("the gold line") != std::string::npos);
        CHECK(tail.content->find("<notepad>")     != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // AC4(b): the cached prefix is UNCHANGED by pad mutations.
    // Different pad content must change ONLY the tail — messages[0] (the system
    // prompt the KV cache covers) is byte-identical across pads, and all the
    // pre-existing messages are untouched.
    // -----------------------------------------------------------------------
    TEST_CASE("pad mutations change only the tail, never the cached prefix") {
        ChatRequest a = make_request("BASE SYSTEM PROMPT");
        ChatRequest b = make_request("BASE SYSTEM PROMPT");

        REQUIRE(apply_notepad_reminder(a, "pad version ALPHA"));
        REQUIRE(apply_notepad_reminder(b, "pad version BETA — totally different"));

        // The leading system message (cached prefix) is identical regardless of
        // pad content.
        REQUIRE(a.messages.front().content.has_value());
        REQUIRE(b.messages.front().content.has_value());
        CHECK(a.messages.front().role    == b.messages.front().role);
        CHECK(*a.messages.front().content == *b.messages.front().content);
        CHECK(*a.messages.front().content == "BASE SYSTEM PROMPT");

        // The pre-existing user message is also identical (untouched).
        CHECK(*a.messages[1].content == *b.messages[1].content);

        // Only the tail differs.
        CHECK(*a.messages.back().content != *b.messages.back().content);
        CHECK(a.messages.back().content->find("ALPHA") != std::string::npos);
        CHECK(b.messages.back().content->find("BETA")  != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // AC4(c): an empty pad is a no-op — the request is untouched.
    // -----------------------------------------------------------------------
    TEST_CASE("empty pad is a no-op (cache never disturbed)") {
        ChatRequest req = make_request("BASE SYSTEM PROMPT");
        const std::size_t before = req.messages.size();

        const bool injected = apply_notepad_reminder(req, "");
        CHECK_FALSE(injected);
        CHECK(req.messages.size() == before);     // nothing appended
        CHECK(*req.messages.back().content == "do the thing");  // tail unchanged
    }
}
