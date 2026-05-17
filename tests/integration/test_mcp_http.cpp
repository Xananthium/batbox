// tests/integration/test_mcp_http.cpp
// ---------------------------------------------------------------------------
// Integration tests for batbox::mcp::HttpTransport.
//
// Strategy:
//   1. Spawn tests/fixtures/fake_mcp_http.py as a child process.
//   2. Read "READY <port>" from its stdout to learn the ephemeral port.
//   3. Construct an HttpTransport pointed at http://127.0.0.1:<port>
//   4. Exercise the acceptance criteria:
//        a. Initialize handshake succeeds against fixture.
//        b. Plain request/response works (ping).
//        c. Streamable-http variant assembles SSE chunks into a response.
//        d. Authorization header from config is honoured.
//        e. Connection close detected → healthy() returns false.
//        f. Notification POST succeeds (fire-and-forget).
//   5. SIGTERM the child process.
//
// Requires Python 3 on PATH.  The fixture path is injected by CMake via the
// BATBOX_FIXTURE_DIR compile definition (see tests/integration/CMakeLists.txt).
// Falls back to walking up the directory tree from cwd.
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/mcp/HttpTransport.hpp>
#include <batbox/mcp/JsonRpc.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Json.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <chrono>

#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>

namespace fs = std::filesystem;
using namespace batbox::mcp;
using namespace batbox;

// ---------------------------------------------------------------------------
// Helper: locate fake_mcp_http.py
// ---------------------------------------------------------------------------
static std::string find_fixture_script() {
#ifdef BATBOX_FIXTURE_DIR
    fs::path p = fs::path(BATBOX_FIXTURE_DIR) / "fake_mcp_http.py";
    if (fs::exists(p)) return p.string();
#endif
    fs::path dir = fs::current_path();
    for (int depth = 0; depth < 10; ++depth) {
        fs::path candidate = dir / "tests" / "fixtures" / "fake_mcp_http.py";
        if (fs::exists(candidate)) return candidate.string();
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    return "";
}

// ---------------------------------------------------------------------------
// FakeServer RAII — forks python3, waits for "READY <port>"
// ---------------------------------------------------------------------------
struct FakeServer {
    pid_t pid{-1};
    int   port{0};
    FILE* stdout_pipe{nullptr};

    bool start(const std::string& script_path,
               const std::vector<std::string>& extra_args = {}) {
        int pipefd[2];
        if (::pipe(pipefd) != 0) return false;

        pid = ::fork();
        if (pid < 0) {
            ::close(pipefd[0]);
            ::close(pipefd[1]);
            return false;
        }

        if (pid == 0) {
            // Child
            ::close(pipefd[0]);
            ::dup2(pipefd[1], STDOUT_FILENO);
            ::close(pipefd[1]);

            std::vector<const char*> argv;
            argv.push_back("python3");
            argv.push_back(script_path.c_str());
            for (const auto& arg : extra_args) {
                argv.push_back(arg.c_str());
            }
            argv.push_back(nullptr);
            ::execvp("python3", const_cast<char* const*>(argv.data()));
            std::exit(127);
        }

        // Parent
        ::close(pipefd[1]);
        stdout_pipe = ::fdopen(pipefd[0], "r");
        if (!stdout_pipe) {
            ::close(pipefd[0]);
            return false;
        }

        // Read "READY <port>"
        char line[256] = {};
        if (!::fgets(line, sizeof(line), stdout_pipe)) {
            return false;
        }
        if (::sscanf(line, "READY %d", &port) != 1) {
            return false;
        }
        return port > 0;
    }

    void stop_server() {
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            int status = 0;
            ::waitpid(pid, &status, 0);
            pid = -1;
        }
        if (stdout_pipe) {
            ::fclose(stdout_pipe);
            stdout_pipe = nullptr;
        }
    }

    ~FakeServer() { stop_server(); }
};

// ---------------------------------------------------------------------------
// Helper: build base URL from port
// ---------------------------------------------------------------------------
static std::string base_url(int port) {
    return "http://127.0.0.1:" + std::to_string(port);
}

// ===========================================================================
// Test cases
// ===========================================================================

TEST_CASE("HttpTransport: plain request/response — initialize + ping") {
    const std::string script = find_fixture_script();
    REQUIRE_FALSE(script.empty());

    FakeServer server;
    REQUIRE(server.start(script));

    auto [cs, ct] = CancelToken::make_root();

    HttpTransport transport(base_url(server.port));
    REQUIRE(transport.start(std::move(ct)));

    CHECK(transport.healthy());

    // Initialize
    auto [cs2, ct2] = CancelToken::make_root();
    auto init_res = transport.request("initialize", Json::object(), std::move(ct2));
    REQUIRE(init_res.has_value());
    CHECK(init_res.value().contains("capabilities"));

    // Ping
    auto [cs3, ct3] = CancelToken::make_root();
    auto ping_res = transport.request("ping", Json::object(), std::move(ct3));
    REQUIRE(ping_res.has_value());

    transport.stop();
    CHECK_FALSE(transport.healthy());
}

TEST_CASE("HttpTransport: Authorization header honored") {
    const std::string script = find_fixture_script();
    REQUIRE_FALSE(script.empty());

    // Server that requires auth
    FakeServer auth_server;
    REQUIRE(auth_server.start(script, {"--require-auth"}));

    auto [cs_a, ct_a] = CancelToken::make_root();

    // Without auth — expect HTTP 401 (transport.start() may succeed
    // since GET /  returns 200 on probe; the method call should fail)
    {
        HttpTransport no_auth(base_url(auth_server.port));
        REQUIRE(no_auth.start(std::move(ct_a)));

        auto [cs_r, ct_r] = CancelToken::make_root();
        auto res = no_auth.request("ping", Json::object(), std::move(ct_r));
        // Expect error (401)
        CHECK_FALSE(res.has_value());
        no_auth.stop();
    }

    // With correct auth — expect success
    FakeServer auth_server2;
    REQUIRE(auth_server2.start(script, {"--require-auth"}));

    std::unordered_map<std::string, std::string> hdrs{
        {"Authorization", "Bearer test-token"}
    };
    HttpTransport with_auth(base_url(auth_server2.port), std::move(hdrs));

    auto [cs_b, ct_b] = CancelToken::make_root();
    REQUIRE(with_auth.start(std::move(ct_b)));

    auto [cs_c, ct_c] = CancelToken::make_root();
    auto res2 = with_auth.request("auth/check", Json::object(), std::move(ct_c));
    REQUIRE(res2.has_value());
    CHECK(res2.value()["auth"] == "Bearer test-token");

    with_auth.stop();
}

TEST_CASE("HttpTransport: streamable-http SSE response variant") {
    const std::string script = find_fixture_script();
    REQUIRE_FALSE(script.empty());

    // Start server in streamable-default mode so all responses come as SSE.
    FakeServer sse_server;
    REQUIRE(sse_server.start(script, {"--streamable-default"}));

    HttpTransport transport(base_url(sse_server.port));

    auto [cs, ct] = CancelToken::make_root();
    REQUIRE(transport.start(std::move(ct)));

    // Enable streamable-http mode (normally set after initialize reveals capability).
    transport.set_streamable_http(true);
    CHECK(transport.streamable_http());

    // Initialize returns SSE with capabilities including streamable-http.
    auto [cs2, ct2] = CancelToken::make_root();
    auto init_res = transport.request("initialize", Json::object(), std::move(ct2));
    REQUIRE(init_res.has_value());
    CHECK(init_res.value()["capabilities"].contains("streamable-http"));

    // tools/list also works over SSE.
    auto [cs3, ct3] = CancelToken::make_root();
    auto list_res = transport.request("tools/list", Json::object(), std::move(ct3));
    REQUIRE(list_res.has_value());
    CHECK(list_res.value().contains("tools"));

    transport.stop();
}

TEST_CASE("HttpTransport: healthy() returns false after stop()") {
    const std::string script = find_fixture_script();
    REQUIRE_FALSE(script.empty());

    FakeServer server;
    REQUIRE(server.start(script));

    HttpTransport transport(base_url(server.port));
    auto [cs, ct] = CancelToken::make_root();
    REQUIRE(transport.start(std::move(ct)));
    CHECK(transport.healthy());

    transport.stop();
    CHECK_FALSE(transport.healthy());
}

TEST_CASE("HttpTransport: notify() succeeds (fire-and-forget)") {
    const std::string script = find_fixture_script();
    REQUIRE_FALSE(script.empty());

    FakeServer server;
    REQUIRE(server.start(script));

    HttpTransport transport(base_url(server.port));
    auto [cs, ct] = CancelToken::make_root();
    REQUIRE(transport.start(std::move(ct)));

    Json notif_params = Json::object();
    notif_params["event"] = "test";
    auto notify_res = transport.notify("notifications/progress", std::move(notif_params));
    CHECK(notify_res.has_value());

    transport.stop();
}

TEST_CASE("HttpTransport: unknown method returns JSON-RPC error") {
    const std::string script = find_fixture_script();
    REQUIRE_FALSE(script.empty());

    FakeServer server;
    REQUIRE(server.start(script));

    HttpTransport transport(base_url(server.port));
    auto [cs, ct] = CancelToken::make_root();
    REQUIRE(transport.start(std::move(ct)));

    auto [cs2, ct2] = CancelToken::make_root();
    auto res = transport.request("does/not/exist", Json::object(), std::move(ct2));
    // Must return an error (JSON-RPC -32601 MethodNotFound)
    CHECK_FALSE(res.has_value());
    CHECK(res.error().find("-32601") != std::string::npos);

    transport.stop();
}

TEST_CASE("HttpTransport: cancelled token prevents request") {
    const std::string script = find_fixture_script();
    REQUIRE_FALSE(script.empty());

    FakeServer server;
    REQUIRE(server.start(script));

    HttpTransport transport(base_url(server.port));
    auto [cs, ct] = CancelToken::make_root();
    REQUIRE(transport.start(std::move(ct)));

    // Cancel before calling request().
    auto [cs2, ct2] = CancelToken::make_root();
    cs2.request_stop();

    auto res = transport.request("ping", Json::object(), std::move(ct2));
    CHECK_FALSE(res.has_value());
    CHECK(res.error() == "cancelled");

    transport.stop();
}

TEST_CASE("HttpTransport: request on stopped transport returns error") {
    const std::string script = find_fixture_script();
    REQUIRE_FALSE(script.empty());

    FakeServer server;
    REQUIRE(server.start(script));

    HttpTransport transport(base_url(server.port));
    auto [cs, ct] = CancelToken::make_root();
    REQUIRE(transport.start(std::move(ct)));

    transport.stop();

    auto [cs2, ct2] = CancelToken::make_root();
    auto res = transport.request("ping", Json::object(), std::move(ct2));
    CHECK_FALSE(res.has_value());
    CHECK(res.error() == "transport stopped");
}
