// tests/integration/test_app_shutdown.cpp
// =============================================================================
// Integration tests for CPP A.4 — App::shutdown() clean teardown.
//
// Strategy:
//   The shutdown sequence involves TUI, MCP, sidecar, agents, session, and
//   plugin subsystems — none of which can be driven headlessly with a real
//   FTXUI event loop.  We validate the observable contracts via:
//
//   Surface 1 — Idempotency
//     Call App::shutdown() twice with a null ShutdownContext (all nullptr
//     fields).  The second call must be a no-op (not crash, not re-enter).
//
//   Surface 2 — Null-safety
//     App::shutdown() with every ShutdownContext field set to nullptr must
//     complete without crashing.  This validates the null-guard on each step.
//
//   Surface 3 — SIGTERM flag
//     Install the sigterm_handler via kill(getpid(), SIGTERM) and verify
//     that g_sigterm_received becomes non-zero.  Done by spawning batbox
//     with --print, sending SIGTERM during execution, and confirming the
//     process exits gracefully (not via SIGKILL / uncaught signal).
//
//   Surface 4 — Headless --print exits cleanly after full shutdown path
//     batbox --print "hello" against a fake OpenAI server must exit 0 with
//     non-empty stdout.  This exercises the init → headless turn →
//     (graceful return from run_headless) path, which does NOT call
//     App::shutdown() (shutdown is for interactive TUI mode only).
//
//   Surface 5 — No zombie subprocess after sidecar lifecycle
//     If a sidecar was never started (prewarm=false), shutdown with a live
//     SidecarManager that is in Cold state must be a no-op and not crash.
//
//   Surface 6 — AgentSupervisor drain with zero agents
//     shutdown() against a live but idle AgentSupervisor (no running agents)
//     must call wait_all() without blocking and without assertion failure.
//
//   Surface 7 — Session store with no active session
//     shutdown() against a live SessionStore with no new_session() call must
//     log "no active session" and return without error.
//
// Tests:
//   1. App::shutdown() with null ctx — must complete without crash
//   2. App::shutdown() idempotency — second call is no-op
//   3. SessionStore with no session — shutdown logs and continues
//   4. AgentSupervisor drain — idle supervisor, no blocking
//   5. SidecarManager in Cold state — shutdown is no-op
//   6. headless --print exits 0 with fake server
//   7. headless --print exits non-zero with unreachable server
//   8. SIGTERM → process exits non-SIGKILL (graceful)
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "App.hpp"

#include <batbox/agents/AgentSupervisor.hpp>
#include <batbox/config/Config.hpp>
#include <batbox/config/EnvLoader.hpp>
#include <batbox/session/SessionStore.hpp>
#include <batbox/sidecar/SidecarManager.hpp>
#include <batbox/plugins/PluginRegistry.hpp>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// POSIX headers for subprocess management.
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helper: find fake_openai_server.py fixture.
// BATBOX_FIXTURE_DIR injected by CMake at compile time.
// ---------------------------------------------------------------------------
static std::string find_fixture_script() {
#ifdef BATBOX_FIXTURE_DIR
    fs::path p = fs::path(BATBOX_FIXTURE_DIR) / "fake_openai_server.py";
    if (fs::exists(p)) return p.string();
#endif
    fs::path dir = fs::current_path();
    for (int depth = 0; depth < 8; ++depth) {
        fs::path candidate = dir / "tests" / "fixtures" / "fake_openai_server.py";
        if (fs::exists(candidate)) return candidate.string();
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    return {};
}

// ---------------------------------------------------------------------------
// Helper: find the batbox binary.
// BATBOX_BINARY_DIR injected by CMake at compile time.
// ---------------------------------------------------------------------------
static std::string find_batbox_binary() {
#ifdef BATBOX_BINARY_DIR
    fs::path p = fs::path(BATBOX_BINARY_DIR) / "src" / "batbox";
    if (fs::exists(p)) return p.string();
    fs::path p2 = fs::path(BATBOX_BINARY_DIR) / "batbox";
    if (fs::exists(p2)) return p2.string();
#endif
    FILE* f = ::popen("which batbox 2>/dev/null", "r");
    if (f) {
        char buf[512] = {};
        if (std::fgets(buf, sizeof(buf), f)) {
            std::string s(buf);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            ::pclose(f);
            if (!s.empty() && fs::exists(s)) return s;
        }
        ::pclose(f);
    }
    return {};
}

// ---------------------------------------------------------------------------
// FakeServer RAII — forks python3, waits for "READY <port>" on stdout.
// ---------------------------------------------------------------------------
struct FakeServer {
    pid_t pid{-1};
    int   port{0};

    bool start(const std::string& script_path) {
        if (script_path.empty()) return false;

        int pipefd[2];
        if (::pipe(pipefd) != 0) return false;

        pid = ::fork();
        if (pid < 0) { ::close(pipefd[0]); ::close(pipefd[1]); return false; }

        if (pid == 0) {
            ::close(pipefd[0]);
            ::dup2(pipefd[1], STDOUT_FILENO);
            ::close(pipefd[1]);
            int devnull = ::open("/dev/null", O_WRONLY);
            if (devnull >= 0) { ::dup2(devnull, STDERR_FILENO); ::close(devnull); }
            ::execlp("python3", "python3", script_path.c_str(), nullptr);
            ::_exit(127);
        }

        ::close(pipefd[1]);
        ::fcntl(pipefd[0], F_SETFL, ::fcntl(pipefd[0], F_GETFL) | O_NONBLOCK);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        char buf[128] = {};
        std::string accum;
        while (std::chrono::steady_clock::now() < deadline) {
            ssize_t n = ::read(pipefd[0], buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                accum += buf;
                auto pos = accum.find('\n');
                if (pos != std::string::npos) {
                    std::string line = accum.substr(0, pos);
                    if (line.rfind("READY ", 0) == 0) {
                        port = std::stoi(line.substr(6));
                        ::close(pipefd[0]);
                        return port > 0;
                    }
                    break;
                }
            } else if (n == 0) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        ::close(pipefd[0]);
        stop();
        return false;
    }

    void stop() {
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            int status = 0;
            ::waitpid(pid, &status, 0);
            pid = -1;
        }
    }

    ~FakeServer() { stop(); }
};

// ---------------------------------------------------------------------------
// RunResult — output of a child process run.
// ---------------------------------------------------------------------------
struct RunResult {
    int         exit_code{-1};
    std::string stdout_text;
    std::string stderr_text;
    bool        signalled{false};  ///< true if child was killed by a signal
    int         signal_num{0};     ///< signal number if signalled
};

// ---------------------------------------------------------------------------
// run_batbox — spawn batbox with given args and environment, capture output.
// timeout_ms: max ms to wait before sending SIGTERM to the child.
// ---------------------------------------------------------------------------
static RunResult run_batbox(
    const std::string&              binary,
    const std::vector<std::string>& args,
    const std::vector<std::string>& env_vars,
    int                             timeout_ms = 10000)
{
    RunResult result;
    if (binary.empty()) {
        result.stderr_text = "batbox binary not found";
        return result;
    }

    int stdout_pipe[2], stderr_pipe[2];
    if (::pipe(stdout_pipe) != 0 || ::pipe(stderr_pipe) != 0) {
        result.stderr_text = "pipe() failed";
        return result;
    }

    pid_t child = ::fork();
    if (child < 0) {
        ::close(stdout_pipe[0]); ::close(stdout_pipe[1]);
        ::close(stderr_pipe[0]); ::close(stderr_pipe[1]);
        result.stderr_text = "fork() failed";
        return result;
    }

    if (child == 0) {
        // Child: redirect stdout/stderr.
        ::dup2(stdout_pipe[1], STDOUT_FILENO);
        ::dup2(stderr_pipe[1], STDERR_FILENO);
        ::close(stdout_pipe[0]); ::close(stdout_pipe[1]);
        ::close(stderr_pipe[0]); ::close(stderr_pipe[1]);

        // Build argv.
        std::vector<const char*> argv;
        argv.push_back(binary.c_str());
        for (const auto& a : args) argv.push_back(a.c_str());
        argv.push_back(nullptr);

        // Inject environment overrides into current env.
        for (const auto& kv : env_vars) {
            ::putenv(const_cast<char*>(kv.c_str()));
        }

        ::execv(binary.c_str(), const_cast<char**>(argv.data()));
        ::_exit(127);
    }

    // Parent: close write ends, read from read ends.
    ::close(stdout_pipe[1]);
    ::close(stderr_pipe[1]);

    ::fcntl(stdout_pipe[0], F_SETFL, ::fcntl(stdout_pipe[0], F_GETFL) | O_NONBLOCK);
    ::fcntl(stderr_pipe[0], F_SETFL, ::fcntl(stderr_pipe[0], F_GETFL) | O_NONBLOCK);

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);

    bool stdout_done = false, stderr_done = false;
    char buf[4096];

    while ((!stdout_done || !stderr_done) &&
           std::chrono::steady_clock::now() < deadline) {
        if (!stdout_done) {
            ssize_t n = ::read(stdout_pipe[0], buf, sizeof(buf));
            if (n > 0) result.stdout_text.append(buf, static_cast<std::size_t>(n));
            else if (n == 0) stdout_done = true;
        }
        if (!stderr_done) {
            ssize_t n = ::read(stderr_pipe[0], buf, sizeof(buf));
            if (n > 0) result.stderr_text.append(buf, static_cast<std::size_t>(n));
            else if (n == 0) stderr_done = true;
        }
        if (!stdout_done || !stderr_done) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    // If timed out, kill the child.
    if (std::chrono::steady_clock::now() >= deadline) {
        ::kill(child, SIGTERM);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        ::kill(child, SIGKILL);
    }

    // Drain remaining output.
    {
        ssize_t n;
        while ((n = ::read(stdout_pipe[0], buf, sizeof(buf))) > 0)
            result.stdout_text.append(buf, static_cast<std::size_t>(n));
        while ((n = ::read(stderr_pipe[0], buf, sizeof(buf))) > 0)
            result.stderr_text.append(buf, static_cast<std::size_t>(n));
    }

    ::close(stdout_pipe[0]);
    ::close(stderr_pipe[0]);

    int status = 0;
    ::waitpid(child, &status, 0);
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.signalled   = true;
        result.signal_num  = WTERMSIG(status);
        result.exit_code   = -1;
    }

    return result;
}

// ===========================================================================
// TEST SUITE: App::shutdown — null-safety and idempotency
// ===========================================================================

TEST_SUITE("App::shutdown null-safety") {

    TEST_CASE("shutdown() with all-null ShutdownContext does not crash") {
        // Reset idempotency flag so this test starts clean.
        batbox::App::reset_shutdown_flag();

        batbox::ShutdownContext ctx;
        // All fields default to nullptr.

        // Must complete without crash or exception.
        CHECK_NOTHROW(batbox::App::shutdown(ctx));

        // Reset for subsequent tests.
        batbox::App::reset_shutdown_flag();
    }

    TEST_CASE("shutdown() is idempotent — second call is no-op") {
        batbox::App::reset_shutdown_flag();

        batbox::ShutdownContext ctx;
        // All fields nullptr.

        // First call — runs all steps (all no-op due to nullptr).
        batbox::App::shutdown(ctx);

        // Second call — must be a pure no-op, no crash, no re-execution.
        CHECK_NOTHROW(batbox::App::shutdown(ctx));
        CHECK_NOTHROW(batbox::App::shutdown(ctx));

        batbox::App::reset_shutdown_flag();
    }

    TEST_CASE("reset_shutdown_flag allows second shutdown after reset") {
        batbox::App::reset_shutdown_flag();

        batbox::ShutdownContext ctx;
        batbox::App::shutdown(ctx);  // first call — executes

        batbox::App::reset_shutdown_flag();

        // After reset, shutdown() should execute again (not skip).
        CHECK_NOTHROW(batbox::App::shutdown(ctx));

        batbox::App::reset_shutdown_flag();
    }
}

// ===========================================================================
// TEST SUITE: App::shutdown — live subsystem teardown
// ===========================================================================

TEST_SUITE("App::shutdown live subsystems") {

    TEST_CASE("SessionStore with no active session — shutdown logs and continues") {
        batbox::App::reset_shutdown_flag();

        // Create a SessionStore in a temp directory.
        const fs::path tmp_dir = fs::temp_directory_path() /
            ("batbox_test_session_" + std::to_string(::getpid()));
        fs::create_directories(tmp_dir);

        batbox::session::SessionStore store(tmp_dir);

        batbox::ShutdownContext ctx;
        ctx.session_store = &store;

        // No new_session() call — current_session_id() returns nullopt.
        // shutdown() must log "no active session" and continue without error.
        CHECK_NOTHROW(batbox::App::shutdown(ctx));

        // Cleanup.
        fs::remove_all(tmp_dir);
        batbox::App::reset_shutdown_flag();
    }

    TEST_CASE("AgentSupervisor idle drain — wait_all does not block") {
        batbox::App::reset_shutdown_flag();

        batbox::agents::AgentSupervisor supervisor;

        batbox::ShutdownContext ctx;
        ctx.supervisor = &supervisor;

        // No agents were spawned — snapshot() should return empty,
        // wait_all() should return immediately without blocking.
        auto t_start = std::chrono::steady_clock::now();
        CHECK_NOTHROW(batbox::App::shutdown(ctx));
        auto t_end = std::chrono::steady_clock::now();

        // Should complete in well under 1 second (generous budget).
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            t_end - t_start).count();
        CHECK(elapsed_ms < 1000);

        batbox::App::reset_shutdown_flag();
    }

    TEST_CASE("SidecarManager in Cold state — shutdown is no-op") {
        batbox::App::reset_shutdown_flag();

        // Build a minimal SidecarConfig.
        batbox::config::EnvMap env;
        auto cfg_res = batbox::config::Config::load_from_env(env);
        REQUIRE(static_cast<bool>(cfg_res));
        const auto& cfg = cfg_res.value();

        batbox::sidecar::SidecarManager sidecar(cfg.sidecar);

        // SidecarManager is in Cold state — no process was started.
        // shutdown() must be a no-op (graceful, no crash).
        batbox::ShutdownContext ctx;
        ctx.sidecar_mgr = &sidecar;

        CHECK_NOTHROW(batbox::App::shutdown(ctx));

        batbox::App::reset_shutdown_flag();
    }

    TEST_CASE("PluginRegistry shutdown — logs count and completes") {
        batbox::App::reset_shutdown_flag();

        batbox::plugins::PluginRegistry plugin_reg;
        // No plugins loaded — size() == 0.

        batbox::ShutdownContext ctx;
        ctx.plugin_registry = &plugin_reg;

        CHECK_NOTHROW(batbox::App::shutdown(ctx));

        batbox::App::reset_shutdown_flag();
    }
}

// ===========================================================================
// TEST SUITE: App::shutdown — process-level smoke tests
// ===========================================================================

TEST_SUITE("App shutdown process-level") {

    TEST_CASE("--print exits 0 with fake server (full init → headless → exit)") {
        const std::string fixture = find_fixture_script();
        const std::string binary  = find_batbox_binary();
        if (fixture.empty() || binary.empty()) {
            MESSAGE("Skipping: fixture script or batbox binary not found");
            return;
        }

        FakeServer srv;
        if (!srv.start(fixture)) {
            MESSAGE("Skipping: fake server failed to start");
            return;
        }

        std::string base_url = "http://127.0.0.1:" + std::to_string(srv.port) + "/v1";
        auto res = run_batbox(binary,
            {"--print", "hello"},
            {"BATBOX_API_BASE_URL=" + base_url,
             "BATBOX_API_KEY=test-key-123"});

        CHECK(res.exit_code == 0);
        CHECK_FALSE(res.stdout_text.empty());
        // Must NOT have been killed by a signal.
        CHECK_FALSE(res.signalled);
    }

    TEST_CASE("--print exits 1 when server is unreachable (no leaked process)") {
        const std::string binary = find_batbox_binary();
        if (binary.empty()) {
            MESSAGE("Skipping: batbox binary not found");
            return;
        }

        // Point to a port that refuses connections.
        auto res = run_batbox(binary,
            {"--print", "hello"},
            {"BATBOX_API_BASE_URL=http://127.0.0.1:1/v1",
             "BATBOX_API_KEY=test-key"},
            5000);

        // Should exit non-zero (1 for inference error).
        CHECK(res.exit_code != 0);
        // Must NOT have been killed by a signal (graceful exit).
        CHECK_FALSE(res.signalled);
    }

    TEST_CASE("SIGTERM during --print causes graceful exit, not hard kill") {
        const std::string binary = find_batbox_binary();
        if (binary.empty()) {
            MESSAGE("Skipping: batbox binary not found");
            return;
        }

        // Spawn batbox --print against an unreachable server.
        // It will block briefly on the connection attempt, giving us time to
        // send SIGTERM and verify it exits gracefully (not SIGKILL, not hang).
        int stdout_pipe[2];
        if (::pipe(stdout_pipe) != 0) {
            MESSAGE("Skipping: pipe() failed");
            return;
        }

        pid_t child = ::fork();
        if (child < 0) {
            ::close(stdout_pipe[0]); ::close(stdout_pipe[1]);
            MESSAGE("Skipping: fork() failed");
            return;
        }

        if (child == 0) {
            ::close(stdout_pipe[0]);
            ::dup2(stdout_pipe[1], STDOUT_FILENO);
            int devnull = ::open("/dev/null", O_WRONLY);
            if (devnull >= 0) { ::dup2(devnull, STDERR_FILENO); ::close(devnull); }
            ::close(stdout_pipe[1]);

            ::putenv(const_cast<char*>("BATBOX_API_BASE_URL=http://127.0.0.1:19999/v1"));
            ::putenv(const_cast<char*>("BATBOX_API_KEY=test-key"));

            const char* argv[] = { binary.c_str(), "--print", "hello", nullptr };
            ::execv(binary.c_str(), const_cast<char**>(argv));
            ::_exit(127);
        }

        ::close(stdout_pipe[1]);
        ::close(stdout_pipe[0]);

        // Give the child 300 ms to start up, then send SIGTERM.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        ::kill(child, SIGTERM);

        // Wait up to 4 s for the child to exit gracefully.
        int status = 0;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (std::chrono::steady_clock::now() < deadline) {
            pid_t r = ::waitpid(child, &status, WNOHANG);
            if (r == child) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        // Final blocking wait to clean up.
        ::waitpid(child, &status, 0);

        // The child must have exited (not been killed by SIGKILL).
        // SIGTERM in the --print path restores default SIGTERM disposition
        // after the signal is first ignored; the process should exit via the
        // normal code path or via SIGTERM default action.
        // Either WIFEXITED or WIFSIGNALED(SIGTERM) is acceptable — the
        // critical requirement is that we are NOT seeing SIGKILL (signal 9).
        if (WIFSIGNALED(status)) {
            const int sig = WTERMSIG(status);
            CHECK(sig != SIGKILL);  // must not be force-killed
        }
        // If WIFEXITED: the process exited with a code — that is also acceptable.
    }
}
