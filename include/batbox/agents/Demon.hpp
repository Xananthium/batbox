// include/batbox/agents/Demon.hpp
// =============================================================================
// batbox::agents::Demon — Party Monster easter-egg sub-agent factory.
//
// CPP 6.8 Decision of Record #9
//
// The Demon agent is a special-cased SubAgent that:
//   - Has a baked-in AgentSpec (not loaded from disk) with the Party Monster
//     system prompt and "demon" name.
//   - Subscribes to ParentMessageObserved events to trigger rate-limited
//     commentary after parent conversation turns.
//   - Is rate-limited: at most 1 comment per 30 seconds AND at most 1% of
//     session token budget consumed overall.
//   - Renders its output into DemonPanel (not SubAgentPanel).
//   - Rotates from kDemonTaglines[] (Party Monster vocabulary, 5s rotation).
//
// Lifecycle
// ---------
//   The Demon is spawned by DemonCmd (CPP S.14) via AgentSupervisor::spawn()
//   using the built-in spec returned by Demon::spec().
//   DemonCmd passes an empty initial prompt to enter Listening state.
//
//   Listening state:
//     The Demon SubAgent waits for ParentMessageObserved events posted to the
//     shared AgentEventQueue by the conversation layer (CPP 3.7).  On each
//     event it checks the rate limits; if allowed, it generates a short Party
//     Monster quip and calls DemonPanel::set_demon_comment() to surface it.
//
//   One-shot task state:
//     When the user supplies a direct prompt (/demon find TODOs in src/), the
//     Demon runs it as an ordinary SubAgent turn in the demon persona, then
//     returns to Listening state.
//
// Rate limits (enforced by DemonRateLimiter)
// ------------------------------------------
//   kDemonMinCommentIntervalSec = 30  — wall-clock seconds between comments
//   kDemonMaxTokenPercent       = 1   — max % of session tokens (estimated)
//
// Thread safety
// -------------
//   Demon is a SubAgent facade; all SubAgent threading rules apply.
//   DemonPanel::set_demon_comment() is thread-safe and safe to call from the
//   Demon jthread.
//
// Blueprint contract (CPP 6.8 blueprints table):
//   class batbox::agents::Demon : public SubAgent
//   file  include/batbox/agents/Demon.hpp
// =============================================================================

#pragma once

#include <batbox/agents/AgentSpec.hpp>

#include <chrono>
#include <cstddef>
#include <string>

namespace batbox::agents {

// =============================================================================
// Rate-limit constants
// =============================================================================

/// Minimum wall-clock interval between demon commentary emissions (seconds).
inline constexpr int kDemonMinCommentIntervalSec = 30;

/// Maximum percentage of session token budget the Demon may consume (0–100).
inline constexpr int kDemonMaxTokenPercent = 1;

// =============================================================================
// DemonRateLimiter — rate-limit state tracker
// =============================================================================

/// Rate-limit state tracker for the Demon agent.
///
/// Tracks the last time a comment was posted and the running token tally so
/// that the Demon SubAgent thread can enforce kDemonMinCommentIntervalSec and
/// kDemonMaxTokenPercent limits without holding a lock on the hot path.
///
/// Intended for use on a single thread (the Demon jthread); not thread-safe.
struct DemonRateLimiter {
    /// Wall-clock time of the last posted comment.
    std::chrono::steady_clock::time_point last_comment_time{};

    /// Estimated tokens the Demon has consumed so far this session.
    std::size_t tokens_used{0};

    /// True when the Demon has never posted (so the first comment fires
    /// unconditionally as long as the token budget allows).
    bool first_comment{true};

    // -------------------------------------------------------------------------
    // is_allowed — check whether a comment may be posted right now.
    //
    // @param session_token_budget  Estimated total tokens for this session.
    //                              Pass 0 to disable the token-percent check
    //                              (e.g. in tests or when budget is unknown).
    //
    // Returns true when BOTH rate limits are satisfied:
    //   1. Time limit: at least kDemonMinCommentIntervalSec seconds since last
    //      comment (or first_comment == true).
    //   2. Token limit: tokens_used < (session_budget * kDemonMaxTokenPercent / 100)
    //      (skipped when session_token_budget == 0).
    // -------------------------------------------------------------------------
    [[nodiscard]] bool is_allowed(std::size_t session_token_budget = 0) const noexcept;

    // -------------------------------------------------------------------------
    // record_comment — update state after posting a comment.
    //
    // @param tokens_this_comment  Tokens used for this comment (prompt + reply).
    // -------------------------------------------------------------------------
    void record_comment(std::size_t tokens_this_comment) noexcept;
};

// =============================================================================
// Demon — Party Monster easter-egg sub-agent
//
// Blueprint contract: class Demon : public SubAgent (CPP 6.8)
//
// Demon is the conceptual sub-agent that wraps a SubAgent instance with a
// baked-in Party Monster AgentSpec and DemonRateLimiter rate limiting.
// The class is a static-methods-only factory; actual thread execution is
// handled by SubAgent (via AgentSupervisor::spawn).
// =============================================================================

/// Party Monster easter-egg sub-agent factory.
///
/// Provides:
///   Demon::spec()   — built-in AgentSpec with the glamour-ghoul system prompt
///   Demon::limiter  — DemonRateLimiter default instance for rate-limit checks
///
/// Usage (in DemonCmd):
///   auto spec = Demon::spec();
///   auto agent_id = supervisor.spawn(spec, prompt, parent_id, ct);
class Demon {
public:
    // -------------------------------------------------------------------------
    // spec() — return the baked-in Party Monster AgentSpec.
    //
    // The spec is built in-process (not loaded from disk) so /demon works even
    // without a ~/.batbox/agents/demon.md file.
    //
    // AgentSpec fields:
    //   name        = "demon"
    //   description = "Party Monster easter-egg — glamour-ghoul commentary"
    //   model       = nullopt (inherits session default)
    //   tools       = empty (the Demon observes, it does not act)
    //   prompt_body = Party Monster system prompt (see Demon.cpp)
    // -------------------------------------------------------------------------
    [[nodiscard]] static AgentSpec spec();

    // Demon is a static-methods-only factory; do not instantiate.
    Demon() = delete;
};

// =============================================================================
// Convenience free function — mirrors Demon::spec() for callers that already
// use the from_type / make_spec pattern from CPP S.14.
// =============================================================================

/// Return the built-in Demon AgentSpec.  Equivalent to Demon::spec().
[[nodiscard]] inline AgentSpec demon_make_spec() { return Demon::spec(); }

} // namespace batbox::agents
