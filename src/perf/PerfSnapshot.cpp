// src/perf/PerfSnapshot.cpp
// =============================================================================
// Implementation of batbox::perf::PerfStore and the g_perf global singleton.
//
// All loads and stores use std::memory_order_relaxed because the three latency
// counters are display-only gauges.  A slightly stale read in InputBar is
// acceptable — we optimise for zero contention over strict freshness.
// =============================================================================

#include <batbox/perf/PerfSnapshot.hpp>

namespace batbox::perf {

// =============================================================================
// Global singleton — zero-initialised at static-init time.
// =============================================================================

PerfStore g_perf;

// =============================================================================
// PerfStore — setters
// =============================================================================

void PerfStore::set_first_token_ms(int ms) noexcept {
    first_token_ms_.store(ms, std::memory_order_relaxed);
}

void PerfStore::set_stream_to_paint_ms(int ms) noexcept {
    stream_to_paint_ms_.store(ms, std::memory_order_relaxed);
}

void PerfStore::set_frame_ms(int ms) noexcept {
    frame_ms_.store(ms, std::memory_order_relaxed);
}

// =============================================================================
// PerfStore — snapshot
// =============================================================================

PerfSnapshot PerfStore::snapshot() const noexcept {
    PerfSnapshot s;
    s.first_token_ms     = first_token_ms_.load(std::memory_order_relaxed);
    s.stream_to_paint_ms = stream_to_paint_ms_.load(std::memory_order_relaxed);
    s.frame_ms           = frame_ms_.load(std::memory_order_relaxed);
    return s;
}

} // namespace batbox::perf
