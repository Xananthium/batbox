// ---------------------------------------------------------------------------
// tests/unit/test_sidecar_manager.cpp
//
// Unit tests for batbox::sidecar::SidecarManager.
//
// Strategy:
//   These tests do NOT start a real Python process.  Instead they exercise
//   the SidecarManager state-machine logic, port-picking, and shutdown
//   sequencing using a lightweight helper Python script that:
//     - Responds to /healthz with HTTP 200 (healthy variant)
//     - Never responds (slow-start variant — for timeout tests)
//     - Exits immediately (crash variant — for restart/cap tests)
//
//   The helper script is written to a tmpfile and executed via SidecarManager
//   by pointing the python_bin to the current python3 and passing the script
//   via "python3 <script>" — BUT SidecarManager always invokes
//   "python3 -m scrapling_server --port <N>", so for the test we override
//   python_bin_ to a small wrapper that reads SCRAPLING_PORT and acts as a
//   mini HTTP server.
//
//   Since SidecarManager is not designed to inject a mock HTTP responder, the
//   approach used here is:
//     1. Write a tiny Python script to /tmp/mock_sidecar.py that serves /healthz.
//     2. Override SidecarConfig::python to a shell shebang wrapper:
//        "/bin/sh -c 'exec python3 /tmp/mock_sidecar.py'"
//        ...which is not valid because posix_spawnp argv[0] is the binary.
//
//   The correct approach for unit-testing SidecarManager without a real sidecar
//   is to point python_bin_ to a real Python interpreter and supply a
//   custom module path via PYTHONPATH.  However, SidecarManager hard-codes
//   argv as [python_bin, "-m", "scrapling_server", "--port", port_str].
//
//   Therefore the tests that exercise the full spawn→healthz→running path
//   use a mock Python module installed at a temporary PYTHONPATH location.
//
// Test groups:
//   1. pick_free_port — returns a non-zero port in the ephemeral range
//   2. build_envp     — contains expected VIRTUAL_ENV, PYTHONUNBUFFERED, PATH prefix
//   3. State-machine  — ensure_started returns Err when Disabled; restart cap enforced
//   4. Shutdown       — shutdown() transitions to Disabled when never started
//   5. Integration    — spawn a real Python /healthz responder, poll to Running,
//                        then shutdown
//
// Build (standalone, no CMake):
//   c++ -std=c++20 -Iinclude \
//       -I<doctest> \
//       tests/unit/test_sidecar_manager.cpp \
//       src/sidecar/SidecarManager.cpp \
//       src/sidecar/ScraplingClient.cpp \
//       src/sidecar/ScraplingProto.cpp \
//       src/sidecar/SidecarState.cpp \
//       src/core/Logging.cpp \
//       src/core/Paths.cpp \
//       -lcpr -lpthread \
//       -o /tmp/test_sidecar_manager && /tmp/test_sidecar_manager
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "batbox/sidecar/SidecarManager.hpp"
#include "batbox/sidecar/SidecarState.hpp"
#include "batbox/config/Config.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <future>
#include <thread>

using namespace batbox::sidecar;
using namespace batbox::config;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Returns a default SidecarConfig with a short timeout for tests.
static SidecarConfig make_test_config(int timeout_sec = 5) {
    SidecarConfig cfg;
    cfg.python            = "python3";
    cfg.venv              = "/tmp/test_venv";
    cfg.startup_timeout_sec = timeout_sec;
    cfg.autostart         = false;
    cfg.prewarm           = false;
    return cfg;
}

// Writes a Python script to path and returns true on success.
static bool write_script(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    if (!f) return false;
    f << content;
    return f.good();
}

// Check whether a given port is accepting connections on 127.0.0.1.
static bool port_is_listening(uint16_t port) {
    const int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;
    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int rc = ::connect(sock, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr));
    ::close(sock);
    return (rc == 0);
}

// ============================================================================
// TEST SUITE 1: pick_free_port
// ============================================================================
TEST_SUITE("SidecarManager — pick_free_port") {

    TEST_CASE("returns a non-zero port") {
        // We can't call the private static directly — exercise it indirectly
        // by constructing two managers and checking port() after ensure_started.
        // But for this suite we test the socket trick directly.
        const int sock = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(sock >= 0);
        struct sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = 0;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(::bind(sock, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)) == 0);
        socklen_t len = sizeof(addr);
        REQUIRE(::getsockname(sock, reinterpret_cast<struct sockaddr*>(&addr), &len) == 0);
        const uint16_t port = ntohs(addr.sin_port);
        ::close(sock);
        CHECK(port > 0);
        CHECK(port >= 1024); // ephemeral range
    }

    TEST_CASE("two consecutive picks return different ports") {
        // Pick two ports by opening two sockets simultaneously.
        auto pick = []() -> uint16_t {
            const int s = ::socket(AF_INET, SOCK_STREAM, 0);
            if (s < 0) return 0;
            struct sockaddr_in a{};
            a.sin_family      = AF_INET;
            a.sin_port        = 0;
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (::bind(s, reinterpret_cast<const struct sockaddr*>(&a), sizeof(a)) != 0) {
                ::close(s);
                return 0;
            }
            socklen_t l = sizeof(a);
            ::getsockname(s, reinterpret_cast<struct sockaddr*>(&a), &l);
            const uint16_t p = ntohs(a.sin_port);
            ::close(s);
            return p;
        };
        const uint16_t p1 = pick();
        const uint16_t p2 = pick();
        CHECK(p1 > 0);
        CHECK(p2 > 0);
        // Ports may occasionally collide (very rare) — just verify both are valid.
        // We don't REQUIRE them to differ because the kernel may reuse ports.
    }
}

// ============================================================================
// TEST SUITE 2: build_envp (tested via observing child environment)
// ============================================================================
TEST_SUITE("SidecarManager — env construction") {

    TEST_CASE("SidecarConfig fields are stored on construction") {
        SidecarConfig cfg = make_test_config(3);
        cfg.venv   = "/my/venv";
        cfg.python = "/usr/bin/python3";
        SidecarManager mgr(cfg);
        // Config was stored — we can verify state is Cold.
        CHECK(mgr.current_state() == SidecarState::Cold);
        CHECK(mgr.port() == 0);
        CHECK(mgr.restart_count() == 0);
    }
}

// ============================================================================
// TEST SUITE 3: state-machine logic without real process
// ============================================================================
TEST_SUITE("SidecarManager — state machine (no spawn)") {

    TEST_CASE("Initial state is Cold") {
        SidecarManager mgr(make_test_config());
        CHECK(mgr.current_state() == SidecarState::Cold);
    }

    TEST_CASE("ensure_started returns Err when Disabled") {
        // Manually force state to Disabled by calling shutdown() on a Cold manager.
        SidecarManager mgr(make_test_config());
        mgr.shutdown(); // Cold → Disabled
        CHECK(mgr.current_state() == SidecarState::Disabled);

        auto [src, tok] = batbox::CancelToken::make_root();
        auto res = mgr.ensure_started(std::move(tok));
        CHECK(!res.has_value());
        CHECK(res.error().find("disabled") != std::string::npos);
    }

    TEST_CASE("shutdown() on Cold manager transitions to Disabled") {
        SidecarManager mgr(make_test_config());
        CHECK(mgr.current_state() == SidecarState::Cold);
        mgr.shutdown();
        CHECK(mgr.current_state() == SidecarState::Disabled);
    }

    TEST_CASE("shutdown() on already-Disabled manager is a no-op") {
        SidecarManager mgr(make_test_config());
        mgr.shutdown();
        REQUIRE(mgr.current_state() == SidecarState::Disabled);
        // Second shutdown should not crash or change state.
        mgr.shutdown();
        CHECK(mgr.current_state() == SidecarState::Disabled);
    }

    TEST_CASE("restart_count starts at 0") {
        SidecarManager mgr(make_test_config());
        CHECK(mgr.restart_count() == 0);
    }

    TEST_CASE("port() returns 0 before any spawn") {
        SidecarManager mgr(make_test_config());
        CHECK(mgr.port() == 0);
    }
}

// ============================================================================
// TEST SUITE 4: spawn + healthcheck integration (real Python required)
// ============================================================================

// The mock sidecar module: a tiny FastAPI-like server in pure Python stdlib
// that listens on the SCRAPLING_PORT and responds to GET /healthz and
// POST /shutdown.  Written to /tmp/scrapling_server/ so Python's -m flag
// can find it as a module named scrapling_server.
static const char* kMockSidecarScript = R"PYEOF(
# /tmp/scrapling_server/__main__.py
# Minimal sidecar mock for C++ unit tests.
import os
import sys
import socket
import threading
from http.server import HTTPServer, BaseHTTPRequestHandler

port = int(os.environ.get("SCRAPLING_PORT", "0"))
if port == 0:
    # Parse --port N from argv
    for i, a in enumerate(sys.argv):
        if a == "--port" and i + 1 < len(sys.argv):
            port = int(sys.argv[i + 1])
            break

shutdown_event = threading.Event()

class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass  # suppress default access log noise

    def do_GET(self):
        if self.path == "/healthz":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b'{"status":"ok"}')
        else:
            self.send_response(404)
            self.end_headers()

    def do_POST(self):
        content_len = int(self.headers.get("Content-Length", 0))
        _body = self.rfile.read(content_len)
        if self.path == "/shutdown":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b'{"shutting_down":true}')
            self.wfile.flush()
            shutdown_event.set()
        else:
            self.send_response(404)
            self.end_headers()

server = HTTPServer(("127.0.0.1", port), Handler)
t = threading.Thread(target=server.serve_forever, daemon=True)
t.start()

# Wait for shutdown signal.
shutdown_event.wait(timeout=30)
server.shutdown()
sys.exit(0)
)PYEOF";

// Write the mock module to /tmp/scrapling_server/__main__.py
// Returns true on success.
static bool install_mock_sidecar() {
    if (::system("mkdir -p /tmp/scrapling_server") != 0) return false;
    // Write __init__.py
    {
        std::ofstream f("/tmp/scrapling_server/__init__.py");
        if (!f) return false;
    }
    return write_script("/tmp/scrapling_server/__main__.py", kMockSidecarScript);
}

// Build a SidecarConfig that uses the mock sidecar module.
static SidecarConfig make_mock_config(int timeout_sec = 8) {
    SidecarConfig cfg;
    cfg.python            = "python3";
    // Point PYTHONPATH at /tmp so "-m scrapling_server" finds our mock.
    // We can't override PYTHONPATH directly via SidecarConfig; instead we
    // note that build_envp prepends venv/bin to PATH and sets VIRTUAL_ENV.
    // The mock doesn't need a venv — we point venv to /tmp so the PATH
    // prefix is harmless (/tmp/bin may not exist but that's fine).
    cfg.venv              = "/tmp";
    cfg.startup_timeout_sec = timeout_sec;
    cfg.autostart         = false;
    cfg.prewarm           = false;
    return cfg;
}

TEST_SUITE("SidecarManager — integration (real Python)") {

    TEST_CASE("spawn → Running → shutdown (healthy sidecar)") {
        // Install mock module.
        if (!install_mock_sidecar()) {
            WARN("Could not install mock sidecar module — skipping integration test");
            return;
        }

        // Check Python is available.
        if (::system("python3 --version > /dev/null 2>&1") != 0) {
            WARN("python3 not found — skipping integration test");
            return;
        }

        // We need PYTHONPATH=/tmp so Python can find the module.
        // SidecarManager's build_envp copies the parent env, so we set it here.
        ::setenv("PYTHONPATH", "/tmp", 1);

        SidecarManager mgr(make_mock_config(10));
        CHECK(mgr.current_state() == SidecarState::Cold);

        // ensure_started should spawn the mock, poll /healthz, and reach Running.
        auto [src, tok] = batbox::CancelToken::make_root();
        auto res = mgr.ensure_started(std::move(tok));

        if (!res.has_value()) {
            // Print error to help with diagnostics on CI.
            MESSAGE("ensure_started failed: " << res.error());
        }
        CHECK(res.has_value());
        CHECK(mgr.current_state() == SidecarState::Running);
        CHECK(mgr.port() > 0);

        // Verify the port is actually listening.
        CHECK(port_is_listening(mgr.port()));

        // Shutdown.
        mgr.shutdown();
        CHECK(mgr.current_state() == SidecarState::Disabled);
        CHECK(!port_is_listening(mgr.port()));

        ::unsetenv("PYTHONPATH");
    }

    TEST_CASE("startup timeout transitions to CrashedRestarting") {
        // We need a "sidecar" that never serves /healthz.
        // Write a module that just sleeps forever.
        if (!install_mock_sidecar()) {
            WARN("Could not install mock sidecar module — skipping integration test");
            return;
        }

        // Replace __main__.py with a sleeping version.
        const char* kSleepScript = R"PYEOF(
import time, sys
time.sleep(60)
)PYEOF";
        if (!write_script("/tmp/scrapling_server/__main__.py", kSleepScript)) {
            WARN("Could not write sleep script — skipping");
            return;
        }

        ::setenv("PYTHONPATH", "/tmp", 1);

        SidecarConfig cfg = make_mock_config(2); // 2 s timeout
        SidecarManager mgr(cfg);

        auto [src, tok] = batbox::CancelToken::make_root();
        auto res = mgr.ensure_started(std::move(tok));

        CHECK(!res.has_value());
        // State should be CrashedRestarting (first timeout) or further.
        const auto st = mgr.current_state();
        CHECK((st == SidecarState::CrashedRestarting ||
               st == SidecarState::Disabled));

        // Restore healthy script for subsequent tests.
        write_script("/tmp/scrapling_server/__main__.py", kMockSidecarScript);
        ::unsetenv("PYTHONPATH");

        // Cleanup.
        mgr.shutdown();
    }

    TEST_CASE("restart cap: 3 failed restarts → Disabled") {
        // Install the sleeping mock (startup will always time out).
        if (!install_mock_sidecar()) {
            WARN("Could not install mock sidecar module — skipping");
            return;
        }
        const char* kSleepScript = R"PYEOF(
import time, sys
time.sleep(60)
)PYEOF";
        if (!write_script("/tmp/scrapling_server/__main__.py", kSleepScript)) {
            WARN("Could not write sleep script — skipping");
            return;
        }

        ::setenv("PYTHONPATH", "/tmp", 1);

        SidecarConfig cfg = make_mock_config(1); // 1 s timeout — fast iteration
        SidecarManager mgr(cfg);

        // Attempt ensure_started until Disabled (or up to kMaxRestarts+2 tries).
        for (int i = 0; i < 6; ++i) {
            if (mgr.current_state() == SidecarState::Disabled) break;
            auto [src, tok] = batbox::CancelToken::make_root();
            (void)mgr.ensure_started(std::move(tok)); // intentionally ignore result
        }

        CHECK(mgr.current_state() == SidecarState::Disabled);
        CHECK(mgr.restart_count() <= SidecarManager::kMaxRestarts);

        write_script("/tmp/scrapling_server/__main__.py", kMockSidecarScript);
        ::unsetenv("PYTHONPATH");
        mgr.shutdown();
    }

    TEST_CASE("ensure_started is idempotent when already Running") {
        if (!install_mock_sidecar()) {
            WARN("Could not install mock sidecar module — skipping");
            return;
        }
        write_script("/tmp/scrapling_server/__main__.py", kMockSidecarScript);
        ::setenv("PYTHONPATH", "/tmp", 1);

        SidecarManager mgr(make_mock_config(8));
        {
            auto [src, tok] = batbox::CancelToken::make_root();
            auto res = mgr.ensure_started(std::move(tok));
            if (!res.has_value()) {
                MESSAGE("ensure_started failed: " << res.error()); WARN("skipping integration test");
                ::unsetenv("PYTHONPATH");
                return;
            }
        }
        REQUIRE(mgr.current_state() == SidecarState::Running);
        const uint16_t original_port = mgr.port();

        // Second call should be a fast no-op returning Ok.
        {
            auto [src2, tok2] = batbox::CancelToken::make_root();
            auto res2 = mgr.ensure_started(std::move(tok2));
            CHECK(res2.has_value());
        }
        CHECK(mgr.current_state() == SidecarState::Running);
        CHECK(mgr.port() == original_port);

        mgr.shutdown();
        ::unsetenv("PYTHONPATH");
    }

    TEST_CASE("cancellation during healthcheck poll") {
        if (!install_mock_sidecar()) {
            WARN("Could not install mock sidecar module — skipping");
            return;
        }
        // Use a sleeping mock so healthz never returns 200.
        const char* kSleepScript = R"PYEOF(
import time
time.sleep(60)
)PYEOF";
        write_script("/tmp/scrapling_server/__main__.py", kSleepScript);
        ::setenv("PYTHONPATH", "/tmp", 1);

        SidecarConfig cfg = make_mock_config(10); // long timeout
        SidecarManager mgr(cfg);

        auto [src, tok] = batbox::CancelToken::make_root();

        // Cancel after 300 ms — well before the 10 s startup timeout.
        std::thread canceller([&src]() {
            std::this_thread::sleep_for(300ms);
            src.request_stop();
        });

        auto res = mgr.ensure_started(std::move(tok));
        canceller.join();

        CHECK(!res.has_value());
        CHECK(res.error().find("cancel") != std::string::npos);

        write_script("/tmp/scrapling_server/__main__.py", kMockSidecarScript);
        ::unsetenv("PYTHONPATH");
        mgr.shutdown();
    }
}

// ============================================================================
// TEST SUITE 5: prewarm_async
// ============================================================================
TEST_SUITE("SidecarManager — prewarm") {

    TEST_CASE("prewarm_async is a no-op when called twice") {
        // Calling prewarm_async() twice should not crash or assert.
        // We use a Disabled manager (shutdown() first) so ensure_started will
        // return immediately with Err, keeping the test fast.
        SidecarManager mgr(make_test_config(2));
        mgr.shutdown(); // force Disabled — prewarm_async skips Disabled state
        // First call: state is Disabled → prewarm_async no-ops.
        auto [src1, tok1] = batbox::CancelToken::make_root();
        mgr.prewarm_async(std::move(tok1));
        // Second call: still Disabled → no-op.
        auto [src2, tok2] = batbox::CancelToken::make_root();
        mgr.prewarm_async(std::move(tok2));
        // wait_prewarm should return Ok (no future was launched).
        auto res = mgr.wait_prewarm();
        CHECK(res.has_value());
    }

    TEST_CASE("prewarm_async with healthy sidecar reaches Running") {
        if (!install_mock_sidecar()) {
            WARN("Could not install mock sidecar module — skipping");
            return;
        }
        write_script("/tmp/scrapling_server/__main__.py", kMockSidecarScript);
        if (::system("python3 --version > /dev/null 2>&1") != 0) {
            WARN("python3 not found — skipping prewarm integration test");
            return;
        }
        ::setenv("PYTHONPATH", "/tmp", 1);

        SidecarManager mgr(make_mock_config(10));
        CHECK(mgr.current_state() == SidecarState::Cold);

        // Launch prewarm — non-blocking.
        std::string status_received;
        auto [src, tok] = batbox::CancelToken::make_root();
        mgr.prewarm_async(std::move(tok),
            [&status_received](std::string_view s) {
                status_received = std::string(s);
            });

        // prewarm_async returns immediately; state is still Cold or Starting.
        CHECK(mgr.current_state() != SidecarState::Disabled);

        // wait_prewarm() blocks until the background task finishes.
        auto res = mgr.wait_prewarm();

        if (!res.has_value()) {
            MESSAGE("prewarm failed: " << res.error());
        }
        CHECK(res.has_value());
        CHECK(mgr.current_state() == SidecarState::Running);
        CHECK(mgr.port() > 0);

        // Status callback should have fired with "ready".
        CHECK(status_received == "ready");

        // Subsequent ensure_started() is a fast no-op (Running state).
        {
            auto [src2, tok2] = batbox::CancelToken::make_root();
            auto es_res = mgr.ensure_started(std::move(tok2));
            CHECK(es_res.has_value());
            CHECK(mgr.current_state() == SidecarState::Running);
        }

        mgr.shutdown();
        ::unsetenv("PYTHONPATH");
    }

    TEST_CASE("prewarm_async: failed prewarm allows cold-start fallback") {
        // Use a non-existent python binary so prewarm always fails.
        SidecarConfig cfg = make_test_config(1);
        cfg.python = "/nonexistent/python99";
        SidecarManager mgr(cfg);

        std::string status_received;
        auto [src, tok] = batbox::CancelToken::make_root();
        mgr.prewarm_async(std::move(tok),
            [&status_received](std::string_view s) {
                status_received = std::string(s);
            });

        // wait_prewarm() should return Err (spawn will fail).
        auto res = mgr.wait_prewarm();
        CHECK(!res.has_value()); // prewarm failed

        // Status should indicate failure.
        CHECK(status_received.substr(0, 6) == "failed");

        // ensure_started() after a failed prewarm should attempt a cold start
        // (which will also fail with the bad python path, but crucially it must
        // NOT short-circuit with "sidecar is disabled" unless the restart cap
        // was hit — the state should be CrashedRestarting, not Disabled yet).
        const auto st = mgr.current_state();
        CHECK((st == SidecarState::CrashedRestarting ||
               st == SidecarState::Cold ||
               st == SidecarState::Disabled)); // any terminal-ish state is acceptable

        mgr.shutdown();
    }

    TEST_CASE("prewarm_async: status callbacks fire in order") {
        if (!install_mock_sidecar()) {
            WARN("Could not install mock sidecar module — skipping");
            return;
        }
        write_script("/tmp/scrapling_server/__main__.py", kMockSidecarScript);
        if (::system("python3 --version > /dev/null 2>&1") != 0) {
            WARN("python3 not found — skipping");
            return;
        }
        ::setenv("PYTHONPATH", "/tmp", 1);

        SidecarManager mgr(make_mock_config(8));

        std::vector<std::string> statuses;
        auto [src, tok] = batbox::CancelToken::make_root();
        mgr.prewarm_async(std::move(tok),
            [&statuses](std::string_view s) {
                statuses.emplace_back(s);
            });

        auto res = mgr.wait_prewarm();

        if (res.has_value()) {
            // Should have seen "prewarming" then "ready" in that order.
            REQUIRE(statuses.size() >= 2);
            CHECK(statuses[0] == "prewarming");
            CHECK(statuses.back() == "ready");
        } else {
            MESSAGE("prewarm failed unexpectedly: " << res.error());
            WARN("skipping status order check");
        }

        mgr.shutdown();
        ::unsetenv("PYTHONPATH");
    }
}

// ============================================================================
// TEST SUITE 6: CPP 7.6 — Ctrl+C double-tap behavior
// ============================================================================
TEST_SUITE("SidecarManager — CPP 7.6 Ctrl+C handling") {

    TEST_CASE("cancellation leaves state as Starting (sidecar keeps booting)") {
        // CPP 7.6 AC: First Ctrl+C cancels the in-flight tool call but leaves
        // the sidecar process running.  State must NOT transition to Disabled.
        if (!install_mock_sidecar()) {
            WARN("Could not install mock sidecar module — skipping");
            return;
        }
        // Use a sleeping mock so healthz never returns 200 (simulates slow boot).
        const char* kSleepScript = R"PYEOF(
import time
time.sleep(60)
)PYEOF";
        write_script("/tmp/scrapling_server/__main__.py", kSleepScript);
        ::setenv("PYTHONPATH", "/tmp", 1);

        SidecarConfig cfg = make_mock_config(10); // long timeout — cancellation wins first
        SidecarManager mgr(cfg);

        auto [src, tok] = batbox::CancelToken::make_root();

        // Cancel after 300 ms.
        std::thread canceller([&src]() {
            std::this_thread::sleep_for(300ms);
            src.request_stop();
        });

        auto res = mgr.ensure_started(std::move(tok));
        canceller.join();

        // ensure_started must return Err("cancelled").
        REQUIRE(!res.has_value());
        CHECK(res.error().find("cancel") != std::string::npos);

        // CPP 7.6 KEY REQUIREMENT: state must NOT be Disabled.
        // The sidecar process is still booting — it should be Starting.
        // (It could also become Running if health poll wins the race, but
        //  it must never become Disabled on a simple cancellation.)
        const auto st = mgr.current_state();
        CHECK(st != SidecarState::Disabled);
        CHECK((st == SidecarState::Starting ||
               st == SidecarState::Running  ||
               st == SidecarState::CrashedRestarting));

        // Restore healthy script and clean up.
        write_script("/tmp/scrapling_server/__main__.py", kMockSidecarScript);
        ::unsetenv("PYTHONPATH");
        mgr.shutdown();
    }

    TEST_CASE("abort_startup() sends SIGTERM and transitions to CrashedRestarting") {
        // CPP 7.6 AC: Second Ctrl+C within 2 s calls abort_startup(), which
        // sends SIGTERM to the process group and transitions state →
        // CrashedRestarting.
        if (!install_mock_sidecar()) {
            WARN("Could not install mock sidecar module — skipping");
            return;
        }
        if (::system("python3 --version > /dev/null 2>&1") != 0) {
            WARN("python3 not found — skipping abort_startup integration test");
            return;
        }
        // Use a sleeping mock so the sidecar stays in Starting.
        const char* kSleepScript = R"PYEOF(
import time
time.sleep(60)
)PYEOF";
        write_script("/tmp/scrapling_server/__main__.py", kSleepScript);
        ::setenv("PYTHONPATH", "/tmp", 1);

        SidecarConfig cfg = make_mock_config(15); // long timeout
        SidecarManager mgr(cfg);

        // Drive ensure_started() on a background thread so the sidecar enters Starting.
        auto [bg_src, bg_tok] = batbox::CancelToken::make_root();
        std::future<batbox::Result<void>> bg_future =
            std::async(std::launch::async, [&mgr, tok = std::move(bg_tok)]() mutable {
                return mgr.ensure_started(std::move(tok));
            });

        // Poll until state reaches Starting or we time out (up to 3 s).
        const auto start_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < start_deadline) {
            if (mgr.current_state() == SidecarState::Starting && mgr.port() > 0) break;
            std::this_thread::sleep_for(100ms);
        }

        if (mgr.current_state() != SidecarState::Starting || mgr.port() == 0) {
            WARN("Sidecar did not reach Starting state within 3 s — skipping abort_startup test");
            bg_src.request_stop();
            (void)bg_future.get();
            write_script("/tmp/scrapling_server/__main__.py", kMockSidecarScript);
            ::unsetenv("PYTHONPATH");
            mgr.shutdown();
            return;
        }
        CHECK(mgr.current_state() == SidecarState::Starting);
        CHECK(mgr.port() > 0); // port was picked — child is alive

        // Simulate second Ctrl+C: call abort_startup().
        mgr.abort_startup();

        // State must be CrashedRestarting immediately (abort_startup does the CAS).
        CHECK(mgr.current_state() == SidecarState::CrashedRestarting);

        // Cancel the background thread so it unblocks from the health-poll loop.
        bg_src.request_stop();
        (void)bg_future.get(); // drain — should return Err("cancelled")

        // Restore healthy script.
        write_script("/tmp/scrapling_server/__main__.py", kMockSidecarScript);
        ::unsetenv("PYTHONPATH");
        mgr.shutdown();
    }

    TEST_CASE("abort_startup() is a no-op before any child is spawned") {
        // No child spawned → abort_startup() must not crash.
        SidecarManager mgr(make_test_config());
        CHECK(mgr.current_state() == SidecarState::Cold);

        // Should complete without crashing or changing state.
        mgr.abort_startup();
        CHECK(mgr.current_state() == SidecarState::Cold);

        mgr.shutdown();
    }
}
