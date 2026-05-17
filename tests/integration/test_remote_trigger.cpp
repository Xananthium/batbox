// tests/integration/test_remote_trigger.cpp
//
// doctest integration tests for batbox::tools::RemoteTriggerTool.
//
// A fixture ix::HttpServer is started on a free port before the test suite
// runs.  Each test case exercises RemoteTriggerTool against that local server
// to avoid any external network dependency.

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <batbox/tools/RemoteTriggerTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/permissions/PermissionMode.hpp>

#include <ixwebsocket/IXHttpServer.h>
#include <ixwebsocket/IXGetFreePort.h>
#include <ixwebsocket/IXHttp.h>
#include <ixwebsocket/IXWebSocketHttpHeaders.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

using namespace batbox;
using namespace batbox::tools;
using namespace batbox::permissions;

// =============================================================================
// Fixture server — started once and shared across all test cases.
// =============================================================================

struct FixtureServer {
    std::unique_ptr<ix::HttpServer> server;
    int                             port{0};
    std::atomic<int>                request_count{0};
    std::string                     last_received_body;
    std::string                     last_received_content_type;
    std::string                     last_received_auth_header;

    // Configured response the server will return for the next request.
    int         response_status{200};
    std::string response_body{"ok"};

    FixtureServer() {
        port   = ix::getFreePort();
        server = std::make_unique<ix::HttpServer>(port, "127.0.0.1");

        server->setOnConnectionCallback(
            [this](ix::HttpRequestPtr request,
                   std::shared_ptr<ix::ConnectionState> /*state*/) -> ix::HttpResponsePtr
            {
                ++request_count;
                last_received_body = request->body;

                auto it = request->headers.find("Content-Type");
                last_received_content_type = (it != request->headers.end()) ? it->second : "";

                auto ait = request->headers.find("Authorization");
                last_received_auth_header = (ait != request->headers.end()) ? ait->second : "";

                int   status = response_status;
                std::string body_copy = response_body;

                ix::WebSocketHttpHeaders resp_headers;
                resp_headers["Content-Type"] = "application/json";

                return std::make_shared<ix::HttpResponse>(
                    status,
                    status < 300 ? "OK" : "Error",
                    ix::HttpErrorCode::Ok,
                    resp_headers,
                    body_copy);
            });

        auto [ok, err] = server->listen();
        if (!ok) {
            throw std::runtime_error("FixtureServer: listen failed: " + err);
        }
        server->start();

        // Give the server a moment to accept connections.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ~FixtureServer() {
        server->stop();
    }

    /// Build the local URL for a given path.
    [[nodiscard]] std::string url(const std::string& path = "/trigger") const {
        return "http://127.0.0.1:" + std::to_string(port) + path;
    }

    /// Reset per-request state before each test.
    void reset(int status = 200, std::string body = "ok") {
        request_count.store(0);
        response_status = status;
        response_body   = std::move(body);
        last_received_body.clear();
        last_received_content_type.clear();
        last_received_auth_header.clear();
    }
};

// Shared fixture instance — initialised in main() before tests run.
static FixtureServer* g_srv = nullptr;

// =============================================================================
// Helper: build a minimal ToolContext.
// =============================================================================

static ToolContext make_ctx() {
    ToolContext ctx;
    ctx.cwd        = "/tmp";
    ctx.mode       = PermissionMode::Default;
    ctx.session_id = "test-session";
    ctx.agent_id   = "";
    return ctx;
}

// =============================================================================
// TEST SUITE: RemoteTriggerTool — construction and basic contract
// =============================================================================

TEST_SUITE("RemoteTriggerTool — construction") {

    TEST_CASE("name() == 'RemoteTrigger'") {
        RemoteTriggerTool t;
        CHECK(t.name() == std::string_view("RemoteTrigger"));
    }

    TEST_CASE("description() is non-empty") {
        RemoteTriggerTool t;
        CHECK_FALSE(std::string(t.description()).empty());
    }

    TEST_CASE("is_read_only() == false") {
        RemoteTriggerTool t;
        CHECK_FALSE(t.is_read_only());
    }

    TEST_CASE("requires_confirmation() == true") {
        RemoteTriggerTool t;
        CHECK(t.requires_confirmation());
    }

    TEST_CASE("schema_json() has correct shape") {
        RemoteTriggerTool t;
        Json s = t.schema_json();
        REQUIRE(s.is_object());
        CHECK(s["name"].get<std::string>() == "RemoteTrigger");
        REQUIRE(s.contains("parameters"));
        CHECK(s["parameters"]["type"].get<std::string>() == "object");
        REQUIRE(s["parameters"]["properties"].contains("url"));
        REQUIRE(s["parameters"]["properties"].contains("payload"));
    }
}

// =============================================================================
// TEST SUITE: RemoteTriggerTool — argument validation (no network)
// =============================================================================

TEST_SUITE("RemoteTriggerTool — argument validation") {

    TEST_CASE("missing url → error") {
        RemoteTriggerTool t({"http://127.0.0.1:*/**"});
        auto ctx  = make_ctx();
        Json args = {{"payload", Json{{"x", 1}}}};
        ToolResult r = t.run(args, ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("url") != std::string::npos);
    }

    TEST_CASE("non-string url → error") {
        RemoteTriggerTool t({"http://127.0.0.1:*/**"});
        auto ctx  = make_ctx();
        Json args = {{"url", 42}, {"payload", "x"}};
        ToolResult r = t.run(args, ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("url") != std::string::npos);
    }

    TEST_CASE("empty url → error") {
        RemoteTriggerTool t({"*"});
        auto ctx  = make_ctx();
        Json args = {{"url", ""}, {"payload", "x"}};
        ToolResult r = t.run(args, ctx);
        CHECK(r.is_error);
    }

    TEST_CASE("missing payload → error") {
        RemoteTriggerTool t({"http://127.0.0.1:*/**"});
        auto ctx  = make_ctx();
        Json args = {{"url", "http://127.0.0.1:8080/hook"}};
        ToolResult r = t.run(args, ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("payload") != std::string::npos);
    }
}

// =============================================================================
// TEST SUITE: RemoteTriggerTool — URL allowlist (no network)
// =============================================================================

TEST_SUITE("RemoteTriggerTool — URL allowlist") {

    TEST_CASE("empty allowed_urls blocks every URL") {
        RemoteTriggerTool t({});  // no allowed URLs
        auto ctx  = make_ctx();
        Json args = {
            {"url",     "http://example.com/hook"},
            {"payload", Json{{"k", "v"}}}
        };
        ToolResult r = t.run(args, ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("allowed") != std::string::npos);
    }

    TEST_CASE("URL not matching any pattern is blocked") {
        RemoteTriggerTool t({"https://allowed.example.com/**"});
        auto ctx  = make_ctx();
        Json args = {
            {"url",     "https://other.example.com/hook"},
            {"payload", Json{{"k", "v"}}}
        };
        ToolResult r = t.run(args, ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("allowed") != std::string::npos);
    }

    TEST_CASE("URL not matching pattern using http: is blocked when pattern requires https:") {
        RemoteTriggerTool t({"https://example.com/**"});
        auto ctx  = make_ctx();
        Json args = {
            {"url",     "http://example.com/hook"},
            {"payload", "data"}
        };
        ToolResult r = t.run(args, ctx);
        CHECK(r.is_error);
    }
}

// =============================================================================
// TEST SUITE: RemoteTriggerTool — cancellation (no network)
// =============================================================================

TEST_SUITE("RemoteTriggerTool — cancellation") {

    TEST_CASE("pre-cancelled context returns error immediately") {
        RemoteTriggerTool t({"http://127.0.0.1:*/**"});
        auto [src, tok] = CancelToken::make_root();
        src.request_stop();

        ToolContext ctx = make_ctx();
        ctx.cancel_token = std::move(tok);

        Json args = {
            {"url",     "http://127.0.0.1:9999/hook"},
            {"payload", Json{{"k","v"}}}
        };
        ToolResult r = t.run(args, ctx);
        CHECK(r.is_error);
        CHECK(r.body == "cancelled");
    }
}

// =============================================================================
// TEST SUITE: RemoteTriggerTool — HTTP fixture integration
// =============================================================================

TEST_SUITE("RemoteTriggerTool — fixture HTTP server integration") {

    TEST_CASE("POST issued — 200 response returned as ok result") {
        REQUIRE(g_srv != nullptr);
        g_srv->reset(200, R"({"status":"triggered"})");

        const std::string target = g_srv->url("/trigger");
        RemoteTriggerTool t({g_srv->url("/**")});
        auto ctx  = make_ctx();
        Json args = {
            {"url",     target},
            {"payload", Json{{"event", "deploy"}, {"env", "staging"}}}
        };

        ToolResult r = t.run(args, ctx);
        REQUIRE_FALSE(r.is_error);
        CHECK(r.body.find("triggered") != std::string::npos);
        CHECK(g_srv->request_count.load() == 1);
    }

    TEST_CASE("POST issued with auth headers from config") {
        REQUIRE(g_srv != nullptr);
        g_srv->reset(200, "ok");

        const std::string target = g_srv->url("/trigger");
        RemoteTriggerTool t(
            {g_srv->url("/**")},
            {{"Authorization", "Bearer test-token-123"}}
        );
        auto ctx  = make_ctx();
        Json args = {
            {"url",     target},
            {"payload", Json{{"ping", true}}}
        };

        ToolResult r = t.run(args, ctx);
        REQUIRE_FALSE(r.is_error);
        CHECK(g_srv->last_received_auth_header == "Bearer test-token-123");
    }

    TEST_CASE("Content-Type: application/json is always set") {
        REQUIRE(g_srv != nullptr);
        g_srv->reset(200, "ok");

        const std::string target = g_srv->url("/trigger");
        RemoteTriggerTool t({g_srv->url("/**")});
        auto ctx  = make_ctx();
        Json args = {
            {"url",     target},
            {"payload", Json{{"x", 1}}}
        };

        ToolResult res = t.run(args, ctx);
        (void)res;
        CHECK(g_srv->last_received_content_type.find("application/json") != std::string::npos);
    }

    TEST_CASE("payload is serialized as JSON in the request body") {
        REQUIRE(g_srv != nullptr);
        g_srv->reset(200, "ok");

        const std::string target = g_srv->url("/trigger");
        RemoteTriggerTool t({g_srv->url("/**")});
        auto ctx  = make_ctx();
        Json payload{{"event", "test"}, {"value", 42}};
        Json args = {{"url", target}, {"payload", payload}};

        ToolResult res = t.run(args, ctx);
        (void)res;
        // Verify the server received valid JSON matching the payload.
        Json received = Json::parse(g_srv->last_received_body);
        CHECK(received["event"].get<std::string>() == "test");
        CHECK(received["value"].get<int>() == 42);
    }

    TEST_CASE("per-call headers override defaults") {
        REQUIRE(g_srv != nullptr);
        g_srv->reset(200, "ok");

        const std::string target = g_srv->url("/trigger");
        RemoteTriggerTool t(
            {g_srv->url("/**")},
            {{"Authorization", "Bearer default-token"}}
        );
        auto ctx  = make_ctx();
        Json args = {
            {"url",     target},
            {"payload", Json{{"x", 1}}},
            {"headers", Json{{"Authorization", "Bearer override-token"}}}
        };

        ToolResult res = t.run(args, ctx);
        (void)res;
        CHECK(g_srv->last_received_auth_header == "Bearer override-token");
    }

    TEST_CASE("non-2xx response — error with body excerpt") {
        REQUIRE(g_srv != nullptr);
        g_srv->reset(404, R"({"error":"not found"})");

        const std::string target = g_srv->url("/missing");
        RemoteTriggerTool t({g_srv->url("/**")});
        auto ctx  = make_ctx();
        Json args = {
            {"url",     target},
            {"payload", Json{{"x", 1}}}
        };

        ToolResult r = t.run(args, ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("404") != std::string::npos);
        // Body excerpt should appear in the error.
        CHECK(r.body.find("not found") != std::string::npos);
    }

    TEST_CASE("HTTP 500 — error with body excerpt") {
        REQUIRE(g_srv != nullptr);
        g_srv->reset(500, "Internal Server Error");

        const std::string target = g_srv->url("/fail");
        RemoteTriggerTool t({g_srv->url("/**")});
        auto ctx  = make_ctx();
        Json args = {
            {"url",     target},
            {"payload", Json{{"x", 1}}}
        };

        ToolResult r = t.run(args, ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("500") != std::string::npos);
    }

    TEST_CASE("URL allowlist — wildcard pattern matches local server") {
        REQUIRE(g_srv != nullptr);
        g_srv->reset(200, R"({"ok":true})");

        const std::string target = g_srv->url("/hook");
        // Use a broad wildcard pattern that matches any http://127.0.0.1 URL.
        RemoteTriggerTool t({"http://127.0.0.1:**"});
        auto ctx  = make_ctx();
        Json args = {{"url", target}, {"payload", Json{{"k", "v"}}}};

        ToolResult r = t.run(args, ctx);
        CHECK_FALSE(r.is_error);
    }

    TEST_CASE("URL outside allowlist does not issue POST") {
        REQUIRE(g_srv != nullptr);
        g_srv->reset(200, "ok");
        int before = g_srv->request_count.load();

        RemoteTriggerTool t({"https://only-https.example.com/**"});
        auto ctx  = make_ctx();
        Json args = {
            {"url",     g_srv->url("/hook")},  // http, not in allowlist
            {"payload", Json{{"x", 1}}}
        };

        ToolResult r = t.run(args, ctx);
        CHECK(r.is_error);
        // Confirm no request was issued to the fixture server.
        CHECK(g_srv->request_count.load() == before);
    }
}

// =============================================================================
// main() — start fixture server, run all tests, stop fixture server.
// =============================================================================

int main(int argc, char** argv) {
    doctest::Context ctx(argc, argv);

    FixtureServer srv;
    g_srv = &srv;

    int result = ctx.run();

    g_srv = nullptr;
    return result;
}
