// tests/unit/test_mcp_status_poller.cpp
// ---------------------------------------------------------------------------
// doctest suite for batbox::app::McpStatusPoller (TUI-FLOW-T11).
//
// Tests:
//   1. Callback fires when failed count changes from 0 to N.
//   2. Callback fires again when count changes back to 0.
//   3. Callback fires when count changes from N to M (N != M).
//   4. Callback does NOT fire when count is unchanged between polls.
//   5. Poller shuts down cleanly within 200ms (no hang on destruction).
//   6. Callback receives correct count value.
//
// The tick_interval parameter is set to 50ms so tests complete quickly without
// sleeping for the default 1s poll interval.
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/app/McpStatusPoller.hpp>
#include <batbox/mcp/McpServerRegistry.hpp>
#include <batbox/mcp/IMcpTransport.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/core/Json.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

using namespace batbox;
using namespace batbox::app;
using namespace batbox::mcp;
using namespace std::chrono_literals;

// ============================================================================
// MockTransport — minimal transport stub for registry injection
// ============================================================================
//
// Reports healthy() based on the value stored in the shared atomic flag.
// Tests control the health state by flipping the flag.

class MockTransport final : public IMcpTransport {
public:
    explicit MockTransport(std::shared_ptr<std::atomic<bool>> healthy_flag)
        : healthy_flag_(std::move(healthy_flag)) {}

    Result<void> start(CancelToken) override { return {}; }
    void         stop()            override {}

    [[nodiscard]] bool healthy() const override {
        return healthy_flag_->load(std::memory_order_relaxed);
    }

    [[nodiscard]] Result<Json> request(std::string, Json, CancelToken) override {
        return Err(std::string("not implemented"));
    }
    [[nodiscard]] Result<void> notify(std::string, Json) override {
        return Err(std::string("not implemented"));
    }
    void on_notification(std::function<void(std::string, Json)>) override {}

private:
    std::shared_ptr<std::atomic<bool>> healthy_flag_;
};

// ============================================================================
// Helpers
// ============================================================================

/// Tick interval for all tests — short enough to be fast, long enough to be
/// stable on a loaded CI machine.
static constexpr auto kTick = 50ms;

/// How long to wait for a callback to arrive.
static constexpr auto kTimeout = 2s;

/// Wait up to kTimeout for predicate to become true, polling at ~5ms intervals.
template <typename Pred>
static bool wait_for_condition(Pred pred, std::chrono::milliseconds timeout = kTimeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return pred(); // one final check
}

// ============================================================================
// TEST SUITE 1 — Callback firing behaviour
// ============================================================================

TEST_SUITE("McpStatusPoller — callback firing") {

    TEST_CASE("callback fires when count changes from 0 to 1") {
        // Arrange: one server, initially healthy.
        auto flag = std::make_shared<std::atomic<bool>>(true);

        McpServerRegistry reg;
        reg.add_transport("srv1", std::make_unique<MockTransport>(flag));

        std::atomic<int> received_count{-1};
        std::atomic<int> call_count{0};

        McpStatusPoller poller(
            &reg,
            [&](int n) {
                received_count.store(n, std::memory_order_relaxed);
                call_count.fetch_add(1, std::memory_order_relaxed);
            },
            kTick);

        // Wait for the initial poll to fire (count is 0 — first observation
        // differs from -1 sentinel so callback fires).
        CHECK(wait_for_condition([&]{ return call_count.load() >= 1; }));
        CHECK(received_count.load() == 0);

        // Act: make the transport unhealthy.
        flag->store(false, std::memory_order_relaxed);

        // Assert: callback fires with count = 1.
        int calls_before = call_count.load();
        CHECK(wait_for_condition([&]{ return call_count.load() > calls_before; }));
        CHECK(received_count.load() == 1);
    }

    TEST_CASE("callback fires when count changes from N back to 0") {
        // Arrange: one unhealthy server.
        auto flag = std::make_shared<std::atomic<bool>>(false);

        McpServerRegistry reg;
        reg.add_transport("srv1", std::make_unique<MockTransport>(flag));

        std::atomic<int> received_count{-1};
        std::atomic<int> call_count{0};

        McpStatusPoller poller(
            &reg,
            [&](int n) {
                received_count.store(n, std::memory_order_relaxed);
                call_count.fetch_add(1, std::memory_order_relaxed);
            },
            kTick);

        // Wait for first poll (count=1).
        CHECK(wait_for_condition([&]{ return call_count.load() >= 1; }));
        CHECK(received_count.load() == 1);

        // Act: restore health.
        flag->store(true, std::memory_order_relaxed);

        // Assert: callback fires with count = 0.
        int calls_before = call_count.load();
        CHECK(wait_for_condition([&]{ return call_count.load() > calls_before; }));
        CHECK(received_count.load() == 0);
    }

    TEST_CASE("callback fires when count changes from 1 to 2") {
        // Arrange: two servers, one unhealthy.
        auto flag1 = std::make_shared<std::atomic<bool>>(false); // unhealthy
        auto flag2 = std::make_shared<std::atomic<bool>>(true);  // healthy

        McpServerRegistry reg;
        reg.add_transport("srv1", std::make_unique<MockTransport>(flag1));
        reg.add_transport("srv2", std::make_unique<MockTransport>(flag2));

        std::atomic<int> received_count{-1};
        std::atomic<int> call_count{0};

        McpStatusPoller poller(
            &reg,
            [&](int n) {
                received_count.store(n, std::memory_order_relaxed);
                call_count.fetch_add(1, std::memory_order_relaxed);
            },
            kTick);

        // Wait for first poll (count=1).
        CHECK(wait_for_condition([&]{ return call_count.load() >= 1; }));
        CHECK(received_count.load() == 1);

        // Act: make second server unhealthy too.
        flag2->store(false, std::memory_order_relaxed);

        // Assert: callback fires with count = 2.
        int calls_before = call_count.load();
        CHECK(wait_for_condition([&]{ return call_count.load() > calls_before; }));
        CHECK(received_count.load() == 2);
    }

    TEST_CASE("callback does NOT fire when count is unchanged between polls") {
        // Arrange: one healthy server (count stays at 0).
        auto flag = std::make_shared<std::atomic<bool>>(true);

        McpServerRegistry reg;
        reg.add_transport("srv1", std::make_unique<MockTransport>(flag));

        std::atomic<int> call_count{0};

        McpStatusPoller poller(
            &reg,
            [&](int) { call_count.fetch_add(1, std::memory_order_relaxed); },
            kTick);

        // Wait for the first poll (initial count=0 fires once).
        CHECK(wait_for_condition([&]{ return call_count.load() >= 1; }));

        // Count the calls after the initial fire.
        int snapshot = call_count.load();

        // Wait for 3 more ticks and verify no additional calls.
        std::this_thread::sleep_for(kTick * 3 + 20ms);
        CHECK(call_count.load() == snapshot); // no additional fires
    }

    TEST_CASE("callback receives the correct failed count value") {
        // Arrange: three servers, two unhealthy.
        auto f1 = std::make_shared<std::atomic<bool>>(false);
        auto f2 = std::make_shared<std::atomic<bool>>(false);
        auto f3 = std::make_shared<std::atomic<bool>>(true);

        McpServerRegistry reg;
        reg.add_transport("a", std::make_unique<MockTransport>(f1));
        reg.add_transport("b", std::make_unique<MockTransport>(f2));
        reg.add_transport("c", std::make_unique<MockTransport>(f3));

        std::atomic<int> received{-1};
        std::atomic<int> calls{0};

        McpStatusPoller poller(
            &reg,
            [&](int n) {
                received.store(n, std::memory_order_relaxed);
                calls.fetch_add(1, std::memory_order_relaxed);
            },
            kTick);

        CHECK(wait_for_condition([&]{ return calls.load() >= 1; }));
        CHECK(received.load() == 2);
    }

    TEST_CASE("empty registry: count is always 0 and callback fires once on startup") {
        McpServerRegistry reg; // no servers

        std::atomic<int> received{-1};
        std::atomic<int> calls{0};

        McpStatusPoller poller(
            &reg,
            [&](int n) {
                received.store(n, std::memory_order_relaxed);
                calls.fetch_add(1, std::memory_order_relaxed);
            },
            kTick);

        // Initial observation: count=0 differs from sentinel -1 → fires once.
        CHECK(wait_for_condition([&]{ return calls.load() >= 1; }));
        CHECK(received.load() == 0);

        // Should not fire again (count stays 0).
        int snap = calls.load();
        std::this_thread::sleep_for(kTick * 3 + 20ms);
        CHECK(calls.load() == snap);
    }
}

// ============================================================================
// TEST SUITE 2 — Shutdown / destructor behaviour
// ============================================================================

TEST_SUITE("McpStatusPoller — shutdown") {

    TEST_CASE("destructor returns within 200ms (no hang)") {
        McpServerRegistry reg;

        auto start = std::chrono::steady_clock::now();
        {
            McpStatusPoller poller(
                &reg,
                [](int) {},
                kTick);
            // Let it run for one tick.
            std::this_thread::sleep_for(kTick + 10ms);
        } // destructor here — must join promptly
        auto elapsed = std::chrono::steady_clock::now() - start;

        // The destructor should return well within 200ms after the scope exit.
        // Total wall time includes the tick sleep above, so we check the duration
        // from scope entry to scope exit does not exceed tick + 200ms.
        CHECK(elapsed < kTick + 200ms);
    }

    TEST_CASE("poller can be destroyed immediately without hanging") {
        McpServerRegistry reg;

        auto start = std::chrono::steady_clock::now();
        {
            McpStatusPoller poller(&reg, [](int) {}, kTick);
            // Destruct immediately — don't even wait one tick.
        }
        auto elapsed = std::chrono::steady_clock::now() - start;

        // Even with no tick elapsed, destruction must complete in < 200ms.
        CHECK(elapsed < 200ms);
    }

    TEST_CASE("callback is not called after destructor returns") {
        McpServerRegistry reg;

        auto flag = std::make_shared<std::atomic<bool>>(true);
        reg.add_transport("srv", std::make_unique<MockTransport>(flag));

        std::atomic<int> call_count{0};

        {
            McpStatusPoller poller(
                &reg,
                [&](int) { call_count.fetch_add(1, std::memory_order_relaxed); },
                kTick);
            // Wait for one poll to confirm the poller is running.
            CHECK(wait_for_condition([&]{ return call_count.load() >= 1; }));
        } // poller destroyed here — thread joined

        // Snapshot the count AFTER destructor returns.
        int count_after_destroy = call_count.load();

        // Sleep for two more tick intervals — no additional callbacks expected.
        std::this_thread::sleep_for(kTick * 2 + 20ms);
        CHECK(call_count.load() == count_after_destroy);
    }
}

// ============================================================================
// TEST SUITE 3 — count_failed_servers() on McpServerRegistry
// ============================================================================

TEST_SUITE("McpServerRegistry — count_failed_servers") {

    TEST_CASE("empty registry returns 0") {
        McpServerRegistry reg;
        CHECK(reg.count_failed_servers() == 0);
    }

    TEST_CASE("all healthy transports: count is 0") {
        auto f1 = std::make_shared<std::atomic<bool>>(true);
        auto f2 = std::make_shared<std::atomic<bool>>(true);

        McpServerRegistry reg;
        reg.add_transport("a", std::make_unique<MockTransport>(f1));
        reg.add_transport("b", std::make_unique<MockTransport>(f2));

        CHECK(reg.count_failed_servers() == 0);
    }

    TEST_CASE("one unhealthy transport: count is 1") {
        auto f1 = std::make_shared<std::atomic<bool>>(false);
        auto f2 = std::make_shared<std::atomic<bool>>(true);

        McpServerRegistry reg;
        reg.add_transport("a", std::make_unique<MockTransport>(f1));
        reg.add_transport("b", std::make_unique<MockTransport>(f2));

        CHECK(reg.count_failed_servers() == 1);
    }

    TEST_CASE("all unhealthy: count equals server count") {
        auto f1 = std::make_shared<std::atomic<bool>>(false);
        auto f2 = std::make_shared<std::atomic<bool>>(false);
        auto f3 = std::make_shared<std::atomic<bool>>(false);

        McpServerRegistry reg;
        reg.add_transport("a", std::make_unique<MockTransport>(f1));
        reg.add_transport("b", std::make_unique<MockTransport>(f2));
        reg.add_transport("c", std::make_unique<MockTransport>(f3));

        CHECK(reg.count_failed_servers() == 3);
    }

    TEST_CASE("count reflects live health state changes") {
        auto flag = std::make_shared<std::atomic<bool>>(true);

        McpServerRegistry reg;
        reg.add_transport("srv", std::make_unique<MockTransport>(flag));

        CHECK(reg.count_failed_servers() == 0);

        flag->store(false, std::memory_order_relaxed);
        CHECK(reg.count_failed_servers() == 1);

        flag->store(true, std::memory_order_relaxed);
        CHECK(reg.count_failed_servers() == 0);
    }
}
