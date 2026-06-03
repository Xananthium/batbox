// tests/integration/test_standing_registry.cpp
// =============================================================================
// Integration tests for the standing-subagent registry (DIS-988, S2/S3):
// the warm, interrogable subagent window + LRU eviction + cancel tokens.
//
// Hermetic: drives the shared fake OpenAI-compatible endpoint
// (tests/fixtures/fake_openai_server.py) — no GPU, no network. Its default
// /v1/chat/completions streaming branch emits a 100-chunk content stream then
// finish_reason=stop, so a SubAgent reaches quiescence with non-empty output.
//
// Determinism strategy (no fixed sleeps):
//   * AC2  — promote() BEFORE start(): the agent is standing from the first
//            quiescence, so it parks deterministically (no close-vs-promote race).
//   * AC3  — interrogate() BLOCKS until the agent has parked and answered, so
//            after it returns the agent is provably warm. The LRU test sequences
//            promotes/interrogates and asserts on the synchronous pool state.
//   * AC5  — interrogate()/promote() on dead/unknown handles return immediately
//            (sentinel / no-op); the reaper guarantees a blocked get() is freed.
//
// Coverage:
//   [AC2] a promoted SubAgent stays warm — interrogate returns a real answer,
//         and a SECOND interrogation is answered too (window not collapsed).
//   [AC5] interrogate after cancel/evict yields the empty sentinel, never hangs.
//   [AC5] supervisor promote/interrogate on an unknown handle is a safe no-op.
//   [AC3] supervisor LRU: promote 3, refresh one via interrogate, lower the
//         bound → the least-recently-interrogated is evicted (not the refreshed).
//   [AC3] eviction is lossless: gold written to the out-of-band notepad survives
//         the evicted window and is still reachable via the pad.
//
// Build standalone is impractical (full agents+conversation+inference stack);
// built via CMake target test_standing_registry (link set in tests/CMakeLists.txt).
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/agents/AgentEvent.hpp>
#include <batbox/agents/AgentSpec.hpp>
#include <batbox/agents/AgentSupervisor.hpp>
#include <batbox/agents/SubAgent.hpp>
#include <batbox/config/Config.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/tools/NotepadStore.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using batbox::CancelSource;
using batbox::agents::AgentEventQueue;
using batbox::agents::AgentSpec;
using batbox::agents::AgentSupervisor;
using batbox::agents::SubAgent;
using batbox::config::Config;

// =============================================================================
// Locate fake_openai_server.py
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
// FakeServer RAII — forks python3, waits for "READY <port>" on stdout.
// =============================================================================
struct FakeServer {
    pid_t pid{-1};
    int   port{0};
    FILE* stdout_pipe{nullptr};

    bool start(const std::string& script_path) {
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
            const char* argv[] = {"python3", script_path.c_str(), nullptr};
            ::execvp("python3", const_cast<char* const*>(argv));
            ::_exit(127);
        }
        ::close(pipefd[1]);
        stdout_pipe = ::fdopen(pipefd[0], "r");
        if (!stdout_pipe) { ::kill(pid, SIGTERM); ::close(pipefd[0]); pid = -1; return false; }
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
        if (pid > 0) { ::kill(pid, SIGTERM); int s = 0; ::waitpid(pid, &s, 0); pid = -1; }
        if (stdout_pipe) { ::fclose(stdout_pipe); stdout_pipe = nullptr; }
    }
    ~FakeServer() { stop(); }
    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port) + "/v1";
    }
};

// A Config pointing the main api endpoint at the fake server.
static Config make_test_config(const std::string& base_url) {
    Config cfg;
    cfg.api.base_url            = base_url;
    cfg.api.api_key             = "test-key-123";
    cfg.api.request_timeout_sec = 10;
    return cfg;
}

// Point a HERMETIC temp HOME so SessionStore / NotepadStore write to /tmp, and
// (for supervisor tests that use Config::load_default internally) the fake
// endpoint via env.  Idempotent across test cases.
static void hermetic_env(const std::string& base_url) {
    static const std::string home = [] {
        std::string h = "/tmp/batbox-dis988-test";
        fs::create_directories(h);
        return h;
    }();
    ::setenv("HOME", home.c_str(), 1);
    ::setenv("BATBOX_API_BASE_URL", base_url.c_str(), 1);
    ::setenv("BATBOX_API_KEY", "test-key-123", 1);
}

TEST_SUITE("StandingRegistry") {

    // -----------------------------------------------------------------------
    // AC2 — the decisive change: a promoted SubAgent's window stays warm and
    // answers interrogations; it is NOT collapsed to a string on pickup.
    // -----------------------------------------------------------------------
    TEST_CASE("AC2: promoted SubAgent stays warm and answers interrogations") {
        const std::string script = find_fixture_script();
        REQUIRE_FALSE(script.empty());
        FakeServer srv;
        REQUIRE(srv.start(script));
        hermetic_env(srv.base_url());

        Config cfg = make_test_config(srv.base_url());
        AgentEventQueue q;
        CancelSource root;
        AgentSpec spec; spec.name = "warm";

        SubAgent agent("a1", spec, "do the initial task", root.token(), q, cfg,
                       []() {});
        agent.promote();                 // standing BEFORE start → deterministic park
        CHECK(agent.is_standing());
        agent.start();

        // The window answers a follow-up against its still-engulfed context.
        std::string a1 = agent.interrogate("first follow-up").get();
        CHECK_FALSE(a1.empty());

        // And it is STILL warm — a second interrogation is answered too.
        std::string a2 = agent.interrogate("second follow-up").get();
        CHECK_FALSE(a2.empty());

        // last_result() surfaces the most recent answer (the status-line source).
        CHECK_FALSE(agent.last_result().empty());

        root.request_stop();             // teardown: parked loop wakes and exits
    }

    // -----------------------------------------------------------------------
    // AC5 — safety: interrogate after cancel/evict yields the empty sentinel and
    // never hangs (the run-loop reaper fulfils any in-flight promise).
    // -----------------------------------------------------------------------
    TEST_CASE("AC5: interrogate after cancel returns sentinel, never hangs") {
        const std::string script = find_fixture_script();
        REQUIRE_FALSE(script.empty());
        FakeServer srv;
        REQUIRE(srv.start(script));
        hermetic_env(srv.base_url());

        Config cfg = make_test_config(srv.base_url());
        AgentEventQueue q;
        CancelSource root;
        AgentSpec spec; spec.name = "warm";

        SubAgent agent("a2", spec, "task", root.token(), q, cfg, []() {});
        agent.promote();
        agent.start();
        // Confirm the window is warm first (so we are cancelling a parked agent).
        CHECK_FALSE(agent.interrogate("warmup").get().empty());

        root.request_stop();             // evict / parent-cancel cascade

        // Any subsequent interrogation resolves to the empty sentinel — the
        // get() below MUST return (a hang here would fail the test by timeout).
        std::string ans = agent.interrogate("after eviction").get();
        CHECK(ans.empty());
    }

    // -----------------------------------------------------------------------
    // AC5 — supervisor promote/interrogate on an unknown handle is a safe no-op.
    // -----------------------------------------------------------------------
    TEST_CASE("AC5: supervisor ops on an unknown handle are safe") {
        AgentSupervisor sup;
        sup.promote("does-not-exist");                       // no throw
        CHECK(sup.interrogate("does-not-exist", "q").empty());
        CHECK(sup.standing_count() == 0);
        sup.set_max_standing_subagents(0);                   // no throw on empty
        CHECK(sup.standing_count() == 0);
    }

    // -----------------------------------------------------------------------
    // AC3 — LRU eviction: the least-recently-interrogated standing subagent is
    // evicted under pressure; an interrogated one survives.
    // -----------------------------------------------------------------------
    TEST_CASE("AC3: lowering the bound evicts the least-recently-interrogated") {
        const std::string script = find_fixture_script();
        REQUIRE_FALSE(script.empty());
        FakeServer srv;
        REQUIRE(srv.start(script));
        hermetic_env(srv.base_url());    // hermetic HOME for sessions

        AgentSupervisor sup;             // default max_concurrent=4, max_standing=4
        // Point spawned agents at the fake server (AgentSupervisor otherwise
        // builds them from Config::load_default = real openai, no key).
        sup.set_agent_config(make_test_config(srv.base_url()));
        AgentSpec spec; spec.name = "warm";
        CancelSource root;

        const std::string a = sup.spawn(spec, "task A", "", root.token());
        sup.promote(a);
        // interrogate BLOCKS until A has parked + answered → A provably warm.
        CHECK_FALSE(sup.interrogate(a, "q").empty());

        const std::string b = sup.spawn(spec, "task B", "", root.token());
        sup.promote(b);
        CHECK_FALSE(sup.interrogate(b, "q").empty());

        const std::string c = sup.spawn(spec, "task C", "", root.token());
        sup.promote(c);
        CHECK_FALSE(sup.interrogate(c, "q").empty());

        CHECK(sup.standing_count() == 3);

        // Refresh A's recency so B becomes the least-recently-interrogated.
        CHECK_FALSE(sup.interrogate(a, "refresh").empty());

        // Pressure: bound the pool to 2 → exactly one eviction (the LRU = B).
        sup.set_max_standing_subagents(2);
        CHECK(sup.standing_count() == 2);

        std::vector<std::string> ids;
        for (const auto& s : sup.standing_status()) ids.push_back(s.id);
        const auto has = [&](const std::string& id) {
            return std::find(ids.begin(), ids.end(), id) != ids.end();
        };
        CHECK(has(a));        // refreshed → survives
        CHECK(has(c));        // most-recently-promoted → survives
        CHECK_FALSE(has(b));  // least-recently-interrogated → evicted

        root.request_stop();
        sup.wait_all();
    }

    // -----------------------------------------------------------------------
    // AC3 — eviction is lossless by construction: gold lives in the out-of-band
    // notepad (S6), so it survives the discarded window and stays reachable.
    // -----------------------------------------------------------------------
    TEST_CASE("AC3: gold in the notepad survives eviction of the window") {
        const std::string script = find_fixture_script();
        REQUIRE_FALSE(script.empty());
        FakeServer srv;
        REQUIRE(srv.start(script));
        hermetic_env(srv.base_url());

        Config cfg = make_test_config(srv.base_url());
        AgentEventQueue q;
        CancelSource root;
        AgentSpec spec; spec.name = "warm";

        SubAgent agent("a-gold", spec, "investigate", root.token(), q, cfg, []() {});
        agent.promote();
        agent.start();
        CHECK_FALSE(agent.interrogate("warmup").get().empty());

        // The gold the standing agent would jot lives in the out-of-band pad,
        // keyed by the agent's session — independent of the agent's window.
        batbox::tools::NotepadStore pad;
        const std::string key =
            batbox::tools::NotepadStore::session_key("sess-gold", std::string{});
        REQUIRE(pad.append(key, "GOLD: the root cause is the off-by-one in foo()",
                           "findings").has_value());

        // Evict the window (LRU eviction fires the same stop_token).
        root.request_stop();
        CHECK(agent.interrogate("post-evict").get().empty());  // window gone

        // The gold is STILL reachable via the re-injected pad — lossless.
        const std::string slice = pad.reinjection_slice(key);
        CHECK(slice.find("GOLD: the root cause") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // DIS-1001 — promote() racing a natural quiescent CLOSED exit must not
    // double-release a semaphore slot or leave a stale standing_lru entry.
    //
    // The race (found by Wren during the DIS-988 gate): the run loop caches
    // standing_=false at quiescence and never re-checks it before the closed
    // exit; promote() step 1 sets standing_=true too late; if on_exit() then
    // runs BEFORE promote() step 2, both release the same slot and step 2 leaves
    // a stale LRU entry for a `done` agent.
    //
    // We reproduce the EXACT interleaving deterministically with two seams (no
    // sleeps): a quiescence seam pauses the agent in the "standing cached=false,
    // status still running, closed exit committed" window; a promote seam pauses
    // promote() between step 1 (which therefore provably sees `running`) and
    // step 2.  We release the agent first, drain it to a full natural exit
    // (wait_all() ⇒ on_exit() complete), THEN let step 2 proceed — forcing the
    // on_exit-before-step2 ordering that triggers the bug.  With the fix, step 2
    // refuses to register the now-exited agent.
    // -----------------------------------------------------------------------
    TEST_CASE("DIS-1001: promote racing natural quiescent-exit keeps slots+LRU consistent") {
        const std::string script = find_fixture_script();
        REQUIRE_FALSE(script.empty());
        FakeServer srv;
        REQUIRE(srv.start(script));
        hermetic_env(srv.base_url());

        AgentSupervisor sup(2);          // max_concurrent = 2
        sup.set_agent_config(make_test_config(srv.base_url()));
        AgentSpec spec; spec.name = "racer";
        CancelSource root;

        // Spawn X — it acquires one of the two slots and starts running.
        const std::string x = sup.spawn(spec, "task", "", root.token());

        std::mutex m;
        std::condition_variable cv;
        bool quiesced = false, release_quiesce = false;   // Seam A (agent)
        bool promote_paused = false, release_promote = false;  // Seam B (promote)

        // Seam A: pause X at its first quiescence (standing cached=false, running).
        sup.set_agent_quiescence_hook_for_test(x, [&] {
            { std::lock_guard<std::mutex> lk(m); quiesced = true; }
            cv.notify_all();
            std::unique_lock<std::mutex> lk(m);
            cv.wait(lk, [&] { return release_quiesce; });
        });

        // Seam B: pause promote() between step 1 and step 2.
        sup.set_promote_race_hook_for_test([&] {
            { std::lock_guard<std::mutex> lk(m); promote_paused = true; }
            cv.notify_all();
            std::unique_lock<std::mutex> lk(m);
            cv.wait(lk, [&] { return release_promote; });
        });

        // 1. Wait until X has cached standing=false at quiescence (still running).
        {
            std::unique_lock<std::mutex> lk(m);
            cv.wait(lk, [&] { return quiesced; });
        }

        // 2. Promote X on another thread.  step 1 sees `running`, sets standing_
        //    (too late for X's cached read), then pauses at Seam B.
        std::thread pth([&] { sup.promote(x); });
        {
            std::unique_lock<std::mutex> lk(m);
            cv.wait(lk, [&] { return promote_paused; });
        }

        // 3. Release X's loop → it proceeds on the cached standing=false → natural
        //    closed exit → on_exit() runs (marks exited, releases X's own slot).
        { std::lock_guard<std::mutex> lk(m); release_quiesce = true; }
        cv.notify_all();

        // 4. Block until X has fully exited (on_exit() complete ⇒ active_count 0).
        sup.wait_all();

        // 5. Release promote() step 2 → it MUST refuse to register the exited X.
        { std::lock_guard<std::mutex> lk(m); release_promote = true; }
        cv.notify_all();
        pth.join();

        // Invariants: no stale LRU entry, and the slot pool is permit-consistent
        // (exactly max_concurrent permits available — not max_concurrent + 1).
        CHECK(sup.standing_count() == 0);
        CHECK(sup.available_slots_for_test() == 2);
    }
}
