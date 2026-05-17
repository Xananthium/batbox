// tests/unit/test_mcp_mock_transport.cpp
// ---------------------------------------------------------------------------
// doctest suite for batbox::mcp::IMcpTransport interface and MockTransport.
//
// This file serves two purposes:
//   1. Validates that IMcpTransport compiles as an ABI-stable virtual base.
//   2. Exercises MockTransport — the in-process test double used by all
//      higher-level MCP tests (McpClient, tool dispatch, etc.).
//
// Build standalone (from repo root):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_mcp_mock_transport.cpp \
//       src/mcp/JsonRpc.cpp \
//       -o /tmp/test_mcp_mock_transport && /tmp/test_mcp_mock_transport
//
// With vcpkg doctest:
//   c++ -std=c++20 -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_mcp_mock_transport.cpp \
//       src/mcp/JsonRpc.cpp \
//       -o /tmp/test_mcp_mock_transport && /tmp/test_mcp_mock_transport
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/mcp/IMcpTransport.hpp>
#include <batbox/mcp/JsonRpc.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Result.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace batbox;
using namespace batbox::mcp;

// ============================================================================
// MockTransport — in-process transport double
// ============================================================================
//
// Design:
//   - Maintains a queue of pre-programmed (method → result) response rules.
//     When request() is called, MockTransport looks up the first matching rule
//     by method name and returns its canned result (or Err if none found).
//   - Records every (method, params) pair sent via request() and notify() for
//     post-test assertion.
//   - Fires the on_notification() callback when push_notification() is called
//     from test code, simulating server-initiated notifications.
//   - healthy() reflects the started/stopped lifecycle state.
//   - Thread-safe: a mutex guards the response queue and recorded calls.
//
// Usage pattern in tests:
//   MockTransport t;
//   t.queue_response("tools/list",
//       Json::parse(R"({"tools":[]})"));
//   auto [src, ct] = CancelToken::make_root();
//   REQUIRE(t.start(std::move(ct)));
//   auto res = t.request("tools/list", Json(nullptr),
//                        CancelToken{});   // never-cancelled token
//   REQUIRE(res.has_value());
//   CHECK(res->contains("tools"));
//   t.stop();

class MockTransport final : public IMcpTransport {
public:
    // -------------------------------------------------------------------------
    // Recorded call — one entry per request() or notify() invocation.
    // -------------------------------------------------------------------------
    struct Call {
        std::string method;
        Json        params;
    };

    // -------------------------------------------------------------------------
    // Response rule — queued by test code via queue_response() / queue_error().
    // -------------------------------------------------------------------------
    struct ResponseRule {
        std::string          method;      // match criterion
        std::optional<Json>  result;      // present → return Ok(result)
        std::optional<std::string> error; // present → return Err(error)
    };

    MockTransport() = default;

    // Non-copyable, non-movable (mutex member prevents move).
    MockTransport(const MockTransport&)            = delete;
    MockTransport& operator=(const MockTransport&) = delete;
    MockTransport(MockTransport&&) noexcept        = delete;
    MockTransport& operator=(MockTransport&&) noexcept = delete;

    // -------------------------------------------------------------------------
    // IMcpTransport interface
    // -------------------------------------------------------------------------

    Result<void> start(CancelToken ct) override {
        if (ct.is_cancelled()) {
            return Err(std::string("cancelled"));
        }
        bool expected = false;
        if (!started_.compare_exchange_strong(expected, true)) {
            // Already started — idempotent, return ok.
            return {};
        }
        stopped_.store(false);
        return {};
    }

    void stop() override {
        bool expected = true;
        if (!started_.compare_exchange_strong(expected, false)) {
            return; // Already stopped — no-op.
        }
        stopped_.store(true);

        // Wake any threads blocked in request() waiting for a response.
        {
            std::lock_guard<std::mutex> lk(mtx_);
            stop_notified_ = true;
        }
        cv_.notify_all();
    }

    [[nodiscard]] bool healthy() const override {
        return started_.load();
    }

    [[nodiscard]] Result<Json> request(std::string method,
                                       Json        params,
                                       CancelToken ct) override {
        // Record the call before checking state so tests can inspect even
        // calls made to a stopped transport.
        {
            std::lock_guard<std::mutex> lk(mtx_);
            calls_.push_back(Call{method, params});
        }

        if (!started_.load()) {
            return Err(std::string("transport not started"));
        }

        if (ct.is_cancelled()) {
            return Err(std::string("cancelled"));
        }

        // Find and consume the first matching response rule.
        std::unique_lock<std::mutex> lk(mtx_);

        // Wait until either a matching rule appears, the transport is stopped,
        // or ct fires. We check with a short spin to honour cancellation
        // without requiring a full threading integration.
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(100);

        while (true) {
            if (ct.is_cancelled()) {
                return Err(std::string("cancelled"));
            }
            if (stop_notified_) {
                return Err(std::string("transport stopped"));
            }

            // Search for a matching rule.
            for (auto it = response_rules_.begin();
                 it != response_rules_.end(); ++it) {
                if (it->method == method) {
                    ResponseRule rule = std::move(*it);
                    response_rules_.erase(it);
                    lk.unlock();

                    if (rule.error.has_value()) {
                        return Err(*rule.error);
                    }
                    return rule.result.value_or(Json(nullptr));
                }
            }

            // No rule found yet — wait briefly for one to be queued or for
            // stop/cancel to fire.
            if (cv_.wait_until(lk, deadline) == std::cv_status::timeout) {
                // Timeout: return error for unmatched request.
                return Err(std::string("no response queued for method: ") + method);
            }
        }
    }

    [[nodiscard]] Result<void> notify(std::string method,
                                      Json        params) override {
        if (!started_.load()) {
            return Err(std::string("transport not started"));
        }
        std::lock_guard<std::mutex> lk(mtx_);
        calls_.push_back(Call{std::move(method), std::move(params)});
        return {};
    }

    void on_notification(
        std::function<void(std::string, Json)> handler) override {
        std::lock_guard<std::mutex> lk(mtx_);
        notification_handler_ = std::move(handler);
    }

    // -------------------------------------------------------------------------
    // Test-control methods (not part of IMcpTransport)
    // -------------------------------------------------------------------------

    /// Queue a successful response for the next request() matching `method`.
    void queue_response(std::string method, Json result) {
        std::lock_guard<std::mutex> lk(mtx_);
        response_rules_.push_back(
            ResponseRule{std::move(method), std::move(result), std::nullopt});
        cv_.notify_all();
    }

    /// Queue an error response for the next request() matching `method`.
    void queue_error(std::string method, std::string error_msg) {
        std::lock_guard<std::mutex> lk(mtx_);
        response_rules_.push_back(
            ResponseRule{std::move(method), std::nullopt, std::move(error_msg)});
        cv_.notify_all();
    }

    /// Simulate a server-initiated notification arriving over the transport.
    /// Fires the registered on_notification() callback synchronously.
    void push_notification(std::string method, Json params) {
        std::function<void(std::string, Json)> handler;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            handler = notification_handler_;
        }
        if (handler) {
            handler(std::move(method), std::move(params));
        }
    }

    /// All recorded request() and notify() calls in order of invocation.
    [[nodiscard]] std::vector<Call> recorded_calls() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return std::vector<Call>(calls_.begin(), calls_.end());
    }

    /// Number of pending (unconsumed) response rules.
    [[nodiscard]] std::size_t pending_responses() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return response_rules_.size();
    }

    /// Reset all recorded state (calls + rules) without changing started state.
    void reset_recorded() {
        std::lock_guard<std::mutex> lk(mtx_);
        calls_.clear();
        response_rules_.clear();
        stop_notified_ = false;
    }

private:
    mutable std::mutex mtx_;
    std::condition_variable cv_;

    std::atomic<bool> started_{false};
    std::atomic<bool> stopped_{false};
    bool              stop_notified_{false};

    std::deque<Call>          calls_;
    std::deque<ResponseRule>  response_rules_;

    std::function<void(std::string, Json)> notification_handler_;
};

// ============================================================================
// Helpers
// ============================================================================

/// Returns a CancelToken that is never cancelled (useful for tests that don't
/// need cancellation).
static CancelToken never_cancel() {
    // Default-constructed CancelToken has no stop_source → never fires.
    return CancelToken{};
}

// ============================================================================
// TEST SUITE 1 — Interface ABI / compile-time checks
// ============================================================================

TEST_SUITE("IMcpTransport — interface compile checks") {

    TEST_CASE("IMcpTransport is abstract (cannot be instantiated directly)") {
        // This test verifies the structural / ABI contract at compile time.
        // The static_assert below would fail to compile if IMcpTransport were
        // not abstract (i.e. if the pure virtual methods were given bodies).
        static_assert(std::is_abstract_v<IMcpTransport>,
            "IMcpTransport must be abstract (pure virtual methods)");
        CHECK(std::is_abstract_v<IMcpTransport>);
    }

    TEST_CASE("IMcpTransport has virtual destructor") {
        static_assert(std::has_virtual_destructor_v<IMcpTransport>,
            "IMcpTransport must have a virtual destructor for safe polymorphic delete");
        CHECK(std::has_virtual_destructor_v<IMcpTransport>);
    }

    TEST_CASE("MockTransport is a concrete IMcpTransport") {
        static_assert(!std::is_abstract_v<MockTransport>,
            "MockTransport must not be abstract");
        static_assert(std::is_base_of_v<IMcpTransport, MockTransport>,
            "MockTransport must derive from IMcpTransport");
        CHECK(std::is_base_of_v<IMcpTransport, MockTransport>);
    }

    TEST_CASE("MockTransport is usable through IMcpTransport pointer") {
        std::unique_ptr<IMcpTransport> t = std::make_unique<MockTransport>();
        // If this compiles and does not crash, the virtual dispatch is correct.
        CHECK_FALSE(t->healthy());
    }
}

// ============================================================================
// TEST SUITE 2 — Lifecycle: start / healthy / stop
// ============================================================================

TEST_SUITE("MockTransport — lifecycle") {

    TEST_CASE("initial state: not healthy, not started") {
        MockTransport t;
        CHECK_FALSE(t.healthy());
    }

    TEST_CASE("start() makes transport healthy") {
        MockTransport t;
        auto [src, ct] = CancelToken::make_root();
        auto res = t.start(std::move(ct));
        REQUIRE(res.has_value());
        CHECK(t.healthy());
    }

    TEST_CASE("start() is idempotent when already healthy") {
        MockTransport t;
        auto [src1, ct1] = CancelToken::make_root();
        REQUIRE(t.start(std::move(ct1)).has_value());
        CHECK(t.healthy());

        auto [src2, ct2] = CancelToken::make_root();
        auto res2 = t.start(std::move(ct2));
        REQUIRE(res2.has_value()); // second start → still ok
        CHECK(t.healthy());        // still healthy
    }

    TEST_CASE("stop() makes transport not healthy") {
        MockTransport t;
        auto [src, ct] = CancelToken::make_root();
        REQUIRE(t.start(std::move(ct)).has_value());
        t.stop();
        CHECK_FALSE(t.healthy());
    }

    TEST_CASE("stop() on an already-stopped transport is a no-op") {
        MockTransport t;
        t.stop(); // never started
        CHECK_FALSE(t.healthy());
        t.stop(); // second stop — must not crash
        CHECK_FALSE(t.healthy());
    }

    TEST_CASE("start() with pre-cancelled token returns error") {
        MockTransport t;
        auto [src, ct] = CancelToken::make_root();
        src.request_stop(); // cancel before start
        auto res = t.start(std::move(ct));
        CHECK_FALSE(res.has_value());
        CHECK(res.error().find("cancelled") != std::string::npos);
        CHECK_FALSE(t.healthy());
    }

    TEST_CASE("polymorphic delete via IMcpTransport pointer is safe") {
        // Validates that the virtual destructor is correctly wired.
        IMcpTransport* raw = new MockTransport();
        auto [src, ct] = CancelToken::make_root();
        REQUIRE(raw->start(std::move(ct)).has_value());
        raw->stop();
        delete raw; // must not leak / crash
        CHECK(true); // reached here without crash
    }
}

// ============================================================================
// TEST SUITE 3 — request() happy path
// ============================================================================

TEST_SUITE("MockTransport — request happy path") {

    TEST_CASE("queued response is returned for matching method") {
        MockTransport t;
        auto [src, ct] = CancelToken::make_root();
        REQUIRE(t.start(std::move(ct)).has_value());

        Json expected = Json::parse(R"({"tools":[],"nextCursor":null})");
        t.queue_response("tools/list", expected);

        auto res = t.request("tools/list", Json(nullptr), never_cancel());
        REQUIRE(res.has_value());
        CHECK(res.value() == expected);
    }

    TEST_CASE("multiple queued responses returned in FIFO order") {
        MockTransport t;
        auto [src, ct] = CancelToken::make_root();
        REQUIRE(t.start(std::move(ct)).has_value());

        Json r1 = Json::parse(R"({"id":1})");
        Json r2 = Json::parse(R"({"id":2})");
        t.queue_response("ping", r1);
        t.queue_response("ping", r2);

        auto res1 = t.request("ping", Json(nullptr), never_cancel());
        auto res2 = t.request("ping", Json(nullptr), never_cancel());

        REQUIRE(res1.has_value());
        REQUIRE(res2.has_value());
        CHECK(res1.value()["id"] == 1);
        CHECK(res2.value()["id"] == 2);
    }

    TEST_CASE("request records call with correct method and params") {
        MockTransport t;
        auto [src, ct] = CancelToken::make_root();
        REQUIRE(t.start(std::move(ct)).has_value());

        Json params = Json::parse(R"({"name":"add"})");
        t.queue_response("tools/call", Json::parse(R"({"content":[]})"));

        auto res = t.request("tools/call", params, never_cancel());
        REQUIRE(res.has_value());

        auto calls = t.recorded_calls();
        REQUIRE(calls.size() == 1);
        CHECK(calls[0].method == "tools/call");
        CHECK(calls[0].params == params);
    }

    TEST_CASE("null params are correctly recorded") {
        MockTransport t;
        auto [src, ct] = CancelToken::make_root();
        REQUIRE(t.start(std::move(ct)).has_value());

        t.queue_response("initialize", Json::parse(R"({"protocolVersion":"2024-11-05"})"));
        auto res = t.request("initialize", Json(nullptr), never_cancel());
        REQUIRE(res.has_value());

        auto calls = t.recorded_calls();
        REQUIRE(calls.size() == 1);
        CHECK(calls[0].params.is_null());
    }

    TEST_CASE("response with null result is returned as null Json") {
        MockTransport t;
        auto [src, ct] = CancelToken::make_root();
        REQUIRE(t.start(std::move(ct)).has_value());

        t.queue_response("ping", Json(nullptr));
        auto res = t.request("ping", Json(nullptr), never_cancel());
        REQUIRE(res.has_value());
        CHECK(res.value().is_null());
    }
}

// ============================================================================
// TEST SUITE 4 — request() error path
// ============================================================================

TEST_SUITE("MockTransport — request error path") {

    TEST_CASE("queued error is returned as Err") {
        MockTransport t;
        auto [src, ct] = CancelToken::make_root();
        REQUIRE(t.start(std::move(ct)).has_value());

        t.queue_error("tools/call", "-32601: Method not found");
        auto res = t.request("tools/call", Json(nullptr), never_cancel());
        CHECK_FALSE(res.has_value());
        CHECK(res.error().find("-32601") != std::string::npos);
    }

    TEST_CASE("request on stopped transport returns error") {
        MockTransport t;
        // Never started.
        auto res = t.request("ping", Json(nullptr), never_cancel());
        CHECK_FALSE(res.has_value());
        CHECK(res.error().find("not started") != std::string::npos);
    }

    TEST_CASE("request with pre-cancelled token returns Err cancelled") {
        MockTransport t;
        auto [src, ct] = CancelToken::make_root();
        REQUIRE(t.start(std::move(ct)).has_value());

        auto [src2, ct2] = CancelToken::make_root();
        src2.request_stop(); // cancel before request

        auto res = t.request("ping", Json(nullptr), std::move(ct2));
        CHECK_FALSE(res.has_value());
        CHECK(res.error().find("cancelled") != std::string::npos);
    }

    TEST_CASE("unmatched request returns descriptive error after timeout") {
        MockTransport t;
        auto [src, ct] = CancelToken::make_root();
        REQUIRE(t.start(std::move(ct)).has_value());

        // No rule queued — should time out and return a descriptive error.
        auto res = t.request("unknown/method", Json(nullptr), never_cancel());
        CHECK_FALSE(res.has_value());
        CHECK(res.error().find("unknown/method") != std::string::npos);
    }
}

// ============================================================================
// TEST SUITE 5 — notify()
// ============================================================================

TEST_SUITE("MockTransport — notify") {

    TEST_CASE("notify returns ok when started") {
        MockTransport t;
        auto [src, ct] = CancelToken::make_root();
        REQUIRE(t.start(std::move(ct)).has_value());

        auto res = t.notify("notifications/progress",
                            Json::parse(R"({"progress":50,"total":100})"));
        CHECK(res.has_value());
    }

    TEST_CASE("notify records method and params") {
        MockTransport t;
        auto [src, ct] = CancelToken::make_root();
        REQUIRE(t.start(std::move(ct)).has_value());

        Json params = Json::parse(R"({"token":"abc","progress":1,"total":10})");
        REQUIRE(t.notify("notifications/progress", params).has_value());

        auto calls = t.recorded_calls();
        REQUIRE(calls.size() == 1);
        CHECK(calls[0].method == "notifications/progress");
        CHECK(calls[0].params == params);
    }

    TEST_CASE("notify returns error on stopped transport") {
        MockTransport t;
        // Not started.
        auto res = t.notify("ping", Json(nullptr));
        CHECK_FALSE(res.has_value());
        CHECK(res.error().find("not started") != std::string::npos);
    }

    TEST_CASE("notify with null params is accepted") {
        MockTransport t;
        auto [src, ct] = CancelToken::make_root();
        REQUIRE(t.start(std::move(ct)).has_value());
        auto res = t.notify("initialized", Json(nullptr));
        CHECK(res.has_value());
    }
}

// ============================================================================
// TEST SUITE 6 — on_notification() inbound dispatch
// ============================================================================

TEST_SUITE("MockTransport — on_notification dispatch") {

    TEST_CASE("registered handler is called when push_notification fires") {
        MockTransport t;
        auto [src, ct] = CancelToken::make_root();

        std::string captured_method;
        Json        captured_params;
        t.on_notification([&](std::string m, Json p) {
            captured_method = std::move(m);
            captured_params = std::move(p);
        });

        REQUIRE(t.start(std::move(ct)).has_value());

        Json notif_params = Json::parse(R"({"level":"info","message":"hello"})");
        t.push_notification("notifications/message", notif_params);

        CHECK(captured_method == "notifications/message");
        CHECK(captured_params == notif_params);
    }

    TEST_CASE("handler receives correct method and null params") {
        MockTransport t;
        auto [src, ct] = CancelToken::make_root();

        bool called = false;
        Json received_params = Json("sentinel");

        t.on_notification([&](std::string m, Json p) {
            CHECK(m == "ping");
            received_params = std::move(p);
            called = true;
        });

        REQUIRE(t.start(std::move(ct)).has_value());
        t.push_notification("ping", Json(nullptr));

        CHECK(called);
        CHECK(received_params.is_null());
    }

    TEST_CASE("replacing handler replaces the old one") {
        MockTransport t;
        auto [src, ct] = CancelToken::make_root();

        int first_count  = 0;
        int second_count = 0;

        t.on_notification([&](std::string, Json) { ++first_count; });
        // Replace with new handler.
        t.on_notification([&](std::string, Json) { ++second_count; });

        REQUIRE(t.start(std::move(ct)).has_value());
        t.push_notification("event", Json(nullptr));

        CHECK(first_count  == 0);  // first handler was replaced
        CHECK(second_count == 1);
    }

    TEST_CASE("no handler set: push_notification is a no-op") {
        MockTransport t;
        auto [src, ct] = CancelToken::make_root();
        REQUIRE(t.start(std::move(ct)).has_value());
        // Must not crash.
        t.push_notification("orphan", Json(nullptr));
        CHECK(true);
    }

    TEST_CASE("multiple notifications dispatched in order") {
        MockTransport t;
        auto [src, ct] = CancelToken::make_root();

        std::vector<std::string> received;
        t.on_notification([&](std::string m, Json) {
            received.push_back(std::move(m));
        });

        REQUIRE(t.start(std::move(ct)).has_value());
        t.push_notification("first",  Json(nullptr));
        t.push_notification("second", Json(nullptr));
        t.push_notification("third",  Json(nullptr));

        REQUIRE(received.size() == 3);
        CHECK(received[0] == "first");
        CHECK(received[1] == "second");
        CHECK(received[2] == "third");
    }
}

// ============================================================================
// TEST SUITE 7 — polymorphic usage via IMcpTransport*
// ============================================================================

TEST_SUITE("IMcpTransport — polymorphic dispatch") {

    TEST_CASE("full request/response cycle through IMcpTransport pointer") {
        std::unique_ptr<IMcpTransport> t = std::make_unique<MockTransport>();

        auto* mock = static_cast<MockTransport*>(t.get());
        mock->queue_response("tools/list",
            Json::parse(R"({"tools":[{"name":"bash"}]})"));

        auto [src, ct] = CancelToken::make_root();
        REQUIRE(t->start(std::move(ct)).has_value());
        CHECK(t->healthy());

        auto res = t->request("tools/list", Json(nullptr), never_cancel());
        REQUIRE(res.has_value());
        CHECK(res.value()["tools"][0]["name"] == "bash");

        t->stop();
        CHECK_FALSE(t->healthy());
    }

    TEST_CASE("notify through IMcpTransport pointer records call") {
        std::unique_ptr<IMcpTransport> t = std::make_unique<MockTransport>();
        auto* mock = static_cast<MockTransport*>(t.get());

        auto [src, ct] = CancelToken::make_root();
        REQUIRE(t->start(std::move(ct)).has_value());

        Json params = Json::parse(R"({"token":"tok1","progress":5,"total":10})");
        REQUIRE(t->notify("notifications/progress", params).has_value());

        auto calls = mock->recorded_calls();
        REQUIRE(calls.size() == 1);
        CHECK(calls[0].method == "notifications/progress");
    }

    TEST_CASE("on_notification registered through interface fires correctly") {
        std::unique_ptr<IMcpTransport> t = std::make_unique<MockTransport>();
        auto* mock = static_cast<MockTransport*>(t.get());

        std::string got_method;
        t->on_notification([&](std::string m, Json) { got_method = std::move(m); });

        auto [src, ct] = CancelToken::make_root();
        REQUIRE(t->start(std::move(ct)).has_value());

        mock->push_notification("tools/list_changed", Json(nullptr));
        CHECK(got_method == "tools/list_changed");
    }
}

// ============================================================================
// TEST SUITE 8 — recorded_calls bookkeeping
// ============================================================================

TEST_SUITE("MockTransport — recorded_calls bookkeeping") {

    TEST_CASE("fresh transport has no recorded calls") {
        MockTransport t;
        CHECK(t.recorded_calls().empty());
    }

    TEST_CASE("calls from both request() and notify() are recorded together") {
        MockTransport t;
        auto [src, ct] = CancelToken::make_root();
        REQUIRE(t.start(std::move(ct)).has_value());

        t.queue_response("ping", Json(nullptr));
        REQUIRE(t.request("ping", Json(nullptr), never_cancel()).has_value());
        REQUIRE(t.notify("initialized", Json(nullptr)).has_value());

        auto calls = t.recorded_calls();
        REQUIRE(calls.size() == 2);
        CHECK(calls[0].method == "ping");
        CHECK(calls[1].method == "initialized");
    }

    TEST_CASE("reset_recorded clears calls and pending rules") {
        MockTransport t;
        auto [src, ct] = CancelToken::make_root();
        REQUIRE(t.start(std::move(ct)).has_value());

        t.queue_response("ping", Json(nullptr));
        REQUIRE(t.request("ping", Json(nullptr), never_cancel()).has_value());

        CHECK(t.recorded_calls().size() == 1);
        t.reset_recorded();
        CHECK(t.recorded_calls().empty());
        CHECK(t.pending_responses() == 0);
    }

    TEST_CASE("pending_responses decrements as responses are consumed") {
        MockTransport t;
        auto [src, ct] = CancelToken::make_root();
        REQUIRE(t.start(std::move(ct)).has_value());

        t.queue_response("a", Json(nullptr));
        t.queue_response("b", Json(nullptr));
        t.queue_response("c", Json(nullptr));
        CHECK(t.pending_responses() == 3);

        REQUIRE(t.request("a", Json(nullptr), never_cancel()).has_value());
        CHECK(t.pending_responses() == 2);

        REQUIRE(t.request("b", Json(nullptr), never_cancel()).has_value());
        CHECK(t.pending_responses() == 1);

        REQUIRE(t.request("c", Json(nullptr), never_cancel()).has_value());
        CHECK(t.pending_responses() == 0);
    }
}
