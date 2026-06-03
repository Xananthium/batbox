// tests/unit/test_standing_reminder.cpp
// =============================================================================
// Unit tests for batbox::conversation::StandingReminder (DIS-988, S2/S3 AC4).
//
// AC4 coverage: the "warm subagents available for follow-up" status surfaced
// each turn as a TAIL reminder, NOT in the cached system-prompt prefix:
//   (a) the warm-subagent list surfaces to the model — apply_standing_reminder
//       appends a trailing message carrying the handles + one-line statuses;
//   (b) the cached prefix (the leading system message) is unchanged by standing
//       mutations — different standing content changes ONLY the tail message;
//   (c) an empty list is a no-op — the request (and its cache) is untouched;
//   (d) the list is BOUNDED — at most max_handles lines, with a "(+N more)" note.
//
// Build standalone (from repo root, x64-linux triplet):
//   c++ -std=c++20 -I include -I build/vcpkg_installed/x64-linux/include \
//       tests/unit/test_standing_reminder.cpp \
//       src/conversation/StandingReminder.cpp \
//       build/vcpkg_installed/x64-linux/lib/libsimdjson.a \
//       -o /tmp/test_standing_reminder && /tmp/test_standing_reminder
//   (libsimdjson is pulled in transitively via ChatRequest.hpp -> Json.hpp.)
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/conversation/StandingReminder.hpp>
#include <batbox/inference/ChatRequest.hpp>

#include <string>
#include <vector>

using batbox::conversation::apply_standing_reminder;
using batbox::conversation::compose_standing_reminder;
using batbox::conversation::StandingHandle;
using batbox::inference::ChatRequest;
using batbox::inference::WireMessage;

namespace {

ChatRequest make_request(const std::string& system_prompt) {
    ChatRequest req;
    WireMessage sys; sys.role = "system"; sys.content = system_prompt;
    WireMessage usr; usr.role = "user";   usr.content = "do the thing";
    req.messages.push_back(std::move(sys));
    req.messages.push_back(std::move(usr));
    return req;
}

std::vector<StandingHandle> sample(std::size_t n) {
    std::vector<StandingHandle> v;
    for (std::size_t i = 0; i < n; ++i) {
        v.push_back(StandingHandle{
            "id-" + std::to_string(i),
            "agent-" + std::to_string(i),
            "result " + std::to_string(i)});
    }
    return v;
}

} // namespace

TEST_SUITE("StandingReminder") {

    // (a)+(d) compose wraps lines in a delimited block; empty → "".
    TEST_CASE("compose wraps non-empty handles; empty list yields empty") {
        CHECK(compose_standing_reminder({}).empty());

        const std::string r = compose_standing_reminder(sample(2));
        CHECK(r.find("<warm_subagents>")  != std::string::npos);
        CHECK(r.find("</warm_subagents>") != std::string::npos);
        CHECK(r.find("id-0")              != std::string::npos);
        CHECK(r.find("agent-1")           != std::string::npos);
        CHECK(r.find("result 1")          != std::string::npos);
    }

    // (d) the list is bounded: at most max_handles lines + a "(+N more)" note.
    TEST_CASE("compose is bounded by max_handles with a (+N more) note") {
        const std::string r = compose_standing_reminder(sample(10), /*max=*/3);
        CHECK(r.find("id-0") != std::string::npos);
        CHECK(r.find("id-1") != std::string::npos);
        CHECK(r.find("id-2") != std::string::npos);
        // The 4th+ entries are NOT individually listed...
        CHECK(r.find("id-3") == std::string::npos);
        // ...but their count is surfaced.
        CHECK(r.find("(+7 more)") != std::string::npos);
    }

    // (c) empty list → apply is a no-op; the request (and cache) untouched.
    TEST_CASE("apply on empty list is a no-op (cache untouched)") {
        ChatRequest req = make_request("SYSTEM PREFIX");
        const std::size_t before = req.messages.size();

        const bool injected = apply_standing_reminder(req, {});
        CHECK(injected == false);
        CHECK(req.messages.size() == before);
        CHECK(req.messages.front().content == "SYSTEM PREFIX");
    }

    // (a)+(b) apply appends a trailing message; the cached prefix is unchanged.
    TEST_CASE("apply appends a tail reminder; cached prefix preserved") {
        ChatRequest req = make_request("SYSTEM PREFIX");
        const std::size_t before = req.messages.size();

        const bool injected = apply_standing_reminder(req, sample(1));
        CHECK(injected == true);
        REQUIRE(req.messages.size() == before + 1);

        // Everything BEFORE the appended reminder is byte-identical: the leading
        // system prefix and the user message are untouched — only the tail grew.
        CHECK(req.messages.front().content == "SYSTEM PREFIX");
        CHECK(req.messages[1].content == "do the thing");

        // The appended tail is a system-role reminder carrying the handle.
        const WireMessage& tail = req.messages.back();
        CHECK(tail.role == "system");
        REQUIRE(tail.content.has_value());
        CHECK(tail.content.value().find("<warm_subagents>") != std::string::npos);
        CHECK(tail.content.value().find("id-0")             != std::string::npos);
    }

    // (b) the cache-preserving property: different standing content changes ONLY
    // the tail; the prefix region is identical across both requests.
    TEST_CASE("different standing content changes only the tail message") {
        ChatRequest a = make_request("SYSTEM PREFIX");
        ChatRequest b = make_request("SYSTEM PREFIX");
        apply_standing_reminder(a, sample(1));
        apply_standing_reminder(b, sample(2));

        // Same prefix region (all but the last message) byte-for-byte.
        REQUIRE(a.messages.size() == b.messages.size());          // both +1
        for (std::size_t i = 0; i + 1 < a.messages.size(); ++i) {
            CHECK(a.messages[i].role    == b.messages[i].role);
            CHECK(a.messages[i].content == b.messages[i].content);
        }
        // Only the tail reminder differs.
        CHECK(a.messages.back().content != b.messages.back().content);
    }
}
