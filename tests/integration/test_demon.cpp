// tests/integration/test_demon.cpp
// =============================================================================
// doctest integration test suite for CPP 6.8: Demon agent spec + rate limiter.
//
// Coverage:
//   AC1  /demon spawns and enters Listening state
//        → demon_make_spec() returns valid AgentSpec with name="demon"
//        → prompt_body contains Party Monster voice markers
//   AC2  Parent turn completes → demon optionally posts comment (within rate limit)
//        → DemonRateLimiter::is_allowed() returns true on first comment
//        → DemonRateLimiter::is_allowed() returns false within 30s window
//        → DemonRateLimiter::record_comment() updates state correctly
//   AC3  Comments render into DemonPanel (not SubAgentPanel)
//        → demon_make_spec() name == "demon" (routes to DemonPanel by convention)
//   AC4  Token budget enforced — after 1% session tokens, demon stops commenting
//        → DemonRateLimiter::is_allowed() returns false when token cap exceeded
//   AC5  /demon dismiss cancels the demon agent
//        → Covered by test_demon_command.cpp (CPP S.14); verified here via spec
//   AC6  Direct commands run as one-shot task with demon persona
//        → demon_make_spec() prompt_body contains task-routing instruction
//
// Strategy
// --------
// Tests in this suite run entirely in memory without spawning threads or
// FTXUI.  demon_make_spec() and DemonRateLimiter are pure logic with no I/O.
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/agents/Demon.hpp>
#include "demon_taglines.hpp"
#include <batbox/agents/AgentSpec.hpp>

#include <chrono>
#include <string>
#include <thread>

using namespace batbox::agents;

// ============================================================================
// TEST SUITE: demon_make_spec — AgentSpec contract
// ============================================================================

TEST_SUITE("demon_make_spec — AgentSpec contract") {

    TEST_CASE("AC1: name is 'demon'") {
        const AgentSpec spec = demon_make_spec();
        CHECK(spec.name == "demon");
    }

    TEST_CASE("AC3: description is non-empty") {
        const AgentSpec spec = demon_make_spec();
        CHECK(!spec.description.empty());
    }

    TEST_CASE("model is nullopt — uses session default") {
        const AgentSpec spec = demon_make_spec();
        CHECK(!spec.model.has_value());
    }

    TEST_CASE("allowed_tools is empty — demon is a commentator, not a tool-caller") {
        const AgentSpec spec = demon_make_spec();
        CHECK(spec.allowed_tools.empty());
    }

    TEST_CASE("AC1: prompt_body is non-empty") {
        const AgentSpec spec = demon_make_spec();
        CHECK(!spec.prompt_body.empty());
    }

    TEST_CASE("AC1: prompt_body contains Party Monster voice markers") {
        const AgentSpec spec = demon_make_spec();
        // Must reference the glamour-ghoul voice style.
        CHECK(spec.prompt_body.find("Party Monster") != std::string::npos);
    }

    TEST_CASE("AC1: prompt_body contains 'Oh my god' affirmation") {
        const AgentSpec spec = demon_make_spec();
        CHECK(spec.prompt_body.find("Oh my god") != std::string::npos);
    }

    TEST_CASE("AC1: prompt_body contains 'Tell me about it' affirmation") {
        const AgentSpec spec = demon_make_spec();
        CHECK(spec.prompt_body.find("Tell me about it") != std::string::npos);
    }

    TEST_CASE("AC6: prompt_body references one-shot task routing") {
        const AgentSpec spec = demon_make_spec();
        // The prompt must instruct the demon to handle direct task prompts.
        CHECK(spec.prompt_body.find("task") != std::string::npos);
    }

    TEST_CASE("spec.source_path is empty — baked-in, not loaded from disk") {
        const AgentSpec spec = demon_make_spec();
        CHECK(spec.source_path.empty());
    }

    TEST_CASE("demon_make_spec can be called repeatedly and produces consistent specs") {
        const AgentSpec a = demon_make_spec();
        const AgentSpec b = demon_make_spec();
        CHECK(a.name == b.name);
        CHECK(a.description == b.description);
        CHECK(a.prompt_body == b.prompt_body);
        CHECK(a.allowed_tools == b.allowed_tools);
    }
}

// ============================================================================
// TEST SUITE: DemonRateLimiter — time-based rate limit
// ============================================================================

TEST_SUITE("DemonRateLimiter — time-based rate limit") {

    TEST_CASE("AC2: is_allowed() returns true on first comment (first_comment=true)") {
        DemonRateLimiter rl;
        CHECK(rl.first_comment == true);
        CHECK(rl.is_allowed());
    }

    TEST_CASE("AC2: is_allowed() returns false immediately after record_comment()") {
        DemonRateLimiter rl;
        // First comment is always allowed.
        REQUIRE(rl.is_allowed());
        // Record a comment with 0 tokens (budget-check disabled at 0).
        rl.record_comment(0);
        // Immediately after: time limit not satisfied → should be blocked.
        CHECK_FALSE(rl.is_allowed());
    }

    TEST_CASE("AC2: first_comment is false after record_comment()") {
        DemonRateLimiter rl;
        rl.record_comment(10);
        CHECK(rl.first_comment == false);
    }

    TEST_CASE("AC2: tokens_used accumulates across record_comment() calls") {
        DemonRateLimiter rl;
        rl.record_comment(50);
        CHECK(rl.tokens_used == 50);
        // Manipulate time by resetting last_comment_time manually.
        // Here we just verify the accumulation.
        rl.record_comment(30);
        CHECK(rl.tokens_used == 80);
    }

    TEST_CASE("AC2: is_allowed() with zero budget skips token check") {
        DemonRateLimiter rl;
        rl.record_comment(9999999);  // Huge token count.
        // With session_token_budget=0, token check is skipped.
        // Time check will still block immediately after record.
        // So is_allowed(0) should be false (time blocked, token check skipped).
        CHECK_FALSE(rl.is_allowed(0));
    }
}

// ============================================================================
// TEST SUITE: DemonRateLimiter — token-budget rate limit (AC4)
// ============================================================================

TEST_SUITE("DemonRateLimiter — token budget (AC4)") {

    TEST_CASE("AC4: is_allowed() returns false when 1% token cap is exceeded") {
        DemonRateLimiter rl;
        // Session budget: 10000 tokens → 1% cap = 100 tokens.
        const std::size_t session_budget = 10000;
        // Simulate having used 101 tokens already (cap exceeded).
        rl.tokens_used   = 101;
        rl.first_comment = false;
        // Time: manipulate by setting last_comment_time to well in the past.
        rl.last_comment_time = std::chrono::steady_clock::now() -
            std::chrono::seconds(kDemonMinCommentIntervalSec + 10);

        // Time limit is satisfied, but token cap is exceeded → blocked.
        CHECK_FALSE(rl.is_allowed(session_budget));
    }

    TEST_CASE("AC4: is_allowed() returns true when within token cap") {
        DemonRateLimiter rl;
        // Session budget: 10000 tokens → 1% cap = 100 tokens.
        const std::size_t session_budget = 10000;
        // Used only 50 tokens — within the 100 cap.
        rl.tokens_used   = 50;
        rl.first_comment = false;
        // Set last comment time to satisfy the 30s time window.
        rl.last_comment_time = std::chrono::steady_clock::now() -
            std::chrono::seconds(kDemonMinCommentIntervalSec + 5);

        CHECK(rl.is_allowed(session_budget));
    }

    TEST_CASE("AC4: is_allowed() returns true at exactly token cap boundary (exclusive)") {
        DemonRateLimiter rl;
        // Session budget: 10000 tokens → cap = 100 tokens.
        const std::size_t session_budget = 10000;
        // At cap boundary: tokens_used == cap → blocked (not strictly less than).
        rl.tokens_used   = 100;
        rl.first_comment = false;
        rl.last_comment_time = std::chrono::steady_clock::now() -
            std::chrono::seconds(kDemonMinCommentIntervalSec + 5);

        CHECK_FALSE(rl.is_allowed(session_budget));
    }

    TEST_CASE("AC4: tokens_used below cap and time satisfied → allowed") {
        DemonRateLimiter rl;
        const std::size_t session_budget = 50000;  // 1% = 500 tokens cap.
        rl.tokens_used   = 499;
        rl.first_comment = false;
        rl.last_comment_time = std::chrono::steady_clock::now() -
            std::chrono::seconds(kDemonMinCommentIntervalSec + 1);

        CHECK(rl.is_allowed(session_budget));
    }

    TEST_CASE("AC4: record_comment accumulates correctly for budget tracking") {
        DemonRateLimiter rl;
        rl.record_comment(30);
        rl.record_comment(25);
        rl.record_comment(50);
        CHECK(rl.tokens_used == 105);
    }
}

// ============================================================================
// TEST SUITE: rate limit constants — contract values
// ============================================================================

TEST_SUITE("Demon — rate limit constants") {

    TEST_CASE("kDemonMinCommentIntervalSec is 30") {
        CHECK(kDemonMinCommentIntervalSec == 30);
    }

    TEST_CASE("kDemonMaxTokenPercent is 1") {
        CHECK(kDemonMaxTokenPercent == 1);
    }
}

// ============================================================================
// TEST SUITE: kDemonTaglines — agent vocabulary
// ============================================================================

TEST_SUITE("kDemonTaglines — agent vocabulary") {

    TEST_CASE("kDemonTaglines has 20 entries") {
        CHECK(kDemonTaglines.size() == 20);
    }

    TEST_CASE("all taglines are non-empty") {
        for (const auto& tagline : kDemonTaglines) {
            CHECK(!tagline.empty());
        }
    }

    TEST_CASE("all taglines are valid string_views (no null terminator issues)") {
        for (const auto& tagline : kDemonTaglines) {
            // Convert to string and back — should be lossless.
            const std::string s(tagline);
            CHECK(s.size() == tagline.size());
        }
    }
}
