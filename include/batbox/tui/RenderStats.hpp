// include/batbox/tui/RenderStats.hpp
// =============================================================================
// RenderStats — atomic ring buffer of frame-render timestamps.
//
// Purpose
// -------
// Provides a lightweight, lock-free ring buffer that the ScreenManager render
// loop populates (one entry per frame) when the BATBOX_PERF=1 environment
// variable is set at process startup.  Integration tests query the buffer to
// verify refresh-rate stability without modifying test-unrelated production
// code paths.
//
// Usage
// -----
//   // In render loop (BATBOX_PERF=1 path):
//   RenderStats::global().push_frame_now();
//
//   // In tests / --dump-frame-stats path:
//   auto samples = RenderStats::global().drain();
//   // samples is a vector<steady_clock::time_point> in chronological order.
//
// Thread safety
// -------------
// push_frame_now() is safe to call from the UI thread only (the FTXUI render
// loop runs on one thread).  drain() may be called from any thread but is
// intended for test/diagnostic teardown use (no concurrent writers during drain).
//
// Ring buffer capacity
// --------------------
// kCapacity = 256 frames (~8.5 s at 30 Hz).  Older samples are silently
// overwritten.  Tests that want stable measurements should drain after a
// fixed observation window.
// =============================================================================

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace batbox::tui {

/// Atomic ring buffer of frame-render timestamps.
///
/// Singleton accessed via RenderStats::global().
/// The render loop calls push_frame_now() once per rendered frame when
/// BATBOX_PERF=1 is set.
class RenderStats {
public:
    /// Number of timestamp slots in the ring buffer.
    static constexpr std::size_t kCapacity = 256;

    using Clock     = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    // -------------------------------------------------------------------------
    // Singleton accessor
    // -------------------------------------------------------------------------

    /// Return the process-global RenderStats instance.
    ///
    /// Constructed on first call; lives until process exit.
    [[nodiscard]] static RenderStats& global() noexcept;

    // -------------------------------------------------------------------------
    // Writer API (UI thread)
    // -------------------------------------------------------------------------

    /// Record the current time as a new frame timestamp.
    ///
    /// Overwrites the oldest entry when the ring is full.
    /// MUST be called from the UI thread (no concurrent writers assumed).
    void push_frame_now() noexcept;

    // -------------------------------------------------------------------------
    // Reader API
    // -------------------------------------------------------------------------

    /// Return all recorded timestamps in chronological order and reset the ring.
    ///
    /// Intended for test teardown or --dump-frame-stats output.
    /// Not safe to call concurrently with push_frame_now().
    [[nodiscard]] std::vector<TimePoint> drain() noexcept;

    /// Return the number of samples currently in the ring (0..kCapacity).
    [[nodiscard]] std::size_t count() const noexcept;

    // -------------------------------------------------------------------------
    // Enable / disable (checked by the render loop)
    // -------------------------------------------------------------------------

    /// True when BATBOX_PERF=1 is set in the environment at startup.
    [[nodiscard]] static bool is_enabled() noexcept;

    // Non-copyable (singleton).
    RenderStats(const RenderStats&)            = delete;
    RenderStats& operator=(const RenderStats&) = delete;
    RenderStats(RenderStats&&)                 = delete;
    RenderStats& operator=(RenderStats&&)      = delete;

private:
    RenderStats() noexcept = default;

    // Ring buffer of raw nanoseconds since epoch (steady_clock).
    // Index head_ is where the NEXT write will land.
    std::array<int64_t, kCapacity> buf_{};
    std::size_t head_{0};    ///< next write slot (wraps mod kCapacity)
    std::size_t size_{0};    ///< number of valid entries (saturates at kCapacity)
};

} // namespace batbox::tui
