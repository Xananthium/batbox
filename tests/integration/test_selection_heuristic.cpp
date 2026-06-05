// tests/integration/test_selection_heuristic.cpp
// =============================================================================
// Integration tests for the DIS-1007 closed-vs-standing selection heuristic:
//   batbox::tools::ShapeSelectionHeuristic  — the predict-ahead classifier
//   batbox::tools::StandingSelector         — the IResultDistiller that wraps the
//                                             closed one-shot SubagentDistiller and
//                                             adds the closed-vs-standing branch.
//
// Decision A — "predict-ahead, confirm-after":
//   * No investigation signal  → CLOSED (one-shot, byte-identical to S1/S4).
//   * Investigation predicted   → STANDING: first-turn gold via the SHARED
//                                 report_gold contract, then confirm-after on
//                                 follow_up_ok/confidence → promote (keep warm)
//                                 or close.
//
// Hermetic — NO GPU, NO network.  Two fake servers, both spawned per test via
// the fork/READY-<port> RAII pattern from the sibling tests:
//   * fake_distill_server.py  — answers the StandingSelector's report_gold
//       first-turn extraction (cfg.distill.* on the SELECTOR's config).  The new
//       --mode goldnofollowup drives follow_up_ok==false.
//   * fake_openai_server.py   — answers the WARM SubAgent's conversation +
//       follow-up interrogations (the SUPERVISOR's agent config distill.*).
//
// Endpoint topology (the crux):
//   - The SELECTOR distills the first turn via cfg.distill.* → the distill server.
//   - The warm SubAgent is spawned with use_distill_endpoint=true, so it reads
//     the SUPERVISOR's cfg.distill.* → the openai server (which streams a real
//     conversation a follow-up can run against).  Setting these to two DIFFERENT
//     servers proves the standing path uses both inference paths end-to-end.
//
// Coverage:
//   [AC1] no investigation signal → StandingSelector output byte-identical to a
//         bare SubagentDistiller; no standing window; standing_count()==0.
//   [AC2] investigation signal → standing_count()>=1 and interrogate(id, q)
//         returns a non-empty answer WITHOUT re-engulfing the source.
//   [AC3] follow_up_ok==true → promoted; follow_up_ok==false → NO standing window
//         even though shape leaned standing.  Caller never flags (classify() takes
//         no mode arg).
//   [AC4] with set_max_standing_subagents(N), a burst of >N investigations never
//         exceeds N standing; LRU evicts the least-recently-interrogated.
//   [AC5] 3090 unreachable / spawn-unavailable / gold parse fail → falls back to
//         closed one-shot or original result; never throws.
//   [AC6] each of the 4 decision paths is observable (CLOSED / PROMOTE /
//         FOLLOW_UP_OK-CANCEL via standing_count + last_standing_id; LRU-EVICT
//         via standing_count under pressure).
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/agents/AgentSpec.hpp>
#include <batbox/agents/AgentSupervisor.hpp>
#include <batbox/config/Config.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/tools/SelectionHeuristic.hpp>
#include <batbox/tools/StandingSelector.hpp>
#include <batbox/tools/SubagentDistiller.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using batbox::CancelSource;
using batbox::CancelToken;
using batbox::Json;
using batbox::agents::AgentSupervisor;
using batbox::config::Config;
using batbox::tools::DispatchMode;
using batbox::tools::ShapeSelectionHeuristic;
using batbox::tools::StandingSelector;
using batbox::tools::SubagentDistiller;
using batbox::tools::ToolContext;
using batbox::tools::ToolResult;

// =============================================================================
// Fixture locators
// =============================================================================

static std::string find_script(const char* name) {
#ifdef BATBOX_FIXTURE_DIR
    {
        fs::path p = fs::path(BATBOX_FIXTURE_DIR) / name;
        if (fs::exists(p)) return p.string();
    }
#endif
    fs::path dir = fs::current_path();
    for (int depth = 0; depth < 8; ++depth) {
        fs::path candidate = dir / "tests" / "fixtures" / name;
        if (fs::exists(candidate)) return candidate.string();
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    return "";
}

// =============================================================================
// FakeServer RAII — forks python3, reads "READY <port>"; optional --mode.
// =============================================================================

struct FakeServer {
    pid_t pid{-1};
    int   port{0};
    FILE* stdout_pipe{nullptr};

    bool start(const std::string& script_path, const std::string& mode = "") {
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
            if (mode.empty()) {
                const char* argv[] = {"python3", script_path.c_str(), nullptr};
                ::execvp("python3", const_cast<char* const*>(argv));
            } else {
                const char* argv[] = {"python3", script_path.c_str(),
                                      "--mode", mode.c_str(), nullptr};
                ::execvp("python3", const_cast<char* const*>(argv));
            }
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

// =============================================================================
// Config / context helpers
// =============================================================================

// A config whose distill.* points at the distill server (the SELECTOR's
// report_gold first-turn endpoint).  cfg.api is left at default ON PURPOSE — the
// standing path must hit cfg.distill.*, proving endpoint separation (as S4 does).
static Config make_selector_cfg(const std::string& distill_base, std::size_t threshold = 100) {
    Config cfg;
    cfg.distill.enabled                = true;
    cfg.distill.base_url               = distill_base;
    cfg.distill.api_key                = "test-key-123";
    cfg.distill.model                  = "fake-distill-model";
    cfg.distill.max_tool_response_size = threshold;
    cfg.distill.request_timeout_sec    = 10;
    cfg.distill.max_tokens             = 256;
    return cfg;
}

// The SUPERVISOR's agent config: distill.* points at the OPENAI server so a
// spawned SubAgent (use_distill_endpoint=true) runs a real conversation there.
static Config make_supervisor_cfg(const std::string& openai_base) {
    Config cfg;
    cfg.api.base_url            = openai_base;
    cfg.api.api_key             = "test-key-123";
    cfg.api.request_timeout_sec = 10;
    cfg.distill.enabled         = true;
    cfg.distill.base_url        = openai_base;   // warm SubAgent talks here
    cfg.distill.api_key         = "test-key-123";
    cfg.distill.model           = "fake-distill-model";
    cfg.distill.request_timeout_sec = 10;
    return cfg;
}

// Point a hermetic HOME so SessionStore writes under /tmp (the warm SubAgent
// persists a session).  Mirrors test_standing_registry's hermetic_env.
static void hermetic_home() {
    static const std::string home = [] {
        std::string h = "/tmp/batbox-dis1007-test";
        fs::create_directories(h);
        return h;
    }();
    ::setenv("HOME", home.c_str(), 1);
}

struct Ctx {
    CancelSource src;
    ToolContext  ctx;
    Ctx() { ctx.cancel_token = src.token(); }
};

static const std::string kGold = "DISTILLED_GOLD_LINE";

// A body large enough that the result-shape signal classifies it standing.
static std::string big_body() { return std::string(20 * 1024, 'x'); }
// A small single-fact body (lookup by shape).
static std::string small_body() { return std::string("the answer is 42"); }

// =============================================================================
// ShapeSelectionHeuristic — the predict-ahead classifier (pure, no server). [AC3/AC6]
// =============================================================================

TEST_CASE("heuristic: broad-search tool identity → Standing; single fact → Closed [AC3]") {
    ShapeSelectionHeuristic h;

    // Tool semantics: grep/glob/web_search/web_fetch are investigations.
    CHECK(h.classify("grep",       Json::object(), ToolResult::ok(small_body())) == DispatchMode::Standing);
    CHECK(h.classify("glob",       Json::object(), ToolResult::ok(small_body())) == DispatchMode::Standing);
    CHECK(h.classify("web_search", Json::object(), ToolResult::ok(small_body())) == DispatchMode::Standing);
    CHECK(h.classify("web_fetch",  Json::object(), ToolResult::ok(small_body())) == DispatchMode::Standing);

    // A single resolved fact from a non-search tool → lookup.
    CHECK(h.classify("read_file",  Json::object(), ToolResult::ok(small_body())) == DispatchMode::Closed);
    CHECK(h.classify("config_get", Json::object(), ToolResult::ok(small_body())) == DispatchMode::Closed);

    // Tool identity is the ONLY positive input that needs no caller flag — the
    // classifier signature carries no mode argument at all (anti-pattern #3).
    CHECK(ShapeSelectionHeuristic::is_investigation_tool("grep"));
    CHECK_FALSE(ShapeSelectionHeuristic::is_investigation_tool("read_file"));
}

TEST_CASE("heuristic: result shape (large / many sections) → Standing; errors → Closed [AC3]") {
    ShapeSelectionHeuristic h;

    // Large body from a non-search tool → investigation by shape.
    CHECK(h.classify("read_file", Json::object(), ToolResult::ok(big_body())) == DispatchMode::Standing);

    // Many distinct sections (newline-delimited records) → investigation by shape.
    std::string many;
    for (int i = 0; i < 40; ++i) many += "record " + std::to_string(i) + "\n";
    CHECK(h.classify("read_file", Json::object(), ToolResult::ok(many)) == DispatchMode::Standing);

    // An error result is never an investigation (must surface verbatim).
    CHECK(h.classify("grep", Json::object(), ToolResult::error(big_body())) == DispatchMode::Closed);
}

// =============================================================================
// [AC1] no investigation signal → byte-identical to a bare SubagentDistiller.
// =============================================================================

TEST_CASE("StandingSelector: lookup is byte-identical to the closed distiller [AC1]") {
    std::string distill = find_script("fake_distill_server.py");
    REQUIRE_FALSE(distill.empty());
    FakeServer dsrv;
    REQUIRE(dsrv.start(distill, "gold"));

    Config cfg = make_selector_cfg(dsrv.base_url());

    // Bare closed distiller (the reference) and the selector with NO supervisor.
    SubagentDistiller bare{cfg};
    StandingSelector  selector{cfg, /*supervisor=*/nullptr};

    Ctx c1, c2;
    // "read_file" of a small single fact → Closed (lookup): the selector must
    // delegate verbatim to the closed distiller.
    ToolResult ref = bare.distill("read_file", Json{{"path", "/x"}},
                                  ToolResult::ok(small_body()), c1.ctx);
    ToolResult got = selector.distill("read_file", Json{{"path", "/x"}},
                                      ToolResult::ok(small_body()), c2.ctx);

    CHECK(got == ref);                 // byte-identical ToolResult
    CHECK(got.body == kGold);
    CHECK(selector.last_standing_id().empty());  // no standing window created
}

// =============================================================================
// [AC2]+[AC3]+[AC6] investigation + follow_up_ok==true → PROMOTE; warm window
// answers a follow-up WITHOUT re-engulfing the source.
// =============================================================================

TEST_CASE("StandingSelector: investigation + follow_up_ok → promoted warm window [AC2][AC3][AC6]") {
    std::string distill = find_script("fake_distill_server.py");
    std::string openai  = find_script("fake_openai_server.py");
    REQUIRE_FALSE(distill.empty());
    REQUIRE_FALSE(openai.empty());
    hermetic_home();

    FakeServer dsrv, osrv;
    REQUIRE(dsrv.start(distill, "gold"));   // follow_up_ok=true, confidence=0.91
    REQUIRE(osrv.start(openai));            // warm SubAgent conversation endpoint

    AgentSupervisor sup;
    sup.set_agent_config(make_supervisor_cfg(osrv.base_url()));

    Config cfg = make_selector_cfg(dsrv.base_url());
    StandingSelector selector{cfg, &sup};

    Ctx c;
    // "grep" → investigation by tool semantics; gold reports follow_up_ok=true.
    ToolResult out = selector.distill("grep", Json{{"pattern", "TODO"}},
                                      ToolResult::ok(big_body()), c.ctx);

    // The gold is returned (closed-equivalent shape, standing provenance).
    CHECK_FALSE(out.is_error);
    CHECK(out.body == kGold);
    REQUIRE(out.structured_payload.has_value());
    CHECK(out.structured_payload->at("standing") == true);

    // PROMOTE path is structurally provable: a standing window exists.
    CHECK(sup.standing_count() >= 1);
    const std::string id = selector.last_standing_id();
    CHECK_FALSE(id.empty());

    // AC2: the warm window answers a follow-up WITHOUT re-engulfing the source.
    std::string answer = sup.interrogate(id, "what else did you find?");
    CHECK_FALSE(answer.empty());

    // Teardown: a standing window is owned + cancelled by the supervisor (it is
    // intended to outlive the dispatch).  The supervisor destructor cancels and
    // joins every agent, so no explicit cancel/wait_all is needed here.
}

// =============================================================================
// [AC3]+[AC6] investigation predicted but follow_up_ok==false → NO standing window.
// =============================================================================

TEST_CASE("StandingSelector: follow_up_ok==false → closed, no warm window [AC3][AC6]") {
    std::string distill = find_script("fake_distill_server.py");
    REQUIRE_FALSE(distill.empty());
    hermetic_home();

    FakeServer dsrv;
    REQUIRE(dsrv.start(distill, "goldnofollowup"));  // follow_up_ok=false

    AgentSupervisor sup;
    // No agent-config server needed: the confirm-after closes BEFORE any spawn.

    Config cfg = make_selector_cfg(dsrv.base_url());
    StandingSelector selector{cfg, &sup};

    Ctx c;
    // grep → shape leans standing, but the subagent says follow_up_ok=false.
    ToolResult out = selector.distill("grep", Json{{"pattern", "x"}},
                                      ToolResult::ok(big_body()), c.ctx);

    CHECK(out.body == kGold);                          // gold still returned
    REQUIRE(out.structured_payload.has_value());
    CHECK(out.structured_payload->at("follow_up_ok") == false);

    // FOLLOW_UP_OK-CANCEL is structurally provable: NO standing window remains.
    CHECK(sup.standing_count() == 0);
    CHECK(selector.last_standing_id().empty());
}

// =============================================================================
// [AC4] a burst of >N investigations never exceeds N standing; LRU evicts the
// least-recently-interrogated.
// =============================================================================

TEST_CASE("StandingSelector: burst of investigations bounded by max_standing [AC4]") {
    std::string distill = find_script("fake_distill_server.py");
    std::string openai  = find_script("fake_openai_server.py");
    REQUIRE_FALSE(distill.empty());
    REQUIRE_FALSE(openai.empty());
    hermetic_home();

    FakeServer dsrv, osrv;
    REQUIRE(dsrv.start(distill, "gold"));   // every first-turn → follow_up_ok=true
    REQUIRE(osrv.start(openai));

    AgentSupervisor sup;
    sup.set_agent_config(make_supervisor_cfg(osrv.base_url()));
    sup.set_max_standing_subagents(2);      // the bound (no NEW bound in the selector)

    Config cfg = make_selector_cfg(dsrv.base_url());
    StandingSelector selector{cfg, &sup};

    // Five investigations in a burst — each predicts standing + promotes.
    std::vector<std::string> ids;
    for (int i = 0; i < 5; ++i) {
        Ctx c;
        ToolResult out = selector.distill("grep", Json{{"pattern", std::to_string(i)}},
                                          ToolResult::ok(big_body()), c.ctx);
        CHECK(out.body == kGold);
        const std::string id = selector.last_standing_id();
        CHECK_FALSE(id.empty());
        ids.push_back(id);
        // Refresh recency so the interrogated one survives eviction pressure.
        (void)sup.interrogate(id, "ping");
    }

    // LRU-EVICT-on-pressure is structurally provable: the pool never exceeds N.
    CHECK(sup.standing_count() == 2);

    // The two survivors are the most-recently-interrogated (the last two spawned).
    std::vector<std::string> alive;
    for (const auto& s : sup.standing_status()) alive.push_back(s.id);
    const auto has = [&](const std::string& id) {
        return std::find(alive.begin(), alive.end(), id) != alive.end();
    };
    CHECK(has(ids[4]));
    CHECK(has(ids[3]));
    CHECK_FALSE(has(ids[0]));  // earliest → evicted

    // Standing windows are torn down by the supervisor destructor (cancel+join).
}

// =============================================================================
// [AC5] robustness — every standing-path failure falls back; never throws.
// =============================================================================

TEST_CASE("StandingSelector: distill endpoint unreachable → original result, no throw [AC5]") {
    std::string distill = find_script("fake_distill_server.py");
    REQUIRE_FALSE(distill.empty());

    // Obtain a now-closed port.
    int dead_port = 0;
    {
        FakeServer s;
        REQUIRE(s.start(distill, "gold"));
        dead_port = s.port;
    }
    AgentSupervisor sup;
    Config cfg = make_selector_cfg("http://127.0.0.1:" + std::to_string(dead_port) + "/v1");
    cfg.distill.request_timeout_sec = 3;  // fail fast
    StandingSelector selector{cfg, &sup};

    Ctx c;
    const std::string big = big_body();
    // grep → standing predicted, but the endpoint is dead: standing first-turn
    // fails → closed fallback also fails (same dead endpoint) → original result.
    ToolResult out = selector.distill("grep", Json::object(), ToolResult::ok(big), c.ctx);
    CHECK_FALSE(out.is_error);
    CHECK(out.body == big);                       // data preserved intact
    CHECK_FALSE(out.structured_payload.has_value());
    CHECK(sup.standing_count() == 0);             // no window leaked
    CHECK(selector.last_standing_id().empty());
}

TEST_CASE("StandingSelector: null supervisor → standing unavailable, closed fallback [AC5]") {
    std::string distill = find_script("fake_distill_server.py");
    REQUIRE_FALSE(distill.empty());
    FakeServer dsrv;
    REQUIRE(dsrv.start(distill, "gold"));

    Config cfg = make_selector_cfg(dsrv.base_url());
    StandingSelector selector{cfg, /*supervisor=*/nullptr};  // no pool

    Ctx c;
    // grep → standing predicted, but no supervisor → falls back to the closed
    // one-shot, which succeeds against the (reachable) distill server.
    ToolResult out = selector.distill("grep", Json::object(),
                                      ToolResult::ok(big_body()), c.ctx);
    CHECK_FALSE(out.is_error);
    CHECK(out.body == kGold);                     // closed one-shot gold
    CHECK(selector.last_standing_id().empty());   // no standing window
}

TEST_CASE("StandingSelector: gold parse fails (wrong tool) → original result [AC5]") {
    std::string distill = find_script("fake_distill_server.py");
    REQUIRE_FALSE(distill.empty());
    FakeServer dsrv;
    REQUIRE(dsrv.start(distill, "wrongtool"));    // never calls report_gold

    AgentSupervisor sup;
    Config cfg = make_selector_cfg(dsrv.base_url());
    StandingSelector selector{cfg, &sup};

    Ctx c;
    const std::string big = big_body();
    ToolResult out = selector.distill("grep", Json::object(), ToolResult::ok(big), c.ctx);
    CHECK(out.body == big);                       // standing→closed→original
    CHECK_FALSE(out.structured_payload.has_value());
    CHECK(sup.standing_count() == 0);
}
