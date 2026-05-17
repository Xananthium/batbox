// tests/integration/test_mcp_ws.cpp
// ---------------------------------------------------------------------------
// Integration test for batbox::mcp::WsTransport.
//
// Strategy:
//   1. Each TEST_CASE spawns tests/fixtures/fake_mcp_ws.py independently.
//   2. Reads "READY <port>" from its stdout to learn the ephemeral port.
//   3. Constructs WsTransport with "ws://127.0.0.1:<port>".
//   4. Exercises the acceptance criteria.
//   5. SIGTERM the child process when done (RAII via FakeWsServer::stop()).
//
// Why per-test-case server:
//   doctest_discover_tests() registers each TEST_CASE as a separate CTest
//   entry, launched as its own process.  Process-global state (g_server,
//   g_server_started) is therefore reset to the initial value at the start
//   of every CTest invocation.  Making each test case spin up its own server
//   removes this dependency while keeping test isolation.
//
// Requires: Python 3 + websockets (pip install websockets) on PATH.
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/mcp/WsTransport.hpp>
#include <batbox/core/CancelToken.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace batbox;
using namespace batbox::mcp;

// ---------------------------------------------------------------------------
// Locate fake_mcp_ws.py
// ---------------------------------------------------------------------------
static std::string find_fixture_script() {
#ifdef BATBOX_FIXTURE_DIR
    fs::path p = fs::path(BATBOX_FIXTURE_DIR) / "fake_mcp_ws.py";
    if (fs::exists(p)) return p.string();
#endif
    fs::path dir = fs::current_path();
    for (int depth = 0; depth < 8; ++depth) {
        fs::path candidate = dir / "tests" / "fixtures" / "fake_mcp_ws.py";
        if (fs::exists(candidate)) return candidate.string();
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    return "";
}

// ---------------------------------------------------------------------------
// FakeWsServer RAII — forks python3, waits for "READY <port>" on stdout.
// ---------------------------------------------------------------------------
struct FakeWsServer {
    pid_t pid{-1};
    int   port{0};
    FILE* stdout_pipe{nullptr};

    bool start(const std::string& script_path) {
        int pipefd[2];
        if (::pipe(pipefd) != 0) return false;

        pid = ::fork();
        if (pid < 0) {
            ::close(pipefd[0]);
            ::close(pipefd[1]);
            return false;
        }

        if (pid == 0) {
            // Child: redirect stdout to write end of pipe.
            ::close(pipefd[0]);
            ::dup2(pipefd[1], STDOUT_FILENO);
            ::close(pipefd[1]);
            // Suppress stderr noise from the fixture.
            int devnull = ::open("/dev/null", O_WRONLY);
            if (devnull >= 0) { ::dup2(devnull, STDERR_FILENO); ::close(devnull); }
            ::execlp("python3", "python3", script_path.c_str(), nullptr);
            ::_exit(127);
        }

        // Parent: read "READY <port>" from the pipe.
        ::close(pipefd[1]);
        stdout_pipe = ::fdopen(pipefd[0], "r");
        if (!stdout_pipe) { ::kill(pid, SIGTERM); pid = -1; return false; }

        char line[256] = {};
        if (!::fgets(line, sizeof(line), stdout_pipe)) {
            ::fclose(stdout_pipe); stdout_pipe = nullptr;
            ::kill(pid, SIGTERM); pid = -1;
            return false;
        }

        if (::sscanf(line, "READY %d", &port) != 1) {
            ::fclose(stdout_pipe); stdout_pipe = nullptr;
            ::kill(pid, SIGTERM); pid = -1;
            return false;
        }
        return true;
    }

    void stop() {
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            int status = 0;
            ::waitpid(pid, &status, 0);
            pid = -1;
        }
        if (stdout_pipe) { ::fclose(stdout_pipe); stdout_pipe = nullptr; }
    }

    ~FakeWsServer() { stop(); }
};

// ---------------------------------------------------------------------------
// Helper: build a ws:// URL from the fixture's port.
// ---------------------------------------------------------------------------
static std::string ws_url(int port) {
    return "ws://127.0.0.1:" + std::to_string(port);
}

// ---------------------------------------------------------------------------
// AC1 — ws:// URL connects, healthy() true after start().
// ---------------------------------------------------------------------------
TEST_CASE("WsTransport: ws:// connects and healthy()") {
    const std::string script_path = find_fixture_script();
    REQUIRE_FALSE(script_path.empty());

    FakeWsServer server;
    REQUIRE(server.start(script_path));
    // Give the server a moment to finish initialising.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    WsTransport t(ws_url(server.port));

    auto [src, ct] = CancelToken::make_root();
    auto res = t.start(std::move(ct));
    REQUIRE(res.has_value());
    CHECK(t.healthy());

    t.stop();
    CHECK_FALSE(t.healthy());
}

// ---------------------------------------------------------------------------
// AC2 — Initialize handshake: ping → "pong".
// ---------------------------------------------------------------------------
TEST_CASE("WsTransport: ping/pong (init-style handshake)") {
    const std::string script_path = find_fixture_script();
    REQUIRE_FALSE(script_path.empty());

    FakeWsServer server;
    REQUIRE(server.start(script_path));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    WsTransport t(ws_url(server.port));

    auto [src, ct] = CancelToken::make_root();
    REQUIRE(t.start(std::move(ct)).has_value());

    auto [rsrc, rct] = CancelToken::make_root();
    auto res = t.request("ping", Json(nullptr), std::move(rct));
    REQUIRE(res.has_value());
    CHECK(res->get<std::string>() == "pong");

    t.stop();
}

// ---------------------------------------------------------------------------
// AC3 — Request/response correlated by id (echo).
// ---------------------------------------------------------------------------
TEST_CASE("WsTransport: echo request/response correlated by id") {
    const std::string script_path = find_fixture_script();
    REQUIRE_FALSE(script_path.empty());

    FakeWsServer server;
    REQUIRE(server.start(script_path));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    WsTransport t(ws_url(server.port));

    auto [src, ct] = CancelToken::make_root();
    REQUIRE(t.start(std::move(ct)).has_value());

    Json params = {{"msg", "hello"}, {"num", 42}};
    auto [rsrc, rct] = CancelToken::make_root();
    auto res = t.request("echo", params, std::move(rct));
    REQUIRE(res.has_value());
    CHECK((*res)["msg"].get<std::string>() == "hello");
    CHECK((*res)["num"].get<int>() == 42);

    t.stop();
}

// ---------------------------------------------------------------------------
// AC4 — Connection close → healthy() false.
// ---------------------------------------------------------------------------
TEST_CASE("WsTransport: connection close sets healthy() false") {
    const std::string script_path = find_fixture_script();
    REQUIRE_FALSE(script_path.empty());

    FakeWsServer server;
    REQUIRE(server.start(script_path));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    WsTransport t(ws_url(server.port));

    auto [src, ct] = CancelToken::make_root();
    REQUIRE(t.start(std::move(ct)).has_value());
    CHECK(t.healthy());

    t.stop();
    CHECK_FALSE(t.healthy());
}

// ---------------------------------------------------------------------------
// AC5 — JSON-RPC error response propagated as Err.
// ---------------------------------------------------------------------------
TEST_CASE("WsTransport: JSON-RPC error propagated as Err") {
    const std::string script_path = find_fixture_script();
    REQUIRE_FALSE(script_path.empty());

    FakeWsServer server;
    REQUIRE(server.start(script_path));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    WsTransport t(ws_url(server.port));

    auto [src, ct] = CancelToken::make_root();
    REQUIRE(t.start(std::move(ct)).has_value());

    auto [rsrc, rct] = CancelToken::make_root();
    auto res = t.request("error_method", Json(nullptr), std::move(rct));
    REQUIRE_FALSE(res.has_value());
    // Error string must contain the code and message.
    CHECK(res.error().find("-32000") != std::string::npos);
    CHECK(res.error().find("deliberate error") != std::string::npos);

    t.stop();
}

// ---------------------------------------------------------------------------
// AC6 — Server-pushed notification received by on_notification handler.
// ---------------------------------------------------------------------------
TEST_CASE("WsTransport: on_notification handler receives server notification") {
    const std::string script_path = find_fixture_script();
    REQUIRE_FALSE(script_path.empty());

    FakeWsServer server;
    REQUIRE(server.start(script_path));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    WsTransport t(ws_url(server.port));

    std::atomic<int>  notif_count{0};
    std::string       notif_method;
    Json              notif_params;
    std::mutex        notif_mu;
    std::condition_variable notif_cv;

    t.on_notification([&](std::string method, Json params) {
        std::lock_guard<std::mutex> lk(notif_mu);
        notif_method = std::move(method);
        notif_params = std::move(params);
        notif_count.fetch_add(1, std::memory_order_release);
        notif_cv.notify_all();
    });

    auto [src, ct] = CancelToken::make_root();
    REQUIRE(t.start(std::move(ct)).has_value());

    // Ask the server to push a notification then reply.
    auto [rsrc, rct] = CancelToken::make_root();
    auto res = t.request("push_notification", Json(nullptr), std::move(rct));
    REQUIRE(res.has_value());
    CHECK(res->get<std::string>() == "sent");

    // Wait up to 2 seconds for the notification to arrive.
    {
        std::unique_lock<std::mutex> lk(notif_mu);
        bool got = notif_cv.wait_for(lk, std::chrono::seconds(2),
                                     [&] { return notif_count.load() > 0; });
        CHECK(got);
        if (got) {
            CHECK(notif_method == "notifications/test");
            CHECK(notif_params["n"].get<int>() == 1);
        }
    }

    t.stop();
}

// ---------------------------------------------------------------------------
// Extra: notify() fire-and-forget (no crash, returns Ok).
// ---------------------------------------------------------------------------
TEST_CASE("WsTransport: notify() fire-and-forget returns Ok") {
    const std::string script_path = find_fixture_script();
    REQUIRE_FALSE(script_path.empty());

    FakeWsServer server;
    REQUIRE(server.start(script_path));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    WsTransport t(ws_url(server.port));

    auto [src, ct] = CancelToken::make_root();
    REQUIRE(t.start(std::move(ct)).has_value());

    Json notif_params = {{"key", "value"}};
    auto res = t.notify("notifications/progress", notif_params);
    CHECK(res.has_value());

    t.stop();
}

// ---------------------------------------------------------------------------
// Extra: request() on stopped transport returns Err("transport stopped").
// ---------------------------------------------------------------------------
TEST_CASE("WsTransport: request on stopped transport returns Err") {
    WsTransport t("ws://127.0.0.1:1");  // unreachable port — never call start().

    auto [rsrc, rct] = CancelToken::make_root();
    auto res = t.request("ping", Json(nullptr), std::move(rct));
    REQUIRE_FALSE(res.has_value());
    // Either "transport stopped" or "transport disconnected" is acceptable.
    CHECK_FALSE(res.error().empty());
}
