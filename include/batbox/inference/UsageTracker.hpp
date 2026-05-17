// include/batbox/inference/UsageTracker.hpp
// =============================================================================
// batbox::inference::UsageTracker — per-session and per-turn usage accumulator.
//
// UsageTracker keeps two independent accumulators:
//   session_total()  — all turns since the tracker was constructed
//   turn_total()     — the most recently completed turn only
//
// After each inference call, the caller supplies a UsageDelta (from the wire
// response), and UsageTracker atomically updates both counters.  Calling
// reset_turn() clears the per-turn counters so the next add() starts fresh.
//
// UsageDelta is defined here (not in ChatResponse.hpp) because it is the
// canonical internal delta type shared by both the inference layer and the
// conversation layer.  ChatResponse.hpp re-uses this struct.
//
// Thread-safety:
//   add() and reset_turn() are safe to call from any thread.
//   session_total() and turn_total() return snapshots and are safe to read
//   concurrently.
//
// Build comment: see ModelPricing.hpp.
// =============================================================================

#pragma once

#include <atomic>

namespace batbox::inference {

// =============================================================================
// UsageDelta — token counts + locally computed cost for one inference call
// =============================================================================

/// Per-turn token and cost delta.  Accumulated by UsageTracker.
///
/// cost_usd is computed locally by UsageTracker::add() via ModelPricing::cost()
/// and is NEVER deserialised from the wire (it defaults to 0.0 until computed).
struct UsageDelta {
    int    prompt_tokens     = 0;
    int    completion_tokens = 0;
    int    total_tokens      = 0;
    double cost_usd          = 0.0;
};

// =============================================================================
// UsageTracker — thread-safe accumulator for session and turn usage
// =============================================================================

/// Accumulates prompt/completion token counts and USD cost across API calls.
///
/// Typical usage pattern:
/// @code
///   UsageTracker tracker;
///
///   // After each streaming turn completes:
///   UsageDelta delta;
///   delta.prompt_tokens     = usage_from_wire.prompt_tokens;
///   delta.completion_tokens = usage_from_wire.completion_tokens;
///   delta.total_tokens      = usage_from_wire.total_tokens;
///   tracker.add(delta);                // cost_usd filled in by add()
///
///   // Display per-turn cost:
///   auto turn = tracker.turn_total();  // snapshot of this turn
///   tracker.reset_turn();              // ready for next turn
///
///   // Display session total:
///   auto session = tracker.session_total();
/// @endcode
class UsageTracker {
public:
    UsageTracker() noexcept = default;

    // Non-copyable, non-movable (atomic members).
    UsageTracker(const UsageTracker&)            = delete;
    UsageTracker& operator=(const UsageTracker&) = delete;

    /// Atomically add delta to both session and turn accumulators.
    ///
    /// If delta.cost_usd is 0.0, the cost is computed via
    /// ModelPricing::cost(model, prompt_tokens, completion_tokens) using the
    /// model stored in the delta (if the overload below is used) or left as
    /// 0.0 for the basic overload.  Use add(delta, model) to request pricing.
    ///
    /// @param delta  Token counts (and optionally a pre-computed cost_usd).
    void add(const UsageDelta& delta);

    /// Snapshot of the current session totals (all turns since construction).
    [[nodiscard]] UsageDelta session_total() const;

    /// Snapshot of the current turn totals (since last reset_turn()).
    [[nodiscard]] UsageDelta turn_total() const;

    /// Reset the per-turn accumulators to zero.
    /// Call after consuming turn_total() to prepare for the next turn.
    void reset_turn() noexcept;

    /// Reset all accumulators (session + turn) to zero.
    void reset_all() noexcept;

private:
    // Session accumulators — never reset.
    std::atomic<int>    session_prompt_tokens_{0};
    std::atomic<int>    session_completion_tokens_{0};
    std::atomic<int>    session_total_tokens_{0};

    // Turn accumulators — reset by reset_turn().
    std::atomic<int>    turn_prompt_tokens_{0};
    std::atomic<int>    turn_completion_tokens_{0};
    std::atomic<int>    turn_total_tokens_{0};

    // Cost accumulators — stored as integer microUSD (multiply by 1e-6) to
    // enable lock-free atomic fetch_add without relying on float atomics.
    // 1 microUSD = $0.000001; max value ~$2147 before overflow, sufficient.
    std::atomic<long long> session_cost_micro_usd_{0};
    std::atomic<long long> turn_cost_micro_usd_{0};
};

} // namespace batbox::inference
