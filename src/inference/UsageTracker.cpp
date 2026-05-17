// src/inference/UsageTracker.cpp
// =============================================================================
// UsageTracker implementation.
//
// Token counts are accumulated with std::atomic<int>::fetch_add (lock-free on
// all mainstream platforms).  Cost is stored as integer microUSD
// (1 microUSD = $0.000001) using std::atomic<long long>::fetch_add to avoid
// the complexity of floating-point atomic compare-exchange loops.
//
// Precision:
//   Storing as microUSD gives 6 decimal places of precision.  At the max
//   long long value (~9.2e18 microUSD = $9.2 trillion), this is more than
//   sufficient for any real session.
// =============================================================================

#include <batbox/inference/UsageTracker.hpp>

namespace batbox::inference {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Convert a USD double to integer microUSD for atomic storage.
static long long to_micro_usd(double usd) noexcept {
    // Multiply by 1e6 and round to nearest integer.
    return static_cast<long long>(usd * 1'000'000.0 + 0.5);
}

/// Convert stored integer microUSD back to a USD double.
static double from_micro_usd(long long micro) noexcept {
    return static_cast<double>(micro) / 1'000'000.0;
}

// ---------------------------------------------------------------------------
// UsageTracker::add
// ---------------------------------------------------------------------------

void UsageTracker::add(const UsageDelta& delta) {
    // Accumulate token counts (atomic fetch_add, sequentially consistent).
    session_prompt_tokens_.fetch_add(delta.prompt_tokens,     std::memory_order_relaxed);
    session_completion_tokens_.fetch_add(delta.completion_tokens, std::memory_order_relaxed);
    session_total_tokens_.fetch_add(delta.total_tokens,       std::memory_order_relaxed);

    turn_prompt_tokens_.fetch_add(delta.prompt_tokens,        std::memory_order_relaxed);
    turn_completion_tokens_.fetch_add(delta.completion_tokens,std::memory_order_relaxed);
    turn_total_tokens_.fetch_add(delta.total_tokens,          std::memory_order_relaxed);

    // Accumulate cost.
    const long long cost_micro = to_micro_usd(delta.cost_usd);
    session_cost_micro_usd_.fetch_add(cost_micro, std::memory_order_relaxed);
    turn_cost_micro_usd_.fetch_add(cost_micro,    std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// UsageTracker::session_total
// ---------------------------------------------------------------------------

UsageDelta UsageTracker::session_total() const {
    UsageDelta d;
    d.prompt_tokens     = session_prompt_tokens_.load(std::memory_order_relaxed);
    d.completion_tokens = session_completion_tokens_.load(std::memory_order_relaxed);
    d.total_tokens      = session_total_tokens_.load(std::memory_order_relaxed);
    d.cost_usd          = from_micro_usd(session_cost_micro_usd_.load(std::memory_order_relaxed));
    return d;
}

// ---------------------------------------------------------------------------
// UsageTracker::turn_total
// ---------------------------------------------------------------------------

UsageDelta UsageTracker::turn_total() const {
    UsageDelta d;
    d.prompt_tokens     = turn_prompt_tokens_.load(std::memory_order_relaxed);
    d.completion_tokens = turn_completion_tokens_.load(std::memory_order_relaxed);
    d.total_tokens      = turn_total_tokens_.load(std::memory_order_relaxed);
    d.cost_usd          = from_micro_usd(turn_cost_micro_usd_.load(std::memory_order_relaxed));
    return d;
}

// ---------------------------------------------------------------------------
// UsageTracker::reset_turn
// ---------------------------------------------------------------------------

void UsageTracker::reset_turn() noexcept {
    turn_prompt_tokens_.store(0,     std::memory_order_relaxed);
    turn_completion_tokens_.store(0, std::memory_order_relaxed);
    turn_total_tokens_.store(0,      std::memory_order_relaxed);
    turn_cost_micro_usd_.store(0LL,  std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// UsageTracker::reset_all
// ---------------------------------------------------------------------------

void UsageTracker::reset_all() noexcept {
    session_prompt_tokens_.store(0,     std::memory_order_relaxed);
    session_completion_tokens_.store(0, std::memory_order_relaxed);
    session_total_tokens_.store(0,      std::memory_order_relaxed);
    session_cost_micro_usd_.store(0LL,  std::memory_order_relaxed);
    reset_turn();
}

} // namespace batbox::inference
