// tests/unit/test_perf_snapshot.cpp
// ---------------------------------------------------------------------------
// doctest suite for batbox::perf::PerfStore + PerfSnapshot (TUI-FLOW-T3)
//
// Coverage:
//   1. Default snapshot values are zero.
//   2. set_first_token_ms stores and snapshot retrieves.
//   3. set_stream_to_paint_ms stores and snapshot retrieves.
//   4. set_frame_ms stores and snapshot retrieves.
//   5. All three fields updated independently — no cross-contamination.
//   6. Concurrent writes from multiple threads do not crash (atomics safe).
//   7. Snapshot after concurrent writes returns valid (non-negative) values.
//   8. g_perf global singleton accessible from multiple translation units.
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/perf/PerfSnapshot.hpp>

#include <atomic>
#include <thread>
#include <vector>

using batbox::perf::PerfStore;
using batbox::perf::PerfSnapshot;

// ---------------------------------------------------------------------------
// Use a local PerfStore for most tests to avoid polluting g_perf.
// ---------------------------------------------------------------------------

TEST_CASE("PerfStore: default snapshot is all zeros") {
    PerfStore store;
    auto snap = store.snapshot();
    CHECK(snap.first_token_ms     == 0);
    CHECK(snap.stream_to_paint_ms == 0);
    CHECK(snap.frame_ms           == 0);
}

TEST_CASE("PerfStore: set_first_token_ms stores and retrieves") {
    PerfStore store;
    store.set_first_token_ms(123);
    auto snap = store.snapshot();
    CHECK(snap.first_token_ms == 123);
    // Other fields remain untouched.
    CHECK(snap.stream_to_paint_ms == 0);
    CHECK(snap.frame_ms           == 0);
}

TEST_CASE("PerfStore: set_stream_to_paint_ms stores and retrieves") {
    PerfStore store;
    store.set_stream_to_paint_ms(45);
    auto snap = store.snapshot();
    CHECK(snap.stream_to_paint_ms == 45);
    CHECK(snap.first_token_ms == 0);
    CHECK(snap.frame_ms       == 0);
}

TEST_CASE("PerfStore: set_frame_ms stores and retrieves") {
    PerfStore store;
    store.set_frame_ms(16);
    auto snap = store.snapshot();
    CHECK(snap.frame_ms == 16);
    CHECK(snap.first_token_ms     == 0);
    CHECK(snap.stream_to_paint_ms == 0);
}

TEST_CASE("PerfStore: all three fields updated independently") {
    PerfStore store;
    store.set_first_token_ms(500);
    store.set_stream_to_paint_ms(30);
    store.set_frame_ms(8);
    auto snap = store.snapshot();
    CHECK(snap.first_token_ms     == 500);
    CHECK(snap.stream_to_paint_ms == 30);
    CHECK(snap.frame_ms           == 8);
}

TEST_CASE("PerfStore: overwrite updates stored value") {
    PerfStore store;
    store.set_first_token_ms(100);
    store.set_first_token_ms(200);
    auto snap = store.snapshot();
    CHECK(snap.first_token_ms == 200);
}

TEST_CASE("PerfStore: concurrent writes from multiple threads do not crash") {
    // This test validates that the atomic implementation is contention-safe.
    PerfStore store;
    constexpr int kThreads = 8;
    constexpr int kIter    = 10000;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    std::atomic<int> done_count{0};

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&store, &done_count, t]() {
            for (int i = 0; i < kIter; ++i) {
                // Interleave writes and reads to stress the atomics.
                store.set_first_token_ms(t * 1000 + i);
                store.set_stream_to_paint_ms(i % 100);
                store.set_frame_ms((t + i) % 50);
                auto snap = store.snapshot();
                // Values must be non-negative (atomics never produce negative
                // results for non-negative inputs, but we guard anyway).
                CHECK(snap.first_token_ms     >= 0);
                CHECK(snap.stream_to_paint_ms >= 0);
                CHECK(snap.frame_ms           >= 0);
            }
            ++done_count;
        });
    }

    for (auto& th : threads) th.join();
    CHECK(done_count.load() == kThreads);
}

TEST_CASE("g_perf global singleton is accessible") {
    // Write via global singleton; read it back.
    batbox::perf::g_perf.set_first_token_ms(999);
    auto snap = batbox::perf::g_perf.snapshot();
    CHECK(snap.first_token_ms == 999);
    // Reset to 0 to leave g_perf clean for subsequent tests.
    batbox::perf::g_perf.set_first_token_ms(0);
    batbox::perf::g_perf.set_stream_to_paint_ms(0);
    batbox::perf::g_perf.set_frame_ms(0);
}
