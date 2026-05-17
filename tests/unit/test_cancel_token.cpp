// ---------------------------------------------------------------------------
// tests/unit/test_cancel_token.cpp
//
// Unit tests for batbox::CancelToken, batbox::CancelSource, and
// batbox::combine_tokens using the doctest framework.
//
// Build:
//   c++ -std=c++20 -Iinclude \
//       -I<path-to-doctest> \
//       tests/unit/test_cancel_token.cpp \
//       src/core/CancelToken.cpp \
//       -lpthread \
//       -o /tmp/test_cancel_token
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "batbox/core/CancelToken.hpp"

#include <atomic>
#include <chrono>
#include <thread>

using namespace batbox;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Basic: root creation
// ---------------------------------------------------------------------------
TEST_CASE("make_root produces a live source and token") {
    auto [src, tok] = CancelToken::make_root();
    CHECK_FALSE(src.stop_requested());
    CHECK_FALSE(tok.is_cancelled());
    CHECK_FALSE(tok.stop_requested());
}

TEST_CASE("request_stop marks both source and token cancelled") {
    auto [src, tok] = CancelToken::make_root();
    src.request_stop();
    CHECK(src.stop_requested());
    CHECK(tok.is_cancelled());
}

TEST_CASE("default-constructed CancelToken is never cancelled") {
    CancelToken tok;
    CHECK_FALSE(tok.is_cancelled());
}

// ---------------------------------------------------------------------------
// CancelSource::token() vends independent handles sharing the same signal
// ---------------------------------------------------------------------------
TEST_CASE("multiple tokens from same source all fire together") {
    CancelSource src;
    CancelToken t1 = src.token();
    CancelToken t2 = src.token();
    CHECK_FALSE(t1.is_cancelled());
    CHECK_FALSE(t2.is_cancelled());
    src.request_stop();
    CHECK(t1.is_cancelled());
    CHECK(t2.is_cancelled());
}

// ---------------------------------------------------------------------------
// throw_if_cancelled
// ---------------------------------------------------------------------------
TEST_CASE("throw_if_cancelled is silent when not cancelled") {
    auto [src, tok] = CancelToken::make_root();
    CHECK_NOTHROW(tok.throw_if_cancelled());
}

TEST_CASE("throw_if_cancelled throws CancelledException when cancelled") {
    auto [src, tok] = CancelToken::make_root();
    src.request_stop();
    CHECK_THROWS_AS(tok.throw_if_cancelled(), CancelledException);
}

TEST_CASE("CancelledException is-a std::runtime_error") {
    CancelledException ex("test cancel");
    const std::runtime_error& base = ex;
    CHECK(std::string(base.what()) == "test cancel");
}

// ---------------------------------------------------------------------------
// on_cancel callback
// ---------------------------------------------------------------------------
TEST_CASE("on_cancel callback fires when source is stopped") {
    auto [src, tok] = CancelToken::make_root();
    std::atomic<int> count{0};
    auto handle = tok.on_cancel([&] { count.fetch_add(1, std::memory_order_relaxed); });
    CHECK(count.load() == 0);
    src.request_stop();
    CHECK(count.load() == 1);
}

TEST_CASE("on_cancel callback fires immediately if already cancelled") {
    auto [src, tok] = CancelToken::make_root();
    src.request_stop();
    std::atomic<int> count{0};
    auto handle = tok.on_cancel([&] { count.fetch_add(1, std::memory_order_relaxed); });
    // std::stop_callback fires immediately on construction if already stopped.
    CHECK(count.load() == 1);
}

TEST_CASE("multiple on_cancel callbacks all fire") {
    auto [src, tok] = CancelToken::make_root();
    std::atomic<int> a{0}, b{0};
    auto h1 = tok.on_cancel([&] { a++; });
    auto h2 = tok.on_cancel([&] { b++; });
    src.request_stop();
    CHECK(a.load() == 1);
    CHECK(b.load() == 1);
}

TEST_CASE("on_cancel handle deregisters callback when destroyed") {
    auto [src, tok] = CancelToken::make_root();
    std::atomic<int> count{0};
    {
        auto handle = tok.on_cancel([&] { count++; });
        // handle destroyed here
    }
    src.request_stop();
    CHECK(count.load() == 0);
}

// ---------------------------------------------------------------------------
// child() propagation
// ---------------------------------------------------------------------------
TEST_CASE("child token fires when parent fires") {
    auto [parent_src, parent_tok] = CancelToken::make_root();
    auto [child_src, child_tok] = parent_tok.child();

    CHECK_FALSE(child_tok.is_cancelled());
    parent_src.request_stop();
    CHECK(child_tok.is_cancelled());
}

TEST_CASE("child token fires when child source fires independently") {
    auto [parent_src, parent_tok] = CancelToken::make_root();
    auto [child_src, child_tok] = parent_tok.child();

    CHECK_FALSE(parent_tok.is_cancelled());
    child_src.request_stop();
    // Only the child should be cancelled.
    CHECK(child_tok.is_cancelled());
    CHECK_FALSE(parent_tok.is_cancelled());
}

TEST_CASE("parent cancel cascades to all children") {
    auto [root_src, root_tok] = CancelToken::make_root();
    auto [c1_src, c1_tok] = root_tok.child();
    auto [c2_src, c2_tok] = root_tok.child();

    root_src.request_stop();
    CHECK(c1_tok.is_cancelled());
    CHECK(c2_tok.is_cancelled());
}

TEST_CASE("grandchild cascade: root cancel propagates two levels deep") {
    auto [root_src, root_tok] = CancelToken::make_root();
    auto [child_src, child_tok] = root_tok.child();
    auto [grand_src, grand_tok] = child_tok.child();

    CHECK_FALSE(grand_tok.is_cancelled());
    root_src.request_stop();
    // Child fires; grandchild should also fire because its parent fires.
    CHECK(child_tok.is_cancelled());
    CHECK(grand_tok.is_cancelled());
}

// ---------------------------------------------------------------------------
// combine_tokens
// ---------------------------------------------------------------------------
TEST_CASE("combine_tokens fires when first token fires") {
    auto [src_a, tok_a] = CancelToken::make_root();
    auto [src_b, tok_b] = CancelToken::make_root();
    CancelToken combined = combine_tokens(std::move(tok_a), std::move(tok_b));

    CHECK_FALSE(combined.is_cancelled());
    src_a.request_stop();
    CHECK(combined.is_cancelled());
}

TEST_CASE("combine_tokens fires when second token fires") {
    auto [src_a, tok_a] = CancelToken::make_root();
    auto [src_b, tok_b] = CancelToken::make_root();
    CancelToken combined = combine_tokens(std::move(tok_a), std::move(tok_b));

    src_b.request_stop();
    CHECK(combined.is_cancelled());
}

TEST_CASE("combine_tokens does not fire when neither source fires") {
    auto [src_a, tok_a] = CancelToken::make_root();
    auto [src_b, tok_b] = CancelToken::make_root();
    CancelToken combined = combine_tokens(std::move(tok_a), std::move(tok_b));
    CHECK_FALSE(combined.is_cancelled());
}

// ---------------------------------------------------------------------------
// Multi-thread scenario:
// Child token observed from another thread; parent cancels from this thread.
// ---------------------------------------------------------------------------
TEST_CASE("multi-thread: child token observed from another thread sees parent cancel") {
    auto [parent_src, parent_tok] = CancelToken::make_root();
    auto [child_src, child_tok] = parent_tok.child();

    std::atomic<bool> observer_saw_cancel{false};
    std::atomic<bool> observer_ready{false};

    // Observer thread: spins on child_tok.is_cancelled() until it sees true.
    std::thread observer([&] {
        observer_ready.store(true, std::memory_order_release);
        // Busy-wait with yield (test, not production code).
        while (!child_tok.is_cancelled()) {
            std::this_thread::yield();
        }
        observer_saw_cancel.store(true, std::memory_order_release);
    });

    // Wait for observer to be ready.
    while (!observer_ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    // Trigger cancellation from the parent on this thread.
    parent_src.request_stop();

    observer.join();
    CHECK(observer_saw_cancel.load());
}

TEST_CASE("multi-thread: on_cancel callback fired from cancelling thread is visible") {
    auto [src, tok] = CancelToken::make_root();
    std::atomic<int> callback_count{0};

    auto h1 = tok.on_cancel([&] {
        callback_count.fetch_add(1, std::memory_order_release);
    });

    // Cancel from a separate thread.
    std::thread canceller([&] {
        src.request_stop();
    });
    canceller.join();

    CHECK(callback_count.load(std::memory_order_acquire) == 1);
}

TEST_CASE("multi-thread: combine_tokens observable across threads") {
    auto [src_a, tok_a] = CancelToken::make_root();
    auto [src_b, tok_b] = CancelToken::make_root();
    CancelToken combined = combine_tokens(std::move(tok_a), std::move(tok_b));

    std::atomic<bool> seen{false};
    std::thread watcher([&] {
        while (!combined.is_cancelled()) {
            std::this_thread::yield();
        }
        seen.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(1ms); // let watcher spin first
    src_b.request_stop();
    watcher.join();
    CHECK(seen.load());
}

// ---------------------------------------------------------------------------
// Move semantics
// ---------------------------------------------------------------------------
TEST_CASE("CancelToken is movable and moved-from is safely destroyable") {
    auto [src, tok] = CancelToken::make_root();
    CancelToken tok2 = std::move(tok);
    // tok is moved-from; tok2 holds the state.
    CHECK_FALSE(tok2.is_cancelled());
    src.request_stop();
    CHECK(tok2.is_cancelled());
    // tok is safely destroyable (no crash).
}

TEST_CASE("CancelSource is movable") {
    CancelSource src;
    CancelToken tok = src.token();
    CancelSource src2 = std::move(src);
    src2.request_stop();
    CHECK(tok.is_cancelled());
}
