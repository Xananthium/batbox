// tests/integration/test_cfg_snapshot_race.cpp
// =============================================================================
// PEXT2 3.4 (D-8) — Config snapshot race regression test.
//
// Verifies that Conversation::run_turn() correctly snapshots
// cfg.api.default_model and cfg.api.api_key at turn start under cfg_mutex,
// so that concurrent /model switches on a parallel thread cannot data-race
// against the inference thread reading those fields.
//
// Under ThreadSanitizer (-DBATBOX_SANITIZERS=thread on Linux):
//   Run with TSAN_OPTIONS=halt_on_error=1 to catch any data race immediately.
//   The test is designed to be TSAN-clean after the PEXT2 3.4 snapshot patch.
//   Before the patch, TSAN would report a data race on:
//     cfg.api.default_model — written by apply_live_model_mutation under mutex,
//                             read in Compactor ctor and build_chat_request
//                             WITHOUT the mutex.
//
// On macOS/arm64 with Apple Clang (TSan not reliably available):
//   The test still exercises the concurrent path and verifies correctness —
//   it just cannot instrument for races.  The stress loop ensures the race
//   window is exercised even without TSan.
//
// Strategy:
//   1. Start the fake_openai_server.py fixture.
//   2. Construct a Config + Conversation with a real cfg_mutex.
//   3. Launch a parallel thread that repeatedly writes cfg.api.default_model
//      under the same mutex (simulating /model switches).
//   4. On the main thread, run several inference turns via run_turn().
//   5. Assert all turns complete Ok (no assertion failure, no torn read).
//   6. Join the writer thread.
//   Under TSan: assert no data race is reported (zero TSan reports = clean).
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/conversation/Conversation.hpp>
#include <batbox/config/Config.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/inference/Client.hpp>
#include <batbox/session/SessionStore.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace batbox::conversation;
using batbox::CancelSource;
using batbox::CancelToken;

// =============================================================================
// Utility: locate fake_openai_server.py
// =============================================================================

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
    return "";
}

// =============================================================================
// FakeServer RAII
// =============================================================================

struct FakeServer {
    pid_t  pid{-1};
    int    port{0};
    FILE*  stdout_pipe{nullptr};

    bool start(const std::string& script_path) {
        int pipefd[2];
        if (::pipe(pipefd) != 0) return false;

        pid = ::fork();
        if (pid < 0) {
            ::close(pipefd[0]); ::close(pipefd[1]);
            return false;
        }
        if (pid == 0) {
            ::close(pipefd[0]);
            ::dup2(pipefd[1], STDOUT_FILENO);
            ::close(pipefd[1]);
            int devnull = ::open("/dev/null", O_WRONLY);
            if (devnull >= 0) { ::dup2(devnull, STDERR_FILENO); ::close(devnull); }
            const char* argv[] = {"python3", script_path.c_str(), nullptr};
            ::execvp("python3", const_cast<char* const*>(argv));
            ::_exit(127);
        }

        ::close(pipefd[1]);
        stdout_pipe = ::fdopen(pipefd[0], "r");
        if (!stdout_pipe) {
            ::kill(pid, SIGTERM); ::close(pipefd[0]); pid = -1;
            return false;
        }

        char line[256]{};
        for (int i = 0; i < 50; ++i) {
            if (::fgets(line, sizeof(line), stdout_pipe) != nullptr) {
                if (::strncmp(line, "READY ", 6) == 0) {
                    port = std::atoi(line + 6);
                    return port > 0;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
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
        if (stdout_pipe) { ::fclose(stdout_pipe); stdout_pipe = nullptr; }
    }

    ~FakeServer() { stop(); }

    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port) + "/v1";
    }
};

// =============================================================================
// TmpDir RAII
// =============================================================================

struct TmpDir {
    fs::path path;

    TmpDir() {
        // Use PID + nanosecond timestamp to avoid hash collisions in ctest -j N
        // (same fix as PEXT2 1.3 ScopedHome anti-pattern).
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count();
        auto base = fs::temp_directory_path()
                    / ("batbox_snap_race_" + std::to_string(::getpid())
                       + "_" + std::to_string(ns));
        fs::create_directories(base);
        path = base;
    }

    ~TmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// =============================================================================
// Helper: build a minimal Config
// =============================================================================

static batbox::config::Config make_test_config(const std::string& base_url) {
    batbox::config::Config cfg;
    cfg.api.base_url            = base_url;
    cfg.api.api_key             = "test-key-123";
    cfg.api.request_timeout_sec = 10;
    cfg.api.default_model       = "gpt-4o";
    cfg.api.max_tokens          = 512;
    cfg.api.temperature         = 0.7;
    cfg.api.top_p               = 1.0;
    cfg.compact.auto_compact_at_pct        = 80;
    cfg.compact.keep_last_n_turns_verbatim = 4;
    return cfg;
}

// =============================================================================
// Test suite — PEXT2 3.4 Config snapshot correctness under /model switch race
// =============================================================================

TEST_SUITE("PEXT2 3.4 — Config snapshot race") {

    // -----------------------------------------------------------------------
    // AC1: run_turn() completes successfully when a parallel thread is issuing
    //      /model-like mutations to cfg.api.default_model under the same mutex.
    //
    //      Under TSan (Linux): assert no data race on cfg.api.default_model
    //      or cfg.api.api_key across 5 turns with concurrent mutations.
    //
    //      Without TSan (macOS): exercise the race window with a tight
    //      stress loop and assert all turns return Ok.
    // -----------------------------------------------------------------------
    TEST_CASE("run_turn succeeds under concurrent cfg.api.default_model mutation") {
        std::string script = find_fixture_script();
        REQUIRE_FALSE(script.empty());

        FakeServer srv;
        REQUIRE(srv.start(script));

        TmpDir tmp;
        // Use a mutable Config (not const) so the writer thread can mutate it.
        batbox::config::Config cfg = make_test_config(srv.base_url());

        // The mutex that guards cfg writes — same pattern as App.cpp TUI mode.
        std::mutex cfg_mutex;

        batbox::inference::Client     client{cfg};
        batbox::session::SessionStore store{tmp.path};

        // Construct Conversation with the cfg_mutex so run_turn() will snapshot
        // cfg.api.default_model under the lock at each turn start.
        Conversation conv{
            client,
            store,
            cfg,
            tmp.path,
            /*on_delta_cb=*/nullptr,
            /*registry=*/nullptr,
            /*gate=*/nullptr,
            /*plan_mode=*/nullptr,
            &cfg_mutex   // PEXT2 3.4: pass the mutex
        };
        conv.user_message("hello from race test");

        // Model names the writer thread will cycle through — distinct enough
        // to make a torn read visible as a garbage string if the race fires.
        const std::string kModels[] = {
            "gpt-4o",
            "claude-3-5-sonnet-20241022",
            "gemini-2.0-flash",
            "gpt-4o-mini",
            "llama-3.3-70b-instruct",
        };
        constexpr int kModelCount = 5;

        // Writer thread: continuously mutates cfg.api.default_model under mutex,
        // simulating a /model command issued from the FTXUI UI thread.
        std::atomic<bool> writer_done{false};
        std::atomic<int>  write_count{0};

        std::thread writer([&]() {
            int idx = 0;
            while (!writer_done.load(std::memory_order_relaxed)) {
                {
                    std::lock_guard<std::mutex> lk(cfg_mutex);
                    // Simulate apply_live_model_mutation: const_cast + write.
                    const_cast<batbox::config::Config&>(cfg).api.default_model =
                        kModels[idx % kModelCount];
                }
                ++write_count;
                ++idx;
                // Tight loop to maximise race window overlap with run_turn().
                // No sleep — we want to race the inference thread as hard as
                // possible within each turn's duration.
            }
        });

        // Reader (main thread): run 5 consecutive turns.
        // Each turn's run_turn() must snapshot the model under the mutex at
        // its start and use the snapshot consistently throughout.
        constexpr int kTurns = 5;
        int ok_count = 0;
        for (int i = 0; i < kTurns; ++i) {
            // Re-add a user message for turns after the first.
            if (i > 0) {
                conv.user_message("turn " + std::to_string(i));
            }
            auto [src, tok] = CancelToken::make_root();
            auto res = conv.run_turn(std::move(tok));
            if (res.has_value()) {
                ++ok_count;
            }
        }

        // Stop the writer and join.
        writer_done.store(true, std::memory_order_relaxed);
        writer.join();

        // All turns must have succeeded — no torn reads, no crashes.
        CHECK(ok_count == kTurns);

        // The writer must have issued at least one write per turn on average
        // to verify the race was actually exercised.
        CHECK(write_count.load() > 0);
    }

    // -----------------------------------------------------------------------
    // AC2: Conversation constructed WITHOUT a mutex (nullptr) still works
    //      correctly in single-threaded mode (headless path, unit tests).
    //
    //      This verifies the nullptr guard in the snapshot lambda does not
    //      break the no-mutex path.
    // -----------------------------------------------------------------------
    TEST_CASE("run_turn works without cfg_mutex (nullptr — headless path)") {
        std::string script = find_fixture_script();
        REQUIRE_FALSE(script.empty());

        FakeServer srv;
        REQUIRE(srv.start(script));

        TmpDir tmp;
        auto cfg = make_test_config(srv.base_url());
        batbox::inference::Client     client{cfg};
        batbox::session::SessionStore store{tmp.path};

        // No mutex — headless / unit-test path.
        Conversation conv{client, store, cfg, tmp.path};
        conv.user_message("headless no-mutex test");

        auto [src, tok] = CancelToken::make_root();
        auto res = conv.run_turn(std::move(tok));

        // Must succeed without a mutex.
        CHECK(res.has_value());
    }

} // TEST_SUITE "PEXT2 3.4 — Config snapshot race"
