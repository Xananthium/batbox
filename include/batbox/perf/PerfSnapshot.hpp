// include/batbox/perf/PerfSnapshot.hpp
// =============================================================================
// batbox::perf::PerfSnapshot — thread-safe container for three TUI-FLOW-T3
// latency measurements:
//
//   first_token_ms     — wall-time from on_submit() to first TokenAppended event
//                        arriving in ChatView (measures network + inference TTFT)
//   stream_to_paint_ms — wall-time from a TokenAppended event being posted to
//                        the completion of the ChatView::OnRender() that follows
//                        it (measures the event-queue + render pipeline latency)
//   frame_ms           — wall-time of the most recent ChatView::OnRender() body
//                        (measures per-frame render cost)
//
// Thread safety
// -------------
// All three values are stored as std::atomic<int> so they can be written by
// the inference worker thread (Conversation::run_turn, ChatView::OnEvent) and
// read by the FTXUI UI thread (InputBar::render_status_row) concurrently
// without a mutex.  std::memory_order_relaxed is used throughout — the values
// are purely diagnostic gauges; stale reads are acceptable.
//
// Usage
// -----
//   // Global singleton (set once from App.cpp before threads start):
//   batbox::perf::g_perf.set_first_token_ms(elapsed_ms);
//
//   // In InputBar::render_status_row() — zero-overhead when env var is unset:
//   if (perf_hud_enabled_) {
//       auto snap = batbox::perf::g_perf.snapshot();
//       // render snap.first_token_ms, snap.stream_to_paint_ms, snap.frame_ms
//   }
//
// Blueprint contract (TUI-FLOW-T3):
//   struct PerfSnapshot  — plain-old-data snapshot (copyable)
//   class  PerfStore     — atomic store/load wrapper
//   batbox::perf::PerfStore g_perf  — global singleton
// =============================================================================

#pragma once

#include <atomic>

namespace batbox::perf {

// =============================================================================
// PerfSnapshot — a plain snapshot of the three latency values (POD, copyable).
// =============================================================================

struct PerfSnapshot {
    int first_token_ms     {0};  ///< submit → first token, milliseconds
    int stream_to_paint_ms {0};  ///< token event posted → OnRender done, ms
    int frame_ms           {0};  ///< wall-time of last OnRender() body, ms
};

// =============================================================================
// PerfStore — atomic getter/setter wrapper around PerfSnapshot fields.
// =============================================================================

class PerfStore {
public:
    PerfStore() noexcept = default;

    // Non-copyable, non-movable (atomics are non-copyable).
    PerfStore(const PerfStore&)            = delete;
    PerfStore& operator=(const PerfStore&) = delete;
    PerfStore(PerfStore&&)                 = delete;
    PerfStore& operator=(PerfStore&&)      = delete;

    // -------------------------------------------------------------------------
    // Setters — called from worker / UI threads
    // -------------------------------------------------------------------------

    /// Record the first-token latency (submit → first token), milliseconds.
    void set_first_token_ms(int ms) noexcept;

    /// Record the stream-to-paint latency (token posted → OnRender done), ms.
    void set_stream_to_paint_ms(int ms) noexcept;

    /// Record the most recent per-frame render duration, milliseconds.
    void set_frame_ms(int ms) noexcept;

    // -------------------------------------------------------------------------
    // Snapshot — read all three values atomically (relaxed; display-only).
    // -------------------------------------------------------------------------

    /// Return a point-in-time snapshot of all three counters.
    /// Reads are relaxed; transient inconsistency between fields is acceptable
    /// because the values are purely informational.
    [[nodiscard]] PerfSnapshot snapshot() const noexcept;

private:
    std::atomic<int> first_token_ms_     {0};
    std::atomic<int> stream_to_paint_ms_ {0};
    std::atomic<int> frame_ms_           {0};
};

// =============================================================================
// Global singleton — one per process, initialised at static-init time.
// =============================================================================

/// Global PerfStore instance.  Accessed from Conversation.cpp (writer),
/// ChatView.cpp (writer), and InputBar.cpp (reader).
extern PerfStore g_perf;

// =============================================================================
// Global enable flag — set once at startup from BATBOX_PERF_HUD env var.
// =============================================================================

/// When false (default), ChatView skips all g_perf writes so the perf path
/// is zero-cost when the HUD is not shown.  Set to true by InputBar
/// constructor when BATBOX_PERF_HUD is non-empty and not "0"/"false".
extern std::atomic<bool> g_perf_enabled;

} // namespace batbox::perf
