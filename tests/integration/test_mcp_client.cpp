// tests/unit/test_mcp_client.cpp
// ---------------------------------------------------------------------------
// Unit tests for batbox::mcp::McpClient.
//
// Acceptance criteria:
//   AC1: Initialize handshake parses capabilities correctly
//   AC2: tools/list returns unified Json annotated with server name
//   AC3: tools/call routes to correct transport
//   AC4: resources/* work end-to-end
//   AC5: prompts/* work end-to-end
//   AC6: Tool-list cache is populated and invalidated on list_changed
//   AC7: Reconnect on transport failure (dispatch retries once)
//   AC8: Unknown server returns Err
//   AC9: Pre-cancelled token returns Err("cancelled")
//   AC10: initialize_all with empty registry returns Ok
//
// Build (from repo root):
//   c++ -std=c++20 -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       /tmp/test_mcp_client.cpp \
//       src/mcp/McpClient.cpp \
//       src/mcp/McpServerRegistry.cpp \
//       src/mcp/JsonRpc.cpp \
//       src/config/McpConfig.cpp \
//       src/core/CancelToken.cpp src/core/Json.cpp \
//       src/core/Logging.cpp src/core/Paths.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       build/vcpkg_installed/arm64-osx/lib/libspdlog.a \
//       build/vcpkg_installed/arm64-osx/lib/libfmt.a \
//       -o /tmp/test_mcp_client && /tmp/test_mcp_client
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/mcp/IMcpTransport.hpp>
#include <batbox/mcp/McpClient.hpp>
#include <batbox/mcp/McpServerRegistry.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace batbox;
using namespace batbox::mcp;

// ============================================================================
// MockTransport — same design as test_mcp_registry.cpp
// ============================================================================

class MockTransport final : public IMcpTransport {
public:
    struct Call { std::string method; Json params; };

    struct ResponseRule {
        std::string               method;
        std::optional<Json>       result;
        std::optional<std::string> error;
    };

    MockTransport() = default;
    MockTransport(const MockTransport&) = delete;
    MockTransport& operator=(const MockTransport&) = delete;
    MockTransport(MockTransport&&) = delete;
    MockTransport& operator=(MockTransport&&) = delete;

    Result<void> start(CancelToken ct) override {
        if (ct.is_cancelled()) return Err(std::string("cancelled"));
        bool expected = false;
        if (!started_.compare_exchange_strong(expected, true)) return {};
        stopped_.store(false);
        stop_notified_ = false;
        start_count_.fetch_add(1);
        return {};
    }

    void stop() override {
        bool expected = true;
        if (!started_.compare_exchange_strong(expected, false)) return;
        stopped_.store(true);
        stop_count_.fetch_add(1);
        std::lock_guard<std::mutex> lk(mtx_);
        stop_notified_ = true;
        cv_.notify_all();
    }

    [[nodiscard]] bool healthy() const override { return started_.load(); }

    [[nodiscard]] Result<Json> request(std::string method, Json params,
                                        CancelToken ct) override {
        { std::lock_guard<std::mutex> lk(mtx_); calls_.push_back({method, params}); }
        if (!started_.load()) return Err(std::string("transport not started"));
        if (ct.is_cancelled()) return Err(std::string("cancelled"));

        std::unique_lock<std::mutex> lk(mtx_);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (true) {
            if (ct.is_cancelled()) return Err(std::string("cancelled"));
            if (stop_notified_)    return Err(std::string("transport stopped"));
            for (auto it = response_rules_.begin(); it != response_rules_.end(); ++it) {
                if (it->method == method) {
                    ResponseRule rule = std::move(*it);
                    response_rules_.erase(it);
                    lk.unlock();
                    if (rule.error.has_value()) return Err(*rule.error);
                    return rule.result.value_or(Json(nullptr));
                }
            }
            if (cv_.wait_until(lk, deadline) == std::cv_status::timeout)
                return Err(std::string("no response queued for: ") + method);
        }
    }

    [[nodiscard]] Result<void> notify(std::string method, Json params) override {
        if (!started_.load()) return Err(std::string("transport not started"));
        std::lock_guard<std::mutex> lk(mtx_);
        calls_.push_back({std::move(method), std::move(params)});
        return {};
    }

    void on_notification(std::function<void(std::string, Json)> handler) override {
        std::lock_guard<std::mutex> lk(mtx_);
        notification_handler_ = std::move(handler);
    }

    // Test helpers.
    void queue_response(std::string method, Json result) {
        std::lock_guard<std::mutex> lk(mtx_);
        response_rules_.push_back({std::move(method), std::move(result), std::nullopt});
        cv_.notify_all();
    }
    void queue_error(std::string method, std::string error_msg) {
        std::lock_guard<std::mutex> lk(mtx_);
        response_rules_.push_back({std::move(method), std::nullopt, std::move(error_msg)});
        cv_.notify_all();
    }
    void push_notification(std::string method, Json params) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (notification_handler_) {
            notification_handler_(std::move(method), std::move(params));
        }
    }
    [[nodiscard]] std::vector<Call> recorded_calls() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return {calls_.begin(), calls_.end()};
    }
    [[nodiscard]] int start_count() const { return start_count_.load(); }
    [[nodiscard]] int stop_count()  const { return stop_count_.load(); }

    // Make transport report unhealthy without stopping the recorder.
    void set_healthy(bool v) { started_.store(v); }

private:
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::atomic<bool> started_{false};
    std::atomic<bool> stopped_{false};
    bool              stop_notified_{false};
    std::atomic<int>  start_count_{0};
    std::atomic<int>  stop_count_{0};
    std::deque<Call>          calls_;
    std::deque<ResponseRule>  response_rules_;
    std::function<void(std::string, Json)> notification_handler_;
};

// ============================================================================
// Helpers
// ============================================================================

static Json make_capabilities(bool tools = true, bool resources = true, bool prompts = true) {
    Json caps;
    if (tools)     caps["tools"]     = Json::object();
    if (resources) caps["resources"] = Json::object();
    if (prompts)   caps["prompts"]   = Json::object();
    return {
        {"protocolVersion", "2024-11-05"},
        {"capabilities",    caps},
        {"serverInfo",      {{"name","test-server"},{"version","0.1"}}}
    };
}

static CancelToken never_cancel() { return CancelToken{}; }

// ============================================================================
// Test fixtures
// ============================================================================

struct Fixture {
    McpServerRegistry reg;
    MockTransport*    mock = nullptr;  // non-owning

    explicit Fixture(const std::string& server_name = "test") {
        auto t = std::make_unique<MockTransport>();
        mock = t.get();
        reg.add_transport(server_name, std::move(t));
        // Start the transport so it's healthy.
        auto [src, tok] = CancelToken::make_root();
        reg.start_all(src.token());
    }
};

// ============================================================================
// AC10: initialize_all with empty registry
// ============================================================================

TEST_CASE("McpClient: initialize_all on empty registry returns Ok") {
    McpServerRegistry empty_reg;
    McpClient client(empty_reg);
    auto [src, tok] = CancelToken::make_root();
    auto r = client.initialize_all(src.token());
    REQUIRE(r.has_value());
}

// ============================================================================
// AC1: initialize handshake parses capabilities correctly
// ============================================================================

TEST_CASE("McpClient: initialize handshake parses capabilities") {
    Fixture f;
    f.mock->queue_response("initialize", make_capabilities(true, true, true));

    McpClient client(f.reg);
    auto r = client.initialize_all(never_cancel());
    REQUIRE(r.has_value());

    auto caps = client.capabilities("test");
    REQUIRE(caps.has_value());
    CHECK(caps->tools);
    CHECK(caps->resources);
    CHECK(caps->prompts);
    CHECK(caps->raw.contains("protocolVersion"));

    // initialized_servers() should include "test".
    auto servers = client.initialized_servers();
    REQUIRE(servers.size() == 1);
    CHECK(servers[0] == "test");

    // Transport should have received initialize + notifications/initialized.
    auto calls = f.mock->recorded_calls();
    bool saw_init = false, saw_notif = false;
    for (auto& c : calls) {
        if (c.method == "initialize")              saw_init = true;
        if (c.method == "notifications/initialized") saw_notif = true;
    }
    CHECK(saw_init);
    CHECK(saw_notif);
}

TEST_CASE("McpClient: initialize parses partial capabilities (tools only)") {
    Fixture f;
    f.mock->queue_response("initialize", make_capabilities(true, false, false));

    McpClient client(f.reg);
    REQUIRE(client.initialize_all(never_cancel()).has_value());

    auto caps = client.capabilities("test");
    REQUIRE(caps.has_value());
    CHECK(caps->tools);
    CHECK(!caps->resources);
    CHECK(!caps->prompts);
}

TEST_CASE("McpClient: capabilities returns nullopt before initialize") {
    Fixture f;
    McpClient client(f.reg);
    auto caps = client.capabilities("test");
    CHECK(!caps.has_value());
}

// ============================================================================
// AC2: tools/list returns Json from server
// ============================================================================

TEST_CASE("McpClient: tools_list returns server response") {
    Fixture f;
    f.mock->queue_response("initialize", make_capabilities());
    f.mock->queue_response("tools/list", Json::parse(
        R"({"tools":[{"name":"echo","description":"echo tool","inputSchema":{"type":"object"}}]})"
    ));

    McpClient client(f.reg);
    REQUIRE(client.initialize_all(never_cancel()).has_value());

    auto r = client.tools_list("test", never_cancel());
    REQUIRE(r.has_value());
    REQUIRE(r->contains("tools"));
    CHECK((*r)["tools"].size() == 1);
    CHECK((*r)["tools"][0]["name"] == "echo");
}

TEST_CASE("McpClient: tools_list caches result on second call") {
    Fixture f;
    f.mock->queue_response("initialize", make_capabilities());
    f.mock->queue_response("tools/list", Json::parse(R"({"tools":[]})"));

    McpClient client(f.reg);
    REQUIRE(client.initialize_all(never_cancel()).has_value());

    // First call — fetches from transport.
    auto r1 = client.tools_list("test", never_cancel());
    REQUIRE(r1.has_value());

    // Second call — uses cache; no additional queued response needed.
    auto r2 = client.tools_list("test", never_cancel());
    REQUIRE(r2.has_value());

    // Only one tools/list request should have been sent.
    int count = 0;
    for (auto& c : f.mock->recorded_calls())
        if (c.method == "tools/list") ++count;
    CHECK(count == 1);
}

TEST_CASE("McpClient: tools_list force_refresh bypasses cache") {
    Fixture f;
    f.mock->queue_response("initialize", make_capabilities());
    f.mock->queue_response("tools/list", Json::parse(R"({"tools":[]})"));
    f.mock->queue_response("tools/list", Json::parse(R"({"tools":[]})"));

    McpClient client(f.reg);
    REQUIRE(client.initialize_all(never_cancel()).has_value());

    REQUIRE(client.tools_list("test", never_cancel()).has_value());
    REQUIRE(client.tools_list("test", never_cancel(), /*force_refresh=*/true).has_value());

    int count = 0;
    for (auto& c : f.mock->recorded_calls())
        if (c.method == "tools/list") ++count;
    CHECK(count == 2);
}

// ============================================================================
// AC6: tools/list cache invalidated on list_changed notification
// ============================================================================

TEST_CASE("McpClient: tools/list cache invalidated by list_changed notification") {
    Fixture f;
    f.mock->queue_response("initialize", make_capabilities());
    f.mock->queue_response("tools/list", Json::parse(R"({"tools":[]})"));    // first fetch
    f.mock->queue_response("tools/list", Json::parse(R"({"tools":[{"name":"new"}]})")); // second

    McpClient client(f.reg);
    REQUIRE(client.initialize_all(never_cancel()).has_value());

    // First fetch — populates cache.
    auto r1 = client.tools_list("test", never_cancel());
    REQUIRE(r1.has_value());
    CHECK((*r1)["tools"].empty());

    // Simulate server sending list_changed notification.
    f.mock->push_notification("notifications/tools/list_changed", Json::object());

    // Second fetch — cache invalidated, fetches from transport again.
    auto r2 = client.tools_list("test", never_cancel());
    REQUIRE(r2.has_value());
    CHECK((*r2)["tools"].size() == 1);

    int count = 0;
    for (auto& c : f.mock->recorded_calls())
        if (c.method == "tools/list") ++count;
    CHECK(count == 2);
}

// ============================================================================
// AC3: tools/call routes to correct transport
// ============================================================================

TEST_CASE("McpClient: tools_call routes to correct transport") {
    Fixture f;
    f.mock->queue_response("initialize", make_capabilities());
    f.mock->queue_response("tools/call", Json::parse(
        R"({"content":[{"type":"text","text":"hello"}]})"
    ));

    McpClient client(f.reg);
    REQUIRE(client.initialize_all(never_cancel()).has_value());

    Json args = {{"message", "hello"}};
    auto r = client.tools_call("test", "echo", args, never_cancel());
    REQUIRE(r.has_value());
    CHECK((*r)["content"][0]["text"] == "hello");

    // Verify the request params sent to the transport.
    auto calls = f.mock->recorded_calls();
    bool found = false;
    for (auto& c : calls) {
        if (c.method == "tools/call") {
            CHECK(c.params["name"] == "echo");
            CHECK(c.params["arguments"]["message"] == "hello");
            found = true;
        }
    }
    CHECK(found);
}

// ============================================================================
// AC4: resources/* work end-to-end
// ============================================================================

TEST_CASE("McpClient: resources_list routes correctly") {
    Fixture f;
    f.mock->queue_response("initialize", make_capabilities());
    f.mock->queue_response("resources/list", Json::parse(
        R"({"resources":[{"name":"file1","uri":"file:///tmp/a.txt"}]})"
    ));

    McpClient client(f.reg);
    REQUIRE(client.initialize_all(never_cancel()).has_value());

    auto r = client.resources_list("test", never_cancel());
    REQUIRE(r.has_value());
    CHECK((*r)["resources"][0]["uri"] == "file:///tmp/a.txt");
}

TEST_CASE("McpClient: resources_read routes correctly with uri") {
    Fixture f;
    f.mock->queue_response("initialize", make_capabilities());
    f.mock->queue_response("resources/read", Json::parse(
        R"({"contents":[{"uri":"file:///tmp/a.txt","mimeType":"text/plain","text":"hello"}]})"
    ));

    McpClient client(f.reg);
    REQUIRE(client.initialize_all(never_cancel()).has_value());

    auto r = client.resources_read("test", "file:///tmp/a.txt", never_cancel());
    REQUIRE(r.has_value());
    CHECK((*r)["contents"][0]["text"] == "hello");

    // Verify uri was sent correctly.
    for (auto& c : f.mock->recorded_calls()) {
        if (c.method == "resources/read") {
            CHECK(c.params["uri"] == "file:///tmp/a.txt");
        }
    }
}

// ============================================================================
// AC5: prompts/* work end-to-end
// ============================================================================

TEST_CASE("McpClient: prompts_list routes correctly") {
    Fixture f;
    f.mock->queue_response("initialize", make_capabilities());
    f.mock->queue_response("prompts/list", Json::parse(
        R"({"prompts":[{"name":"greet"}]})"
    ));

    McpClient client(f.reg);
    REQUIRE(client.initialize_all(never_cancel()).has_value());

    auto r = client.prompts_list("test", never_cancel());
    REQUIRE(r.has_value());
    CHECK((*r)["prompts"][0]["name"] == "greet");
}

TEST_CASE("McpClient: prompts_get routes with name and args") {
    Fixture f;
    f.mock->queue_response("initialize", make_capabilities());
    f.mock->queue_response("prompts/get", Json::parse(
        R"({"messages":[{"role":"user","content":"hello world"}]})"
    ));

    McpClient client(f.reg);
    REQUIRE(client.initialize_all(never_cancel()).has_value());

    Json prompt_args = {{"name", "world"}};
    auto r = client.prompts_get("test", "greet", prompt_args, never_cancel());
    REQUIRE(r.has_value());
    CHECK((*r)["messages"][0]["role"] == "user");

    for (auto& c : f.mock->recorded_calls()) {
        if (c.method == "prompts/get") {
            CHECK(c.params["name"] == "greet");
            CHECK(c.params["arguments"]["name"] == "world");
        }
    }
}

// ============================================================================
// AC8: Unknown server returns Err
// ============================================================================

TEST_CASE("McpClient: tools_list on unknown server returns Err") {
    Fixture f;
    f.mock->queue_response("initialize", make_capabilities());

    McpClient client(f.reg);
    REQUIRE(client.initialize_all(never_cancel()).has_value());

    auto r = client.tools_list("nonexistent", never_cancel());
    REQUIRE(!r.has_value());
    CHECK(r.error().find("unknown server") != std::string::npos);
}

TEST_CASE("McpClient: tools_call on unknown server returns Err") {
    Fixture f;
    f.mock->queue_response("initialize", make_capabilities());

    McpClient client(f.reg);
    REQUIRE(client.initialize_all(never_cancel()).has_value());

    auto r = client.tools_call("no-such-server", "echo", Json::object(), never_cancel());
    REQUIRE(!r.has_value());
    CHECK(r.error().find("unknown server") != std::string::npos);
}

// ============================================================================
// AC7: Reconnect on transport failure
// ============================================================================

TEST_CASE("McpClient: dispatch retries once on transport error") {
    // We inject a transport that fails once, then succeeds.
    McpServerRegistry reg;
    auto t = std::make_unique<MockTransport>();
    MockTransport* mock = t.get();
    reg.add_transport("flaky", std::move(t));

    auto [src, tok] = CancelToken::make_root();
    reg.start_all(src.token());

    McpClient client(reg);

    // Queue initialize response and tools/list.
    mock->queue_response("initialize", make_capabilities());
    REQUIRE(client.initialize_all(never_cancel()).has_value());

    // First tools/list fails with transport error; after reconnect,
    // the registry will re-start the transport and McpClient will retry.
    // Queue the error, then queue the successful response for the retry.
    mock->queue_error("tools/list", "connection reset");
    // After reconnect, McpClient calls do_initialize again, then retries:
    mock->queue_response("initialize", make_capabilities()); // re-init after reconnect
    mock->queue_response("tools/list", Json::parse(R"({"tools":[]})"));

    auto r = client.tools_list("flaky", never_cancel());
    // The retry should succeed.
    REQUIRE(r.has_value());
}

// ============================================================================
// AC9: Pre-cancelled token
// ============================================================================

TEST_CASE("McpClient: pre-cancelled token returns cancelled error") {
    Fixture f;
    f.mock->queue_response("initialize", make_capabilities());

    McpClient client(f.reg);
    REQUIRE(client.initialize_all(never_cancel()).has_value());

    // Pre-cancel a token.
    auto [src, tok] = CancelToken::make_root();
    src.request_stop();

    auto r = client.tools_list("test", std::move(tok));
    // The mock transport returns Err("cancelled") for cancelled tokens.
    REQUIRE(!r.has_value());
    CHECK(r.error() == "cancelled");
}

// ============================================================================
// Multiple servers — parallel initialize
// ============================================================================

TEST_CASE("McpClient: initialize_all initializes multiple servers in parallel") {
    McpServerRegistry reg;

    auto t1 = std::make_unique<MockTransport>();
    auto t2 = std::make_unique<MockTransport>();
    MockTransport* mock1 = t1.get();
    MockTransport* mock2 = t2.get();
    reg.add_transport("server1", std::move(t1));
    reg.add_transport("server2", std::move(t2));

    auto [src, tok] = CancelToken::make_root();
    reg.start_all(src.token());

    mock1->queue_response("initialize", make_capabilities(true, false, false));
    mock2->queue_response("initialize", make_capabilities(false, true, false));

    McpClient client(reg);
    auto r = client.initialize_all(never_cancel());
    REQUIRE(r.has_value());

    auto s1 = client.capabilities("server1");
    auto s2 = client.capabilities("server2");
    REQUIRE(s1.has_value());
    REQUIRE(s2.has_value());
    CHECK(s1->tools);
    CHECK(!s1->resources);
    CHECK(!s2->tools);
    CHECK(s2->resources);
}

TEST_CASE("McpClient: initialize_all reports partial failures") {
    McpServerRegistry reg;
    auto t1 = std::make_unique<MockTransport>();
    auto t2 = std::make_unique<MockTransport>();
    MockTransport* mock1 = t1.get();
    MockTransport* mock2 = t2.get();
    reg.add_transport("ok_server", std::move(t1));
    reg.add_transport("bad_server", std::move(t2));

    auto [src, tok] = CancelToken::make_root();
    reg.start_all(src.token());

    mock1->queue_response("initialize", make_capabilities());
    mock2->queue_error("initialize", "server refuses handshake");

    McpClient client(reg);
    auto r = client.initialize_all(never_cancel());
    // Should fail (one server failed).
    REQUIRE(!r.has_value());
    CHECK(r.error().find("bad_server") != std::string::npos);
    CHECK(r.error().find("server refuses handshake") != std::string::npos);

    // ok_server should still be initialized.
    auto caps = client.capabilities("ok_server");
    REQUIRE(caps.has_value());
}

// ============================================================================
// initialize_one
// ============================================================================

TEST_CASE("McpClient: initialize_one initializes a single server") {
    Fixture f;
    f.mock->queue_response("initialize", make_capabilities(true, false, false));

    McpClient client(f.reg);
    auto r = client.initialize_one("test", never_cancel());
    REQUIRE(r.has_value());

    auto caps = client.capabilities("test");
    REQUIRE(caps.has_value());
    CHECK(caps->tools);
}

TEST_CASE("McpClient: initialize_one on unknown server returns Err") {
    Fixture f;
    McpClient client(f.reg);
    auto r = client.initialize_one("no-such-server", never_cancel());
    REQUIRE(!r.has_value());
    CHECK(r.error().find("unknown server") != std::string::npos);
}

// ============================================================================
// tools/list RPC error propagated
// ============================================================================

TEST_CASE("McpClient: tools_list RPC error from server is propagated") {
    Fixture f;
    f.mock->queue_response("initialize", make_capabilities());
    f.mock->queue_error("tools/list", "method not found");

    McpClient client(f.reg);
    REQUIRE(client.initialize_all(never_cancel()).has_value());

    // First call fails (and reconnect fails too since mock is still running),
    // and after reconnect/re-init, second attempt also will get "method not found".
    // Queue enough responses to cover the reconnect path:
    f.mock->queue_response("initialize", make_capabilities()); // re-init after restart
    f.mock->queue_error("tools/list", "method not found");     // retry also fails

    auto r = client.tools_list("test", never_cancel());
    REQUIRE(!r.has_value());
}
