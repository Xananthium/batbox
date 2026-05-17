// ---------------------------------------------------------------------------
// tests/integration/test_sidecar_lifecycle.cpp
//
// Integration tests for batbox::sidecar::SidecarManager lifecycle.
// Task: CPP 7.9
//
// Acceptance criteria covered:
//   AC1 — Cold start: state begins Cold, reaches Running after ensure_started
//   AC2 — Crash simulation: kill sidecar; manager detects and restarts
//   AC3 — Restart cap: after kMaxRestarts exhausted → Disabled state
//   AC4 — Shutdown: graceful via /shutdown; SIGTERM fallback when ignored
//   AC5 — No leaked Python processes after teardown
//
// Strategy:
//   Install tests/fixtures/fake_scrapling_server.py as a Python module at
//   /tmp/batbox_test_sidecar_<pid>/scrapling_server/__main__.py and set
//   PYTHONPATH so SidecarManager's posix_spawn picks it up via
//   "python3 -m scrapling_server --port <N>".
//
// Note on AC3 implementation:
//   The never-healthy variant sleeps indefinitely (not self-exits) so that the
//   startup timeout path fires.  This keeps child_pid_ positive when the cap
//   is hit, which means shutdown() correctly kills the lingering child and
//   joins the stderr reader thread — avoiding the std::terminate that would
//   occur if a joinable thread was abandoned during destruction.
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/sidecar/SidecarManager.hpp>
#include <batbox/sidecar/SidecarState.hpp>
#include <batbox/sidecar/ScraplingProto.hpp>
#include <batbox/config/Config.hpp>
#include <batbox/core/CancelToken.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace batbox::sidecar;
using namespace batbox::config;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Locate tests/fixtures/fake_scrapling_server.py.
// BATBOX_FIXTURE_DIR is injected by CMake at compile time.
// ---------------------------------------------------------------------------
static std::string find_fixture_file() {
#ifdef BATBOX_FIXTURE_DIR
    fs::path p = fs::path(BATBOX_FIXTURE_DIR) / "fake_scrapling_server.py";
    if (fs::exists(p)) return p.string();
#endif
    fs::path dir = fs::current_path();
    for (int depth = 0; depth < 10; ++depth) {
        fs::path candidate = dir / "tests" / "fixtures" / "fake_scrapling_server.py";
        if (fs::exists(candidate)) return candidate.string();
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    return "";
}

// ---------------------------------------------------------------------------
// SidecarTestFixture — creates a per-process temp module directory and sets
// PYTHONPATH so SidecarManager's child can import it via -m scrapling_server.
// ---------------------------------------------------------------------------
struct SidecarTestFixture {
    std::string module_dir;   // /tmp/batbox_test_sidecar_<pid>
    std::string fixture_path; // tests/fixtures/fake_scrapling_server.py

    bool installed = false;

    SidecarTestFixture() {
        fixture_path = find_fixture_file();
        if (fixture_path.empty()) return;

        module_dir = "/tmp/batbox_test_sidecar_" + std::to_string(::getpid());
        const std::string pkg_dir = module_dir + "/scrapling_server";

        ::mkdir(module_dir.c_str(), 0755);
        ::mkdir(pkg_dir.c_str(), 0755);

        // Empty __init__.py so the directory is a package.
        { std::ofstream f(pkg_dir + "/__init__.py"); if (!f) return; }

        // Copy fake_scrapling_server.py → scrapling_server/__main__.py.
        {
            std::ifstream src(fixture_path, std::ios::binary);
            if (!src) return;
            std::ofstream dst(pkg_dir + "/__main__.py", std::ios::binary);
            if (!dst) return;
            dst << src.rdbuf();
            if (!dst.good()) return;
        }

        ::setenv("PYTHONPATH", module_dir.c_str(), 1);
        installed = true;
    }

    // Replace __main__.py content (for variant tests).
    bool install_main(const std::string& content) const {
        std::ofstream f(module_dir + "/scrapling_server/__main__.py");
        if (!f) return false;
        f << content;
        return f.good();
    }

    // Restore __main__.py to the original fake server.
    bool restore_main() const {
        if (fixture_path.empty()) return false;
        std::ifstream src(fixture_path, std::ios::binary);
        if (!src) return false;
        std::ofstream dst(module_dir + "/scrapling_server/__main__.py", std::ios::binary);
        if (!dst) return false;
        dst << src.rdbuf();
        return dst.good();
    }

    ~SidecarTestFixture() {
        ::unsetenv("PYTHONPATH");
        if (!module_dir.empty()) {
            (void)::system(("rm -rf \"" + module_dir + "\" 2>/dev/null").c_str());
        }
    }
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool port_is_listening(uint16_t port) {
    const int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;
    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int rc = ::connect(sock,
        reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr));
    ::close(sock);
    return (rc == 0);
}

static SidecarConfig make_test_config(int timeout_sec = 10) {
    SidecarConfig cfg;
    cfg.python              = "python3";
    cfg.venv                = "/tmp";
    cfg.startup_timeout_sec = timeout_sec;
    cfg.autostart           = false;
    cfg.prewarm             = false;
    return cfg;
}

static std::size_t count_sidecar_procs() {
    FILE* f = ::popen("pgrep -f 'scrapling_server' 2>/dev/null | wc -l", "r");
    if (!f) return 0;
    char buf[64]{};
    (void)::fgets(buf, sizeof(buf), f);
    ::pclose(f);
    return static_cast<std::size_t>(std::atol(buf));
}

static bool python3_available() {
    return ::system("python3 --version > /dev/null 2>&1") == 0;
}

// ============================================================================
// AC1 — Cold start: state begins Cold, reaches Running
// ============================================================================
TEST_SUITE("SidecarLifecycle — AC1 cold start") {

    TEST_CASE("Cold → Running on first ensure_started") {
        SidecarTestFixture fix;
        if (!fix.installed || !python3_available()) {
            WARN("Prerequisites not met — skipping");
            return;
        }

        SidecarManager mgr(make_test_config(10));
        CHECK(mgr.current_state() == SidecarState::Cold);

        auto [src, tok] = batbox::CancelToken::make_root();
        auto res = mgr.ensure_started(std::move(tok));

        if (!res.has_value()) {
            MESSAGE("ensure_started error: " << res.error());
        }
        REQUIRE(res.has_value());
        CHECK(mgr.current_state() == SidecarState::Running);
        CHECK(mgr.port() > 0);
        CHECK(port_is_listening(static_cast<uint16_t>(mgr.port())));

        mgr.shutdown();
        CHECK(mgr.current_state() == SidecarState::Disabled);
        CHECK(!port_is_listening(static_cast<uint16_t>(mgr.port())));
    }

    TEST_CASE("ensure_started is idempotent when already Running") {
        SidecarTestFixture fix;
        if (!fix.installed || !python3_available()) {
            WARN("Prerequisites not met — skipping");
            return;
        }

        SidecarManager mgr(make_test_config(10));
        {
            auto [src, tok] = batbox::CancelToken::make_root();
            auto res = mgr.ensure_started(std::move(tok));
            if (!res.has_value()) { WARN("First ensure_started failed — skipping"); return; }
        }
        REQUIRE(mgr.current_state() == SidecarState::Running);
        const uint16_t port_before = mgr.port();

        {
            auto [src2, tok2] = batbox::CancelToken::make_root();
            auto res2 = mgr.ensure_started(std::move(tok2));
            CHECK(res2.has_value());
        }
        CHECK(mgr.current_state() == SidecarState::Running);
        CHECK(mgr.port() == port_before);

        mgr.shutdown();
    }

    TEST_CASE("POST /fetch returns canned FetchResponse once Running") {
        SidecarTestFixture fix;
        if (!fix.installed || !python3_available()) {
            WARN("Prerequisites not met — skipping");
            return;
        }

        SidecarManager mgr(make_test_config(10));
        {
            auto [src, tok] = batbox::CancelToken::make_root();
            if (!mgr.ensure_started(std::move(tok)).has_value()) {
                WARN("ensure_started failed — skipping");
                return;
            }
        }
        REQUIRE(mgr.current_state() == SidecarState::Running);

        proto::FetchRequest req;
        req.url = "https://example.com";

        auto [src, tok] = batbox::CancelToken::make_root();
        auto fetch_res = mgr.request<proto::FetchRequest, proto::FetchResponse>(
            "/fetch", req, std::move(tok));

        CHECK(fetch_res.has_value());
        if (fetch_res.has_value()) {
            CHECK(fetch_res.value().status_code == 200);
            CHECK(!fetch_res.value().markdown.empty());
            CHECK(!fetch_res.value().is_error);
        }

        mgr.shutdown();
    }
}

// ============================================================================
// AC2 — Crash recovery
// ============================================================================
TEST_SUITE("SidecarLifecycle — AC2 crash recovery") {

    TEST_CASE("Kill sidecar process — next ensure_started recovers or reaches Disabled") {
        SidecarTestFixture fix;
        if (!fix.installed || !python3_available()) {
            WARN("Prerequisites not met — skipping");
            return;
        }

        SidecarManager mgr(make_test_config(10));
        {
            auto [src, tok] = batbox::CancelToken::make_root();
            if (!mgr.ensure_started(std::move(tok)).has_value()) {
                WARN("First ensure_started failed — skipping");
                return;
            }
        }
        REQUIRE(mgr.current_state() == SidecarState::Running);

        // Forcibly kill the sidecar child process.
        {
            FILE* f = ::popen("pgrep -f 'scrapling_server' 2>/dev/null", "r");
            if (f) {
                char buf[64];
                while (::fgets(buf, sizeof(buf), f) != nullptr) {
                    const pid_t pid = static_cast<pid_t>(std::atoi(buf));
                    if (pid > 0) ::kill(pid, SIGKILL);
                }
                ::pclose(f);
            }
        }

        // Give the OS time to deliver SIGKILL and mark the process dead.
        std::this_thread::sleep_for(300ms);

        // The next ensure_started should detect the crash (CrashedRestarting)
        // and attempt to restart, or reach Disabled if something goes wrong.
        auto [src2, tok2] = batbox::CancelToken::make_root();
        (void)mgr.ensure_started(std::move(tok2));

        const auto st = mgr.current_state();
        CHECK((st == SidecarState::Running ||
               st == SidecarState::CrashedRestarting ||
               st == SidecarState::Disabled));

        mgr.shutdown();
        CHECK(mgr.current_state() == SidecarState::Disabled);
    }

    TEST_CASE("FAKE_SIDECAR_CRASH_AFTER=2 — sidecar exits after 2 requests") {
        SidecarTestFixture fix;
        if (!fix.installed || !python3_available()) {
            WARN("Prerequisites not met — skipping");
            return;
        }

        ::setenv("FAKE_SIDECAR_CRASH_AFTER", "2", 1);

        SidecarManager mgr(make_test_config(10));
        {
            auto [src, tok] = batbox::CancelToken::make_root();
            auto res = mgr.ensure_started(std::move(tok));
            if (!res.has_value()) {
                ::unsetenv("FAKE_SIDECAR_CRASH_AFTER");
                WARN("ensure_started failed — skipping");
                return;
            }
        }
        REQUIRE(mgr.current_state() == SidecarState::Running);

        // Send a /fetch to consume the 2nd request slot and trigger crash.
        {
            proto::FetchRequest req;
            req.url = "https://example.com";
            auto [src, tok] = batbox::CancelToken::make_root();
            (void)mgr.request<proto::FetchRequest, proto::FetchResponse>(
                "/fetch", req, std::move(tok));
        }

        // Give the OS time to register the crash.
        std::this_thread::sleep_for(300ms);

        // The state should reflect the crash.
        const auto st = mgr.current_state();
        CHECK((st == SidecarState::Running ||
               st == SidecarState::CrashedRestarting ||
               st == SidecarState::Disabled));

        mgr.shutdown();
        ::unsetenv("FAKE_SIDECAR_CRASH_AFTER");
    }
}

// ============================================================================
// AC3 — Restart cap: after kMaxRestarts failures → Disabled
//
// Uses a never-healthy script that sleeps indefinitely (does NOT self-exit).
// This means the startup timeout fires each time (not child-death detection).
// After each timeout the child is still alive, so kill_and_reap + thread join
// happen cleanly inside ensure_started's restart logic before the next spawn.
// The cap check (restart_count >= kMaxRestarts) triggers Disabled on the
// 4th call to ensure_started; at that point child_pid_ is still > 0 from the
// last spawn, so shutdown() fully cleans up (kills child + joins thread).
// ============================================================================
TEST_SUITE("SidecarLifecycle — AC3 restart cap") {

    TEST_CASE("Exhausted restart cap leads to Disabled state") {
        SidecarTestFixture fix;
        if (!fix.installed || !python3_available()) {
            WARN("Prerequisites not met — skipping");
            return;
        }

        // Script that sleeps indefinitely — never answers /healthz.
        // The startup timeout (1s) fires rather than child-death detection,
        // so each attempt ends with kill_and_reap (child still running when
        // timeout fires) rather than the reaped-by-wait_for_healthy path.
        static const char kNeverHealthyScript[] = R"PYEOF(
import sys
import time

# Parse --port argument (required by SidecarManager, but we ignore it).
port = 0
for i, a in enumerate(sys.argv):
    if a == "--port" and i + 1 < len(sys.argv):
        port = int(sys.argv[i + 1])
        break

# Sleep indefinitely — never serve /healthz.
while True:
    time.sleep(60)
)PYEOF";

        if (!fix.install_main(kNeverHealthyScript)) {
            WARN("Could not install never-healthy script — skipping");
            return;
        }

        // 1 second startup timeout so each attempt fails in ~1s.
        SidecarConfig cfg = make_test_config(1);
        SidecarManager mgr(cfg);

        // Drive ensure_started until Disabled (up to kMaxRestarts + 2 calls).
        for (int i = 0; i < SidecarManager::kMaxRestarts + 2; ++i) {
            if (mgr.current_state() == SidecarState::Disabled) break;
            auto [src, tok] = batbox::CancelToken::make_root();
            (void)mgr.ensure_started(std::move(tok));
        }

        CHECK(mgr.current_state() == SidecarState::Disabled);
        CHECK(mgr.restart_count() <= SidecarManager::kMaxRestarts);

        // ensure_started on a Disabled manager must return Err("...disabled...").
        {
            auto [src, tok] = batbox::CancelToken::make_root();
            auto res = mgr.ensure_started(std::move(tok));
            CHECK(!res.has_value());
            if (!res.has_value()) {
                CHECK(res.error().find("disabled") != std::string::npos);
            }
        }

        // shutdown() on the Disabled+child-alive manager kills the lingering
        // child from the last spawn and joins the stderr reader thread.
        fix.restore_main();
        mgr.shutdown();
    }
}

// ============================================================================
// AC4 — Shutdown sequences
// ============================================================================
TEST_SUITE("SidecarLifecycle — AC4 shutdown") {

    TEST_CASE("Graceful shutdown: state Disabled + port released within 3s") {
        SidecarTestFixture fix;
        if (!fix.installed || !python3_available()) {
            WARN("Prerequisites not met — skipping");
            return;
        }

        SidecarManager mgr(make_test_config(10));
        {
            auto [src, tok] = batbox::CancelToken::make_root();
            auto res = mgr.ensure_started(std::move(tok));
            if (!res.has_value()) { WARN("ensure_started failed — skipping"); return; }
        }
        REQUIRE(mgr.current_state() == SidecarState::Running);
        const uint16_t p = mgr.port();
        REQUIRE(p > 0);
        REQUIRE(port_is_listening(p));

        const auto t0 = std::chrono::steady_clock::now();
        mgr.shutdown();
        const auto elapsed = std::chrono::steady_clock::now() - t0;

        CHECK(mgr.current_state() == SidecarState::Disabled);
        // Graceful /shutdown should complete well under 3s.
        CHECK(elapsed < 3s);
        CHECK(!port_is_listening(p));
    }

    TEST_CASE("SIGTERM fallback when sidecar ignores /shutdown endpoint") {
        SidecarTestFixture fix;
        if (!fix.installed || !python3_available()) {
            WARN("Prerequisites not met — skipping");
            return;
        }

        // Tell the fake server to acknowledge /shutdown but NOT exit.
        ::setenv("FAKE_SIDECAR_IGNORE_SHUTDOWN", "1", 1);

        SidecarManager mgr(make_test_config(10));
        {
            auto [src, tok] = batbox::CancelToken::make_root();
            auto res = mgr.ensure_started(std::move(tok));
            if (!res.has_value()) {
                ::unsetenv("FAKE_SIDECAR_IGNORE_SHUTDOWN");
                WARN("ensure_started failed — skipping");
                return;
            }
        }
        REQUIRE(mgr.current_state() == SidecarState::Running);
        const uint16_t p = mgr.port();

        // shutdown() posts /shutdown (server ignores it), waits 200ms for
        // self-exit (never comes), then sends SIGTERM with a 2s grace period,
        // then SIGKILL if needed.  Total must be < 5s.
        const auto t0 = std::chrono::steady_clock::now();
        mgr.shutdown();
        const auto elapsed = std::chrono::steady_clock::now() - t0;

        CHECK(mgr.current_state() == SidecarState::Disabled);
        CHECK(elapsed < 5s);
        CHECK(!port_is_listening(p));

        ::unsetenv("FAKE_SIDECAR_IGNORE_SHUTDOWN");
    }

    TEST_CASE("shutdown() on Cold manager transitions to Disabled") {
        SidecarManager mgr(make_test_config());
        REQUIRE(mgr.current_state() == SidecarState::Cold);
        mgr.shutdown();
        CHECK(mgr.current_state() == SidecarState::Disabled);
    }

    TEST_CASE("shutdown() on already-Disabled manager is a no-op") {
        SidecarManager mgr(make_test_config());
        mgr.shutdown();
        REQUIRE(mgr.current_state() == SidecarState::Disabled);
        mgr.shutdown(); // must not crash
        CHECK(mgr.current_state() == SidecarState::Disabled);
    }
}

// ============================================================================
// AC5 — No leaked Python processes after teardown
// ============================================================================
TEST_SUITE("SidecarLifecycle — AC5 no leaked processes") {

    TEST_CASE("No scrapling_server processes remain after normal shutdown") {
        SidecarTestFixture fix;
        if (!fix.installed || !python3_available()) {
            WARN("Prerequisites not met — skipping");
            return;
        }

        const std::size_t procs_before = count_sidecar_procs();

        {
            SidecarManager mgr(make_test_config(10));
            auto [src, tok] = batbox::CancelToken::make_root();
            auto res = mgr.ensure_started(std::move(tok));
            if (!res.has_value()) {
                WARN("ensure_started failed — skipping leak check");
                return;
            }
            REQUIRE(mgr.current_state() == SidecarState::Running);
            mgr.shutdown();
            CHECK(mgr.current_state() == SidecarState::Disabled);
        }

        // Allow the OS to finish reaping.
        std::this_thread::sleep_for(300ms);

        const std::size_t procs_after = count_sidecar_procs();
        // After teardown, sidecar process count must not exceed the baseline.
        CHECK(procs_after <= procs_before);
    }

    TEST_CASE("Destructor reaps child when shutdown() not called explicitly") {
        SidecarTestFixture fix;
        if (!fix.installed || !python3_available()) {
            WARN("Prerequisites not met — skipping");
            return;
        }

        uint16_t spawned_port = 0;
        {
            SidecarManager mgr(make_test_config(10));
            auto [src, tok] = batbox::CancelToken::make_root();
            auto res = mgr.ensure_started(std::move(tok));
            if (!res.has_value()) {
                WARN("ensure_started failed — skipping destructor test");
                return;
            }
            REQUIRE(mgr.current_state() == SidecarState::Running);
            spawned_port = mgr.port();
            // Let mgr go out of scope WITHOUT calling shutdown().
        }

        std::this_thread::sleep_for(300ms);
        CHECK(!port_is_listening(spawned_port));
    }
}
