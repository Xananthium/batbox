// tests/integration/test_mcp_resources_tools.cpp
// =============================================================================
// Integration tests for batbox::tools::ListMcpResourcesTool and
// batbox::tools::ReadMcpResourceTool.
//
// Uses an in-process MockTransport (same design as test_mcp_mock_transport.cpp)
// injected into McpServerRegistry via add_transport().  No real processes or
// network required.
//
// Acceptance criteria tested:
//   AC1: ListMcpResources without args returns resources from ALL servers
//   AC2: ListMcpResources with args.server filters to that server only
//   AC3: ReadMcpResource returns body of the resource
//   AC4: Unknown server name returns error
//   AC5: Integration test against fixture (MockTransport used as fixture)
//
// Build standalone (from repo root):
//   c++ -std=c++20 -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/integration/test_mcp_resources_tools.cpp \
//       src/tools/ListMcpResourcesTool.cpp \
//       src/tools/ReadMcpResourceTool.cpp \
//       src/mcp/McpServerRegistry.cpp \
//       src/mcp/JsonRpc.cpp \
//       src/config/McpConfig.cpp \
//       src/core/CancelToken.cpp src/core/Json.cpp \
//       src/core/Logging.cpp src/core/Paths.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       build/vcpkg_installed/arm64-osx/lib/libspdlog.a \
//       build/vcpkg_installed/arm64-osx/lib/libfmt.a \
//       -o /tmp/test_mcp_resources_tools && /tmp/test_mcp_resources_tools
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/mcp/IMcpTransport.hpp>
#include <batbox/mcp/McpServerRegistry.hpp>
#include <batbox/tools/ListMcpResourcesTool.hpp>
#include <batbox/tools/ReadMcpResourceTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using namespace batbox;
using namespace batbox::mcp;
using namespace batbox::tools;

// ============================================================================
// MockTransport — in-process double
// ============================================================================

class MockTransport final : public IMcpTransport {
public:
    struct Call {
        std::string method;
        Json        params;
    };

    struct ResponseRule {
        std::string                method;
        std::optional<Json>        result;
        std::optional<std::string> error;
    };

    MockTransport() = default;
    MockTransport(const MockTransport&)            = delete;
    MockTransport& operator=(const MockTransport&) = delete;
    MockTransport(MockTransport&&)                 = delete;
    MockTransport& operator=(MockTransport&&)      = delete;

    Result<void> start(CancelToken ct) override {
        if (ct.is_cancelled()) return Err(std::string("cancelled"));
        bool expected = false;
        if (!started_.compare_exchange_strong(expected, true)) return {};
        stopped_.store(false);
        return {};
    }

    void stop() override {
        bool expected = true;
        if (!started_.compare_exchange_strong(expected, false)) return;
        stopped_.store(true);
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
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
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

    void on_notification(std::function<void(std::string, Json)>) override {}

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

private:
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::atomic<bool> started_{false};
    std::atomic<bool> stopped_{false};
    bool              stop_notified_{false};
    std::deque<Call>         calls_;
    std::deque<ResponseRule> response_rules_;
};

// ============================================================================
// Helpers
// ============================================================================

static ToolContext make_ctx() {
    ToolContext ctx;
    ctx.session_id = "test-session";
    return ctx;
}

static CancelToken never_cancel() { return CancelToken{}; }

// Build resources/list response JSON for a given list of resource dicts.
static Json make_resources_response(
    std::vector<std::tuple<std::string,std::string,std::string>> items)
{
    Json arr = Json::array();
    for (auto& [name, uri, desc] : items) {
        arr.push_back(Json{{"name", name}, {"uri", uri}, {"description", desc}});
    }
    return Json{{"resources", arr}};
}

// Build resources/read response JSON with text content.
static Json make_read_response_text(const std::string& uri,
                                     const std::string& mime,
                                     const std::string& text) {
    return Json{{"contents", Json::array({
        Json{{"uri", uri}, {"mimeType", mime}, {"text", text}}
    })}};
}

// Build resources/read response JSON with blob content.
static Json make_read_response_blob(const std::string& uri,
                                     const std::string& mime) {
    return Json{{"contents", Json::array({
        Json{{"uri", uri}, {"mimeType", mime}, {"blob", "base64encodeddata=="}}
    })}};
}

// ============================================================================
// TEST SUITE 1 — ListMcpResources: no server filter (all servers)
// ============================================================================

TEST_SUITE("ListMcpResources — all servers") {

    TEST_CASE("AC1: no args — returns resources from all servers") {
        McpServerRegistry reg;

        auto* mock_a = new MockTransport();
        auto* mock_b = new MockTransport();

        reg.add_transport("serverA", std::unique_ptr<MockTransport>(mock_a));
        reg.add_transport("serverB", std::unique_ptr<MockTransport>(mock_b));

        // Start transports so they accept requests.
        REQUIRE(mock_a->start(never_cancel()).has_value());
        REQUIRE(mock_b->start(never_cancel()).has_value());

        mock_a->queue_response("resources/list",
            make_resources_response({{"file1", "file:///tmp/a.txt", "File A"}}));
        mock_b->queue_response("resources/list",
            make_resources_response({{"db1", "db://main", "Main database"}}));

        ListMcpResourcesTool tool{reg};
        ToolContext ctx = make_ctx();

        auto result = tool.run(Json::object(), ctx);

        CHECK_FALSE(result.is_error);
        CHECK(result.body.find("serverA:file1") != std::string::npos);
        CHECK(result.body.find("serverB:db1")   != std::string::npos);

        // Payload should contain both servers.
        REQUIRE(result.structured_payload.has_value());
        const Json& payload = *result.structured_payload;
        CHECK(payload["servers"].size() == 2);
    }

    TEST_CASE("AC1: no servers connected — informative message") {
        McpServerRegistry reg;
        ListMcpResourcesTool tool{reg};
        ToolContext ctx = make_ctx();

        auto result = tool.run(Json::object(), ctx);

        CHECK_FALSE(result.is_error);
        CHECK(result.body == "No MCP servers are connected.");
    }

    TEST_CASE("AC1: server with empty resource list") {
        McpServerRegistry reg;
        auto* mock = new MockTransport();
        reg.add_transport("empty_server", std::unique_ptr<MockTransport>(mock));
        REQUIRE(mock->start(never_cancel()).has_value());

        mock->queue_response("resources/list", Json{{"resources", Json::array()}});

        ListMcpResourcesTool tool{reg};
        auto ctx_ = make_ctx();

        auto result = tool.run(Json::object(), ctx_);

        CHECK_FALSE(result.is_error);
        CHECK(result.body == "No resources found.");
    }

    TEST_CASE("AC1: transport error for one server — still returns others") {
        McpServerRegistry reg;
        auto* mock_ok  = new MockTransport();
        auto* mock_err = new MockTransport();

        reg.add_transport("ok_server",  std::unique_ptr<MockTransport>(mock_ok));
        reg.add_transport("err_server", std::unique_ptr<MockTransport>(mock_err));

        REQUIRE(mock_ok->start(never_cancel()).has_value());
        REQUIRE(mock_err->start(never_cancel()).has_value());

        mock_ok->queue_response("resources/list",
            make_resources_response({{"res1", "file:///good.txt", ""}}));
        mock_err->queue_error("resources/list", "RPC error: not implemented");

        ListMcpResourcesTool tool{reg};
        auto ctx_ = make_ctx();

        auto result = tool.run(Json::object(), ctx_);

        CHECK_FALSE(result.is_error);
        CHECK(result.body.find("ok_server:res1")  != std::string::npos);
        CHECK(result.body.find("err_server error") != std::string::npos);
    }
}

// ============================================================================
// TEST SUITE 2 — ListMcpResources: with server filter
// ============================================================================

TEST_SUITE("ListMcpResources — single server filter") {

    TEST_CASE("AC2: with args.server — only queries that server") {
        McpServerRegistry reg;
        auto* mock_a = new MockTransport();
        auto* mock_b = new MockTransport();

        reg.add_transport("serverA", std::unique_ptr<MockTransport>(mock_a));
        reg.add_transport("serverB", std::unique_ptr<MockTransport>(mock_b));

        REQUIRE(mock_a->start(never_cancel()).has_value());
        REQUIRE(mock_b->start(never_cancel()).has_value());

        mock_a->queue_response("resources/list",
            make_resources_response({{"file1", "file:///a.txt", "From A"}}));
        // mock_b deliberately has no queued response — should not be called.

        ListMcpResourcesTool tool{reg};
        Json args = Json::object();
        args["server"] = "serverA";

        auto ctx_ = make_ctx();


        auto result = tool.run(args, ctx_);

        CHECK_FALSE(result.is_error);
        CHECK(result.body.find("serverA:file1") != std::string::npos);
        // serverB should not appear in the result.
        CHECK(result.body.find("serverB") == std::string::npos);

        // Verify only serverA was queried.
        auto calls_b = mock_b->recorded_calls();
        CHECK(calls_b.empty());
    }

    TEST_CASE("AC4: unknown server name — returns error") {
        McpServerRegistry reg;
        ListMcpResourcesTool tool{reg};

        Json args = Json::object();
        args["server"] = "nonexistent";

        auto ctx_ = make_ctx();


        auto result = tool.run(args, ctx_);

        CHECK(result.is_error);
        CHECK(result.body.find("unknown server: nonexistent") != std::string::npos);
    }

    TEST_CASE("args.server empty string — error") {
        McpServerRegistry reg;
        ListMcpResourcesTool tool{reg};

        Json args = Json::object();
        args["server"] = "";

        auto ctx_ = make_ctx();


        auto result = tool.run(args, ctx_);
        CHECK(result.is_error);
    }

    TEST_CASE("args.server wrong type — error") {
        McpServerRegistry reg;
        ListMcpResourcesTool tool{reg};

        Json args = Json::object();
        args["server"] = 42;

        auto ctx_ = make_ctx();


        auto result = tool.run(args, ctx_);
        CHECK(result.is_error);
        CHECK(result.body.find("must be a string") != std::string::npos);
    }
}

// ============================================================================
// TEST SUITE 3 — ListMcpResources: identity and schema
// ============================================================================

TEST_SUITE("ListMcpResources — identity and schema") {

    TEST_CASE("name() returns ListMcpResources") {
        McpServerRegistry reg;
        ListMcpResourcesTool tool{reg};
        CHECK(tool.name() == "ListMcpResources");
    }

    TEST_CASE("is_read_only() returns true") {
        McpServerRegistry reg;
        ListMcpResourcesTool tool{reg};
        CHECK(tool.is_read_only());
    }

    TEST_CASE("requires_confirmation() returns false") {
        McpServerRegistry reg;
        ListMcpResourcesTool tool{reg};
        CHECK_FALSE(tool.requires_confirmation());
    }

    TEST_CASE("schema_json() has correct structure") {
        McpServerRegistry reg;
        ListMcpResourcesTool tool{reg};
        Json schema = tool.schema_json();
        CHECK(schema["name"] == "ListMcpResources");
        CHECK(schema.contains("description"));
        CHECK(schema.contains("parameters"));
        CHECK(schema["parameters"]["type"] == "object");
        CHECK(schema["parameters"]["properties"].contains("server"));
    }

    TEST_CASE("pre-cancelled token — returns error immediately") {
        McpServerRegistry reg;
        auto [src, ct] = CancelToken::make_root();
        src.request_stop();

        ListMcpResourcesTool tool{reg};
        ToolContext ctx = make_ctx();
        ctx.cancel_token = std::move(ct);

        auto result = tool.run(Json::object(), ctx);
        CHECK(result.is_error);
        CHECK(result.body == "cancelled");
    }
}

// ============================================================================
// TEST SUITE 4 — ReadMcpResource: success cases
// ============================================================================

TEST_SUITE("ReadMcpResource — success") {

    TEST_CASE("AC3: returns text body of resource") {
        McpServerRegistry reg;
        auto* mock = new MockTransport();
        reg.add_transport("myserver", std::unique_ptr<MockTransport>(mock));
        REQUIRE(mock->start(never_cancel()).has_value());

        mock->queue_response("resources/read",
            make_read_response_text("file:///tmp/hello.txt", "text/plain",
                                    "Hello, world!"));

        ReadMcpResourceTool tool{reg};
        Json args = Json::object();
        args["server"] = "myserver";
        args["uri"]    = "file:///tmp/hello.txt";

        auto ctx_ = make_ctx();


        auto result = tool.run(args, ctx_);

        CHECK_FALSE(result.is_error);
        CHECK(result.body == "Hello, world!");

        // Payload check.
        REQUIRE(result.structured_payload.has_value());
        const Json& payload = *result.structured_payload;
        CHECK(payload["server"] == "myserver");
        CHECK(payload["uri"]    == "file:///tmp/hello.txt");
        CHECK(payload["contents"].size() == 1);
        CHECK(payload["contents"][0]["text"] == "Hello, world!");
    }

    TEST_CASE("binary blob resource — placeholder text returned") {
        McpServerRegistry reg;
        auto* mock = new MockTransport();
        reg.add_transport("blobserver", std::unique_ptr<MockTransport>(mock));
        REQUIRE(mock->start(never_cancel()).has_value());

        mock->queue_response("resources/read",
            make_read_response_blob("file:///image.png", "image/png"));

        ReadMcpResourceTool tool{reg};
        Json args = Json::object();
        args["server"] = "blobserver";
        args["uri"]    = "file:///image.png";

        auto ctx_ = make_ctx();


        auto result = tool.run(args, ctx_);

        CHECK_FALSE(result.is_error);
        CHECK(result.body.find("<binary blob>") != std::string::npos);

        REQUIRE(result.structured_payload.has_value());
        CHECK((*result.structured_payload)["contents"][0]["blob"] == true);
    }

    TEST_CASE("resources/read sends correct URI in params") {
        McpServerRegistry reg;
        auto* mock = new MockTransport();
        reg.add_transport("checkserver", std::unique_ptr<MockTransport>(mock));
        REQUIRE(mock->start(never_cancel()).has_value());

        mock->queue_response("resources/read",
            make_read_response_text("db://records/1", "application/json", "{}"));

        ReadMcpResourceTool tool{reg};
        Json args = Json::object();
        args["server"] = "checkserver";
        args["uri"]    = "db://records/1";

        auto ctx_ = make_ctx();


        (void)tool.run(args, ctx_);

        auto calls = mock->recorded_calls();
        REQUIRE(calls.size() == 1);
        CHECK(calls[0].method == "resources/read");
        CHECK(calls[0].params["uri"] == "db://records/1");
    }

    TEST_CASE("empty contents array — informative body") {
        McpServerRegistry reg;
        auto* mock = new MockTransport();
        reg.add_transport("emptyserver", std::unique_ptr<MockTransport>(mock));
        REQUIRE(mock->start(never_cancel()).has_value());

        mock->queue_response("resources/read", Json{{"contents", Json::array()}});

        ReadMcpResourceTool tool{reg};
        Json args = Json::object();
        args["server"] = "emptyserver";
        args["uri"]    = "file:///empty";

        auto ctx_ = make_ctx();


        auto result = tool.run(args, ctx_);
        CHECK_FALSE(result.is_error);
        CHECK(result.body.find("empty") != std::string::npos);
    }
}

// ============================================================================
// TEST SUITE 5 — ReadMcpResource: error cases
// ============================================================================

TEST_SUITE("ReadMcpResource — errors") {

    TEST_CASE("AC4: unknown server — error") {
        McpServerRegistry reg;
        ReadMcpResourceTool tool{reg};

        Json args = Json::object();
        args["server"] = "ghost_server";
        args["uri"]    = "file:///test.txt";

        auto ctx_ = make_ctx();


        auto result = tool.run(args, ctx_);

        CHECK(result.is_error);
        CHECK(result.body.find("unknown server: ghost_server") != std::string::npos);
    }

    TEST_CASE("transport RPC error — surfaced as is_error") {
        McpServerRegistry reg;
        auto* mock = new MockTransport();
        reg.add_transport("errserver", std::unique_ptr<MockTransport>(mock));
        REQUIRE(mock->start(never_cancel()).has_value());

        mock->queue_error("resources/read", "-32601: Method not found");

        ReadMcpResourceTool tool{reg};
        Json args = Json::object();
        args["server"] = "errserver";
        args["uri"]    = "file:///missing.txt";

        auto ctx_ = make_ctx();


        auto result = tool.run(args, ctx_);

        CHECK(result.is_error);
        CHECK(result.body.find("ReadMcpResource:") != std::string::npos);
    }

    TEST_CASE("missing 'server' arg — error") {
        McpServerRegistry reg;
        ReadMcpResourceTool tool{reg};

        Json args = Json::object();
        args["uri"] = "file:///test.txt";

        auto ctx_ = make_ctx();


        auto result = tool.run(args, ctx_);
        CHECK(result.is_error);
        CHECK(result.body.find("'server'") != std::string::npos);
    }

    TEST_CASE("missing 'uri' arg — error") {
        McpServerRegistry reg;
        auto* mock = new MockTransport();
        reg.add_transport("myserver", std::unique_ptr<MockTransport>(mock));
        REQUIRE(mock->start(never_cancel()).has_value());

        ReadMcpResourceTool tool{reg};
        Json args = Json::object();
        args["server"] = "myserver";

        auto ctx_ = make_ctx();


        auto result = tool.run(args, ctx_);
        CHECK(result.is_error);
        CHECK(result.body.find("'uri'") != std::string::npos);
    }

    TEST_CASE("'server' not a string — error") {
        McpServerRegistry reg;
        ReadMcpResourceTool tool{reg};

        Json args = Json::object();
        args["server"] = 99;
        args["uri"]    = "file:///test.txt";

        auto ctx_ = make_ctx();


        auto result = tool.run(args, ctx_);
        CHECK(result.is_error);
        CHECK(result.body.find("must be a string") != std::string::npos);
    }

    TEST_CASE("'uri' empty string — error") {
        McpServerRegistry reg;
        auto* mock = new MockTransport();
        reg.add_transport("myserver", std::unique_ptr<MockTransport>(mock));
        REQUIRE(mock->start(never_cancel()).has_value());

        ReadMcpResourceTool tool{reg};
        Json args = Json::object();
        args["server"] = "myserver";
        args["uri"]    = "";

        auto ctx_ = make_ctx();


        auto result = tool.run(args, ctx_);
        CHECK(result.is_error);
        CHECK(result.body.find("'uri' must be a non-empty") != std::string::npos);
    }
}

// ============================================================================
// TEST SUITE 6 — ReadMcpResource: identity and schema
// ============================================================================

TEST_SUITE("ReadMcpResource — identity and schema") {

    TEST_CASE("name() returns ReadMcpResource") {
        McpServerRegistry reg;
        ReadMcpResourceTool tool{reg};
        CHECK(tool.name() == "ReadMcpResource");
    }

    TEST_CASE("is_read_only() returns true") {
        McpServerRegistry reg;
        ReadMcpResourceTool tool{reg};
        CHECK(tool.is_read_only());
    }

    TEST_CASE("requires_confirmation() returns false") {
        McpServerRegistry reg;
        ReadMcpResourceTool tool{reg};
        CHECK_FALSE(tool.requires_confirmation());
    }

    TEST_CASE("schema_json() has correct structure") {
        McpServerRegistry reg;
        ReadMcpResourceTool tool{reg};
        Json schema = tool.schema_json();
        CHECK(schema["name"] == "ReadMcpResource");
        CHECK(schema.contains("description"));
        CHECK(schema.contains("parameters"));
        CHECK(schema["parameters"]["properties"].contains("server"));
        CHECK(schema["parameters"]["properties"].contains("uri"));

        // Both server and uri are required.
        const Json& required = schema["parameters"]["required"];
        bool has_server = false, has_uri = false;
        for (const auto& r : required) {
            if (r == "server") has_server = true;
            if (r == "uri")    has_uri    = true;
        }
        CHECK(has_server);
        CHECK(has_uri);
    }

    TEST_CASE("pre-cancelled token — returns error immediately") {
        McpServerRegistry reg;
        auto [src, ct] = CancelToken::make_root();
        src.request_stop();

        ReadMcpResourceTool tool{reg};
        ToolContext ctx = make_ctx();
        ctx.cancel_token = std::move(ct);

        Json args = Json::object();
        args["server"] = "any";
        args["uri"]    = "file:///test";

        auto result = tool.run(args, ctx);
        CHECK(result.is_error);
        CHECK(result.body == "cancelled");
    }
}
