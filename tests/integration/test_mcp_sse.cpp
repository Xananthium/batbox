// tests/integration/test_mcp_sse.cpp
// ---------------------------------------------------------------------------
// Integration tests for SseTransport.
//
// Spawns tests/fixtures/fake_mcp_sse.py as a child process, connects
// SseTransport to it, exercises the full request/response and notification
// paths, then sends SIGTERM to clean up.
//
// Acceptance criteria tested:
//   AC1. Initial /sse connection succeeds, endpoint URL captured
//   AC2. Outgoing POST to endpoint URL with JSON-RPC body
//   AC3. Response correlated by id from the SSE stream
//   AC4. Connection drop detected -> healthy() false
//   AC5. Integration test with fixture
//
// Build standalone (from repo root):
//   cmake --build build --target test_mcp_sse
//   ctest --test-dir build -R test_mcp_sse -V
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/mcp/SseTransport.hpp>
#include <batbox/core/CancelToken.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// BATBOX_FIXTURE_DIR is injected by CMake in the test target's compile defs.
// ---------------------------------------------------------------------------
#ifndef BATBOX_FIXTURE_DIR
#  define BATBOX_FIXTURE_DIR "tests/fixtures"
#endif

// ---------------------------------------------------------------------------
// Helper: spawn fake_mcp_sse.py and return (pid, port).
// Reads "READY <port>" from the process stdout before returning.
// Returns pid=-1 on failure.
// ---------------------------------------------------------------------------
struct FakeServer {
    pid_t pid  = -1;
    int   port = 0;
    FILE* stdout_pipe = nullptr;

    ~FakeServer() { stop(); }

    bool start() {
        std::string script =
            std::string(BATBOX_FIXTURE_DIR) + "/fake_mcp_sse.py";

        // Create a pipe to read the child's stdout.
        int pipefd[2];
        if (::pipe(pipefd) != 0) return false;

        pid_t child = ::fork();
        if (child < 0) {
            ::close(pipefd[0]);
            ::close(pipefd[1]);
            return false;
        }

        if (child == 0) {
            // Child: redirect stdout to write end of pipe.
            ::close(pipefd[0]);
            ::dup2(pipefd[1], STDOUT_FILENO);
            ::close(pipefd[1]);
            // Suppress child stderr noise.
            int devnull = ::open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                ::dup2(devnull, STDERR_FILENO);
                ::close(devnull);
            }
            ::execl("/usr/bin/env", "env", "python3", script.c_str(), nullptr);
            ::_exit(127);
        }

        // Parent: close write end, read from child.
        ::close(pipefd[1]);
        stdout_pipe = ::fdopen(pipefd[0], "r");
        if (!stdout_pipe) {
            ::kill(child, SIGKILL);
            ::waitpid(child, nullptr, 0);
            ::close(pipefd[0]);
            return false;
        }

        pid = child;

        // Read "READY <port>\n" from the child.
        char line[256] = {};
        if (::fgets(line, sizeof(line), stdout_pipe) == nullptr) {
            stop();
            return false;
        }

        // Parse "READY <port>".
        int p = 0;
        if (::sscanf(line, "READY %d", &p) != 1) {
            stop();
            return false;
        }
        port = p;
        return true;
    }

    void stop() {
        if (stdout_pipe) {
            ::fclose(stdout_pipe);
            stdout_pipe = nullptr;
        }
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            int status;
            // Wait up to 3 seconds.
            for (int i = 0; i < 30; ++i) {
                pid_t r = ::waitpid(pid, &status, WNOHANG);
                if (r == pid) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            // Force kill if still running.
            ::kill(pid, SIGKILL);
            ::waitpid(pid, nullptr, 0);
            pid = -1;
        }
    }
};

// ---------------------------------------------------------------------------
// Convenience: make a never-cancelled CancelToken.
// ---------------------------------------------------------------------------
static batbox::CancelToken make_token() {
    return batbox::CancelToken{};
}

// ===========================================================================
// Test suite
// ===========================================================================

TEST_SUITE("SseTransport integration") {

// ---------------------------------------------------------------------------
// AC1 + AC5: start() succeeds and healthy() becomes true after endpoint event.
// ---------------------------------------------------------------------------
TEST_CASE("start() connects and captures endpoint URL") {
    FakeServer server;
    REQUIRE(server.start());
    INFO("fake_mcp_sse.py bound to port " << server.port);

    std::string url = "http://127.0.0.1:" + std::to_string(server.port) + "/sse";
    batbox::mcp::SseTransport transport{url};

    auto result = transport.start(make_token());
    REQUIRE(result.has_value());
    CHECK(transport.healthy());
}

// ---------------------------------------------------------------------------
// AC2 + AC3: request() POSTs JSON-RPC and receives a correlated response via SSE.
// ---------------------------------------------------------------------------
TEST_CASE("request() sends POST and receives correlated SSE response") {
    FakeServer server;
    REQUIRE(server.start());

    std::string url = "http://127.0.0.1:" + std::to_string(server.port) + "/sse";
    batbox::mcp::SseTransport transport{url};
    REQUIRE(transport.start(make_token()).has_value());

    // Send a "ping" request — the fixture responds with {}.
    auto [src, ct] = batbox::CancelToken::make_root();
    auto response = transport.request("ping", batbox::Json(nullptr), std::move(ct));

    CHECK(response.has_value());
    if (response.has_value()) {
        CHECK(response->is_object());
    }
}

// ---------------------------------------------------------------------------
// AC2 + AC3: echo request round-trips params.
// ---------------------------------------------------------------------------
TEST_CASE("request() echo method round-trips params") {
    FakeServer server;
    REQUIRE(server.start());

    std::string url = "http://127.0.0.1:" + std::to_string(server.port) + "/sse";
    batbox::mcp::SseTransport transport{url};
    REQUIRE(transport.start(make_token()).has_value());

    batbox::Json params = batbox::Json::object();
    params["message"] = "hello";

    auto [src, ct] = batbox::CancelToken::make_root();
    auto response = transport.request("echo", params, std::move(ct));

    CHECK(response.has_value());
    if (response.has_value()) {
        CHECK(response->contains("echo"));
    }
}

// ---------------------------------------------------------------------------
// AC2: notify() POSTs a notification and gets 202 back.
// ---------------------------------------------------------------------------
TEST_CASE("notify() sends notification without waiting for response") {
    FakeServer server;
    REQUIRE(server.start());

    std::string url = "http://127.0.0.1:" + std::to_string(server.port) + "/sse";
    batbox::mcp::SseTransport transport{url};
    REQUIRE(transport.start(make_token()).has_value());

    batbox::Json params = batbox::Json::object();
    params["event"] = "test";

    auto result = transport.notify("notifications/event", params);
    CHECK(result.has_value());
}

// ---------------------------------------------------------------------------
// Multiple sequential requests — verifies id correlation with different ids.
// ---------------------------------------------------------------------------
TEST_CASE("multiple sequential requests are correlated correctly") {
    FakeServer server;
    REQUIRE(server.start());

    std::string url = "http://127.0.0.1:" + std::to_string(server.port) + "/sse";
    batbox::mcp::SseTransport transport{url};
    REQUIRE(transport.start(make_token()).has_value());

    for (int i = 0; i < 3; ++i) {
        auto [src, ct] = batbox::CancelToken::make_root();
        auto resp = transport.request("ping", batbox::Json(nullptr), std::move(ct));
        CHECK(resp.has_value());
    }
}

// ---------------------------------------------------------------------------
// AC1: start() is idempotent on an already-healthy transport.
// ---------------------------------------------------------------------------
TEST_CASE("start() is idempotent on healthy transport") {
    FakeServer server;
    REQUIRE(server.start());

    std::string url = "http://127.0.0.1:" + std::to_string(server.port) + "/sse";
    batbox::mcp::SseTransport transport{url};
    REQUIRE(transport.start(make_token()).has_value());
    CHECK(transport.healthy());

    // Second call should return ok immediately.
    auto second = transport.start(make_token());
    CHECK(second.has_value());
    CHECK(transport.healthy());
}

// ---------------------------------------------------------------------------
// AC4: healthy() becomes false after stop().
// ---------------------------------------------------------------------------
TEST_CASE("healthy() is false after stop()") {
    FakeServer server;
    REQUIRE(server.start());

    std::string url = "http://127.0.0.1:" + std::to_string(server.port) + "/sse";
    batbox::mcp::SseTransport transport{url};
    REQUIRE(transport.start(make_token()).has_value());
    CHECK(transport.healthy());

    transport.stop();
    CHECK_FALSE(transport.healthy());
}

// ---------------------------------------------------------------------------
// AC4: healthy() is false after the server drops the connection.
// ---------------------------------------------------------------------------
TEST_CASE("healthy() becomes false when server stops") {
    FakeServer server;
    REQUIRE(server.start());

    std::string url = "http://127.0.0.1:" + std::to_string(server.port) + "/sse";
    batbox::mcp::SseTransport transport{url};
    REQUIRE(transport.start(make_token()).has_value());
    CHECK(transport.healthy());

    // Kill the server — SSE stream will close.
    server.stop();

    // Give the reader thread a moment to detect the disconnect.
    for (int i = 0; i < 50; ++i) {
        if (!transport.healthy()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    CHECK_FALSE(transport.healthy());
}

// ---------------------------------------------------------------------------
// Cancellation: start() returns Err("cancelled") when token fires before
// the fixture sends the endpoint event (tested by connecting to a non-
// existent server so cpr blocks, then cancelling).
// ---------------------------------------------------------------------------
TEST_CASE("start() returns cancelled when token fires") {
    // Use an invalid port — cpr::Download will try to connect but fail fast.
    // We cancel immediately to test the cancellation path.
    std::string url = "http://127.0.0.1:19999/sse";  // no server here
    batbox::mcp::SseTransport transport{url};

    auto [src, ct] = batbox::CancelToken::make_root();
    src.request_stop(); // Cancel before we even call start().

    auto result = transport.start(std::move(ct));
    CHECK_FALSE(result.has_value());
    if (!result.has_value()) {
        CHECK(result.error() == "cancelled");
    }
}

// ---------------------------------------------------------------------------
// request() on a stopped transport returns Err("transport stopped").
// ---------------------------------------------------------------------------
TEST_CASE("request() on stopped transport returns error") {
    FakeServer server;
    REQUIRE(server.start());

    std::string url = "http://127.0.0.1:" + std::to_string(server.port) + "/sse";
    batbox::mcp::SseTransport transport{url};
    REQUIRE(transport.start(make_token()).has_value());

    transport.stop();

    auto [src, ct] = batbox::CancelToken::make_root();
    auto result = transport.request("ping", batbox::Json(nullptr), std::move(ct));
    CHECK_FALSE(result.has_value());
    if (!result.has_value()) {
        CHECK(result.error().find("transport") != std::string::npos);
    }
}

} // TEST_SUITE
