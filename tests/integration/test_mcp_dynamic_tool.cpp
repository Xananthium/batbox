// tests/integration/test_mcp_dynamic_tool.cpp
// ---------------------------------------------------------------------------
// Integration tests for batbox::tools::McpTool.
//
// Exercises the full run() lifecycle using an in-process MockTransport injected
// into McpServerRegistry.  No network or real MCP server is required.
//
// Acceptance criteria tested:
//   AC1: Routes to correct transport via McpServerRegistry::get
//   AC2: Returns raw JSON-RPC result in body (compact JSON dump)
//   AC3: Error response surfaced as is_error=true
//   AC4: Unknown server → is_error=true with descriptive message
//   AC5: Unhealthy transport → is_error=true
//   AC6: Missing required args → is_error=true
//   AC7: Optional params defaults to null
//   AC8: Cancellation → is_error=true with "cancelled"
//
// Build standalone (from repo root):
//   c++ -std=c++20 -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/integration/test_mcp_dynamic_tool.cpp \
//       src/tools/McpTool.cpp \
//       src/mcp/McpServerRegistry.cpp \
//       src/mcp/JsonRpc.cpp \
//       src/config/McpConfig.cpp \
//       src/core/CancelToken.cpp src/core/Json.cpp \
//       src/core/Logging.cpp src/core/Paths.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       build/vcpkg_installed/arm64-osx/lib/libspdlog.a \
//       build/vcpkg_installed/arm64-osx/lib/libfmt.a \
//       -o /tmp/test_mcp_dynamic_tool && /tmp/test_mcp_dynamic_tool
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/mcp/IMcpTransport.hpp>
#include <batbox/mcp/McpServerRegistry.hpp>
#include <batbox/permissions/PermissionMode.hpp>
#include <batbox/tools/McpTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
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
using namespace batbox::tools;

// ============================================================================
// MockTransport — in-process double (same design as test_mcp_registry.cpp)
// ============================================================================

class MockTransport final : public IMcpTransport {
public:
    struct Call { std::string method; Json params; };

    struct ResponseRule {
        std::string                method;
        std::optional<Json>        result;
        std::optional<std::string> error;
    };

    MockTransport() = default;
    MockTransport(const MockTransport&) = delete;
    MockTransport& operator=(const MockTransport&) = delete;
    MockTransport(MockTransport&&) noexcept = delete;
    MockTransport& operator=(MockTransport&&) noexcept = delete;

    Result<void> start(CancelToken ct) override {
        if (ct.is_cancelled()) return Err(std::string("cancelled"));
        bool expected = false;
        if (!started_.compare_exchange_strong(expected, true)) return {};
        stopped_.store(false);
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
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        while (true) {
            if (ct.is_cancelled()) return Err(std::string("cancelled"));
            if (stop_notified_) return Err(std::string("transport stopped"));
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
    [[nodiscard]] std::vector<Call> recorded_calls() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return std::vector<Call>(calls_.begin(), calls_.end());
    }
    [[nodiscard]] int start_count() const { return start_count_.load(); }
    [[nodiscard]] int stop_count()  const { return stop_count_.load();  }

private:
    mutable std::mutex            mtx_;
    std::condition_variable       cv_;
    std::atomic<bool>             started_{false};
    std::atomic<bool>             stopped_{false};
    bool                          stop_notified_{false};
    std::atomic<int>              start_count_{0};
    std::atomic<int>              stop_count_{0};
    std::deque<Call>              calls_;
    std::deque<ResponseRule>      response_rules_;
    std::function<void(std::string, Json)> notification_handler_;
};

// ============================================================================
// Helpers
// ============================================================================

static ToolContext make_ctx() {
    ToolContext ctx;
    ctx.cwd        = std::filesystem::temp_directory_path();
    ctx.mode       = batbox::permissions::PermissionMode::Default;
    ctx.session_id = "test-session";
    ctx.agent_id   = "";
    return ctx;
}

// Build a registry with a named, started MockTransport.
// Returns (registry, raw pointer to mock — valid for lifetime of registry).
static std::pair<std::unique_ptr<McpServerRegistry>, MockTransport*>
make_registry_with_mock(std::string server_name)
{
    auto reg  = std::make_unique<McpServerRegistry>();
    auto mock = std::make_unique<MockTransport>();
    MockTransport* raw = mock.get();

    // Start the mock so healthy() == true.
    // CancelToken default-constructs as a never-cancelled token.
    (void)mock->start(CancelToken{});

    reg->add_transport(std::move(server_name), std::move(mock));
    return {std::move(reg), raw};
}

// ============================================================================
// Test suite
// ============================================================================

TEST_CASE("McpTool: name and description")
{
    auto [reg, _mock] = make_registry_with_mock("test");
    McpTool tool(*reg);

    CHECK(tool.name()        == "MCP");
    CHECK(!std::string_view(tool.description()).empty());
}

TEST_CASE("McpTool: schema_json has required shape")
{
    auto [reg, _mock] = make_registry_with_mock("test");
    McpTool tool(*reg);

    const Json schema = tool.schema_json();
    REQUIRE(schema.is_object());
    CHECK(schema.at("name").get<std::string>() == "MCP");
    CHECK(schema.contains("description"));
    CHECK(schema.contains("parameters"));

    const Json& params = schema.at("parameters");
    CHECK(params.at("type").get<std::string>() == "object");
    CHECK(params.at("properties").contains("server"));
    CHECK(params.at("properties").contains("method"));
    CHECK(params.at("properties").contains("params"));

    // "server" and "method" are required.
    const Json& required = params.at("required");
    REQUIRE(required.is_array());
    bool has_server = false;
    bool has_method = false;
    for (const auto& r : required) {
        if (r.get<std::string>() == "server") has_server = true;
        if (r.get<std::string>() == "method") has_method = true;
    }
    CHECK(has_server);
    CHECK(has_method);
}

TEST_CASE("McpTool: is_read_only and requires_confirmation")
{
    auto [reg, _mock] = make_registry_with_mock("test");
    McpTool tool(*reg);

    CHECK_FALSE(tool.is_read_only());
    CHECK(tool.requires_confirmation());
}

// AC1 + AC2: Routes to correct transport; returns raw JSON-RPC result in body.
TEST_CASE("McpTool: successful call returns compact JSON body")
{
    auto [reg, mock] = make_registry_with_mock("filesystem");
    McpTool tool(*reg);

    const Json expected_result = Json{
        {"tools", Json::array({
            Json{{"name", "read_file"}, {"description", "Read a file"}}
        })}
    };
    mock->queue_response("tools/list", expected_result);

    Json args = Json{{"server", "filesystem"}, {"method", "tools/list"}};
    ToolContext ctx = make_ctx();

    const ToolResult result = tool.run(args, ctx);

    REQUIRE_FALSE(result.is_error);
    // Body should be compact JSON dump of the result.
    const Json parsed_body = Json::parse(result.body);
    CHECK(parsed_body == expected_result);

    // Verify structured payload contains server + method + result.
    REQUIRE(result.structured_payload.has_value());
    const Json& payload = *result.structured_payload;
    CHECK(payload.at("server").get<std::string>() == "filesystem");
    CHECK(payload.at("method").get<std::string>() == "tools/list");
    CHECK(payload.at("result") == expected_result);

    // Verify the transport actually received the call.
    const auto calls = mock->recorded_calls();
    REQUIRE(calls.size() == 1u);
    CHECK(calls[0].method == "tools/list");
}

// AC2: params forwarded verbatim.
TEST_CASE("McpTool: params are forwarded to transport")
{
    auto [reg, mock] = make_registry_with_mock("github");
    McpTool tool(*reg);

    const Json tool_call_params = Json{
        {"name", "create_issue"},
        {"arguments", Json{{"title", "Bug report"}, {"body", "Details here"}}}
    };
    mock->queue_response("tools/call", Json{{"content", "issue created"}});

    Json args = Json{
        {"server", "github"},
        {"method", "tools/call"},
        {"params", tool_call_params}
    };
    ToolContext ctx = make_ctx();

    const ToolResult result = tool.run(args, ctx);

    REQUIRE_FALSE(result.is_error);

    const auto calls = mock->recorded_calls();
    REQUIRE(calls.size() == 1u);
    CHECK(calls[0].method == "tools/call");
    CHECK(calls[0].params == tool_call_params);
}

// AC7: params optional → defaults to null.
TEST_CASE("McpTool: absent params defaults to null")
{
    auto [reg, mock] = make_registry_with_mock("myserver");
    McpTool tool(*reg);

    mock->queue_response("ping", Json{{"pong", true}});

    Json args = Json{{"server", "myserver"}, {"method", "ping"}};
    ToolContext ctx = make_ctx();

    const ToolResult result = tool.run(args, ctx);

    REQUIRE_FALSE(result.is_error);

    const auto calls = mock->recorded_calls();
    REQUIRE(calls.size() == 1u);
    CHECK(calls[0].method == "ping");
    // params should be null Json
    CHECK(calls[0].params.is_null());
}

// AC3: transport error surfaced as is_error=true.
TEST_CASE("McpTool: transport error returns is_error=true")
{
    auto [reg, mock] = make_registry_with_mock("filesystem");
    McpTool tool(*reg);

    mock->queue_error("tools/call", "-32601: Method not found");

    Json args = Json{
        {"server",  "filesystem"},
        {"method",  "tools/call"},
        {"params",  Json{{"name", "nonexistent_tool"}}}
    };
    ToolContext ctx = make_ctx();

    const ToolResult result = tool.run(args, ctx);

    CHECK(result.is_error);
    CHECK(result.body.find("MCP error:") != std::string::npos);
    CHECK(result.body.find("-32601") != std::string::npos);
}

// AC4: unknown server → is_error=true.
TEST_CASE("McpTool: unknown server returns is_error=true")
{
    auto [reg, _mock] = make_registry_with_mock("filesystem");
    McpTool tool(*reg);

    Json args = Json{{"server", "does_not_exist"}, {"method", "tools/list"}};
    ToolContext ctx = make_ctx();

    const ToolResult result = tool.run(args, ctx);

    CHECK(result.is_error);
    CHECK(result.body.find("unknown server") != std::string::npos);
    CHECK(result.body.find("does_not_exist") != std::string::npos);
}

// AC5: unhealthy transport → is_error=true.
TEST_CASE("McpTool: unhealthy transport returns is_error=true")
{
    auto reg  = std::make_unique<McpServerRegistry>();
    auto mock = std::make_unique<MockTransport>();
    // Do NOT start the mock; healthy() == false.
    reg->add_transport("stopped_server", std::move(mock));

    McpTool tool(*reg);

    Json args = Json{{"server", "stopped_server"}, {"method", "tools/list"}};
    ToolContext ctx = make_ctx();

    const ToolResult result = tool.run(args, ctx);

    CHECK(result.is_error);
    CHECK(result.body.find("not healthy") != std::string::npos);
    CHECK(result.body.find("stopped_server") != std::string::npos);
}

// AC6: missing 'server' argument → is_error=true.
TEST_CASE("McpTool: missing server argument returns is_error=true")
{
    auto [reg, _mock] = make_registry_with_mock("test");
    McpTool tool(*reg);

    Json args = Json{{"method", "tools/list"}};
    ToolContext ctx = make_ctx();

    const ToolResult result = tool.run(args, ctx);

    CHECK(result.is_error);
    CHECK(result.body.find("server") != std::string::npos);
}

// AC6: missing 'method' argument → is_error=true.
TEST_CASE("McpTool: missing method argument returns is_error=true")
{
    auto [reg, _mock] = make_registry_with_mock("test");
    McpTool tool(*reg);

    Json args = Json{{"server", "test"}};
    ToolContext ctx = make_ctx();

    const ToolResult result = tool.run(args, ctx);

    CHECK(result.is_error);
    CHECK(result.body.find("method") != std::string::npos);
}

// AC6: empty server string → is_error=true.
TEST_CASE("McpTool: empty server string returns is_error=true")
{
    auto [reg, _mock] = make_registry_with_mock("test");
    McpTool tool(*reg);

    Json args = Json{{"server", ""}, {"method", "tools/list"}};
    ToolContext ctx = make_ctx();

    const ToolResult result = tool.run(args, ctx);

    CHECK(result.is_error);
}

// AC8: Cancellation before dispatch → is_error=true with "cancelled".
TEST_CASE("McpTool: pre-cancelled context returns is_error=true")
{
    auto [reg, _mock] = make_registry_with_mock("test");
    McpTool tool(*reg);

    Json args = Json{{"server", "test"}, {"method", "tools/list"}};
    ToolContext ctx = make_ctx();

    // Cancel the token before running.
    auto [src, ct] = CancelToken::make_root();
    src.request_stop();
    ctx.cancel_token = std::move(ct);

    const ToolResult result = tool.run(args, ctx);

    CHECK(result.is_error);
    CHECK(result.body == "cancelled");
}

// Multiple servers in the registry — routes only to the correct one.
TEST_CASE("McpTool: routes to correct transport among multiple servers")
{
    auto reg = std::make_unique<McpServerRegistry>();

    auto mock_a = std::make_unique<MockTransport>();
    auto mock_b = std::make_unique<MockTransport>();
    MockTransport* raw_a = mock_a.get();
    MockTransport* raw_b = mock_b.get();

    // Start both mocks.
    (void)mock_a->start(CancelToken{});
    (void)mock_b->start(CancelToken{});

    reg->add_transport("server_a", std::move(mock_a));
    reg->add_transport("server_b", std::move(mock_b));

    McpTool tool(*reg);

    // Queue responses on each server.
    raw_a->queue_response("ping", Json{{"from", "a"}});
    raw_b->queue_response("ping", Json{{"from", "b"}});

    // Call server_b first.
    Json args_b = Json{{"server", "server_b"}, {"method", "ping"}};
    ToolContext ctx = make_ctx();
    const ToolResult result_b = tool.run(args_b, ctx);

    REQUIRE_FALSE(result_b.is_error);
    const Json parsed_b = Json::parse(result_b.body);
    CHECK(parsed_b.at("from").get<std::string>() == "b");

    // server_a received no calls.
    CHECK(raw_a->recorded_calls().empty());
    // server_b received one call.
    REQUIRE(raw_b->recorded_calls().size() == 1u);
    CHECK(raw_b->recorded_calls()[0].method == "ping");

    // Now call server_a — its queued response still pending.
    Json args_a = Json{{"server", "server_a"}, {"method", "ping"}};
    const ToolResult result_a = tool.run(args_a, ctx);

    REQUIRE_FALSE(result_a.is_error);
    const Json parsed_a = Json::parse(result_a.body);
    CHECK(parsed_a.at("from").get<std::string>() == "a");
}

// Null JSON result from server is handled gracefully.
TEST_CASE("McpTool: null JSON result from transport succeeds")
{
    auto [reg, mock] = make_registry_with_mock("minimal");
    McpTool tool(*reg);

    mock->queue_response("notify/ack", Json(nullptr));

    Json args = Json{{"server", "minimal"}, {"method", "notify/ack"}};
    ToolContext ctx = make_ctx();

    const ToolResult result = tool.run(args, ctx);

    CHECK_FALSE(result.is_error);
    CHECK(result.body == "null");
}
