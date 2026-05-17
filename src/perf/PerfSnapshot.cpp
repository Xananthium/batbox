// src/perf/PerfSnapshot.cpp
// =============================================================================
// Implementation of batbox::perf::PerfStore and the g_perf global singleton.
//
// All loads and stores use std::memory_order_relaxed because the three latency
// counters are display-only gauges.  A slightly stale read in InputBar is
// acceptable — we optimise for zero contention over strict freshness.
// =============================================================================

#include <batbox/perf/PerfSnapshot.hpp>

#include <cstdlib>    // std::getenv
#include <string_view>

namespace batbox::perf {

// =============================================================================
// Global singleton — zero-initialised at static-init time.
// =============================================================================

PerfStore g_perf;

// Enable flag — initialised once at static-init time from BATBOX_PERF_HUD.
// Using a lambda to perform env-var inspection before threads start.
std::atomic<bool> g_perf_enabled{[]() -> bool {
    const char* env = std::getenv("BATBOX_PERF_HUD");
    if (!env || env[0] == '\0') return false;
    std::string_view sv(env);
    return (sv != "0" && sv != "false");
}()};

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
