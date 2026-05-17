// tests/unit/test_conversation_cancel.cpp
// ---------------------------------------------------------------------------
// Unit tests for TUI-FIX-T3: Esc-to-interrupt cancel path.
//
// Tests:
//   1. CancelSource::request_stop() causes CancelToken::is_cancelled() immediately.
//   2. A child token is cancelled when the parent source fires.
//   3. cancel fires within 200ms wall time (latency gate).
//   4. Multiple request_stop() calls are idempotent.
//   5. A null (default-constructed) CancelToken is never cancelled.
//   6. Cancellation propagates through a child chain (grandchild).
//
// Build + run standalone:
//   c++ -std=c++20 \
//       -I include \
//       tests/unit/test_conversation_cancel.cpp \
//       src/core/CancelToken.cpp \
//       -o /tmp/test_conversation_cancel && /tmp/test_conversation_cancel
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/core/CancelToken.hpp>

#include <atomic>
#include <chrono>
#include <thread>

using batbox::CancelSource;
using batbox::CancelToken;

// ---------------------------------------------------------------------------
// Helper: wall-clock elapsed since a time_point in milliseconds.
// ---------------------------------------------------------------------------
namespace {

template<typename Clock>
double elapsed_ms(std::chrono::time_point<Clock> start) {
    auto now = Clock::now();
    return std::chrono::duration<double, std::milli>(now - start).count();
}

} // anonymous namespace

// ===========================================================================
// Test 1 — Basic cancellation: request_stop fires is_cancelled immediately
// ===========================================================================
TEST_CASE("CancelToken: request_stop immediately fires is_cancelled") {
    auto [src, tok] = CancelToken::make_root();

    CHECK_FALSE(tok.is_cancelled());
    CHECK_FALSE(src.stop_requested());

    src.request_stop();

    CHECK(tok.is_cancelled());
    CHECK(src.stop_requested());
}

// ===========================================================================
// Test 2 — Child token inherits parent cancellation
// ===========================================================================
TEST_CASE("CancelToken: child token cancels when parent source fires") {
    auto [parent_src, parent_tok] = CancelToken::make_root();
    auto [child_src, child_tok]   = parent_tok.child();

    CHECK_FALSE(child_tok.is_cancelled());

    parent_src.request_stop();

    CHECK(child_tok.is_cancelled());
    // Child source fires as well (it is linked)
    // Note: child_src.stop_requested() may or may not be set depending on
    // implementation — what matters is child_tok.is_cancelled().
}

// ===========================================================================
// Test 3 — Latency gate: cancel fires within 200ms
//
// Simulates the TUI-FIX-T3 requirement: pressing Esc must cancel the
// in-flight stream within 200ms.  The test fires request_stop() from a
// background thread after a 5ms delay and checks that is_cancelled() becomes
// true before 200ms elapse.
// ===========================================================================
TEST_CASE("CancelToken: is_cancelled observable within 200ms after request_stop") {
    auto [src, tok] = CancelToken::make_root();

    const auto start = std::chrono::steady_clock::now();

    // Simulate the UI thread pressing Esc after ~5ms.
    std::thread([&src]() mutable {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        src.request_stop();
    }).detach();

    // Poll (simulates the SSE reader checking the token).
    // We poll at most 200ms, sleeping 1ms per iteration.
    bool cancelled = false;
    for (int i = 0; i < 200 && !cancelled; ++i) {
        if (tok.is_cancelled()) {
            cancelled = true;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    const double ms = elapsed_ms(start);
    CHECK(cancelled);
    CHECK(ms < 200.0);
}

// ===========================================================================
// Test 4 — Idempotency: multiple request_stop() calls are safe
// ===========================================================================
TEST_CASE("CancelToken: multiple request_stop calls are idempotent") {
    auto [src, tok] = CancelToken::make_root();

    src.request_stop();
    CHECK(tok.is_cancelled());

    // Second and third calls must not throw or flip is_cancelled back to false.
    src.request_stop();
    src.request_stop();
    CHECK(tok.is_cancelled());
}

// ===========================================================================
// Test 5 — Default-constructed CancelToken is never cancelled
// ===========================================================================
TEST_CASE("CancelToken: default-constructed token is never cancelled") {
    CancelToken empty;
    CHECK_FALSE(empty.is_cancelled());
}

// ===========================================================================
// Test 6 — Grandchild cancellation: child-of-child also fires
// ===========================================================================
TEST_CASE("CancelToken: grandchild token cancels when root source fires") {
    auto [root_src, root_tok]       = CancelToken::make_root();
    auto [child_src, child_tok]     = root_tok.child();
    auto [gc_src,    gc_tok]        = child_tok.child();

    CHECK_FALSE(gc_tok.is_cancelled());

    root_src.request_stop();

    CHECK(child_tok.is_cancelled());
    CHECK(gc_tok.is_cancelled());
}

// ===========================================================================
// Test 7 — Independent child cancellation: child can cancel without parent
// ===========================================================================
TEST_CASE("CancelToken: child source fires child without cancelling parent") {
    auto [root_src, root_tok]   = CancelToken::make_root();
    auto [child_src, child_tok] = root_tok.child();

    CHECK_FALSE(root_tok.is_cancelled());
    CHECK_FALSE(child_tok.is_cancelled());

    child_src.request_stop();

    CHECK(child_tok.is_cancelled());
    // Root token must NOT be cancelled by the child.
    CHECK_FALSE(root_tok.is_cancelled());
}

// ===========================================================================
// Test 8 — Concurrent cancel: atomic visibility across threads
//
// Spawns N reader threads that all poll is_cancelled().  The main thread
// calls request_stop() and expects all readers to observe cancellation.
// ===========================================================================
TEST_CASE("CancelToken: concurrent readers all observe cancellation") {
    constexpr int kReaderCount = 8;
    auto [src, tok] = CancelToken::make_root();

    std::atomic<int> observed_count{0};
    std::vector<std::thread> readers;
    readers.reserve(kReaderCount);

    std::atomic<bool> start_flag{false};

    for (int i = 0; i < kReaderCount; ++i) {
        readers.emplace_back([&tok, &observed_count, &start_flag]() {
            // Busy-wait until main thread fires start.
            while (!start_flag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            // Poll up to 200ms.
            for (int j = 0; j < 200; ++j) {
                if (tok.is_cancelled()) {
                    observed_count.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }

    // Fire start and immediately request_stop.
    start_flag.store(true, std::memory_order_release);
    src.request_stop();

    for (auto& t : readers) t.join();

    CHECK(observed_count.load() == kReaderCount);
}
