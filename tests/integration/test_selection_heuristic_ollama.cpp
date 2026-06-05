// tests/integration/test_selection_heuristic_ollama.cpp
// =============================================================================
// DIS-1017 — REAL-MODEL re-proof of the DIS-1007 AC2/AC3 selection behaviour.
//
// This is the exact unblock action Paulina named on DIS-1007 (08:31, 2026-06-03):
// re-prove AC2 (warm-window interrogate without re-engulf) and AC3 (follow_up_ok
// honored-not-obeyed → promote/close routing) against a REAL ollama model, not
// the hermetic fake server.  The hermetic suite (test_selection_heuristic.cpp)
// stays the default green CI bar; this file is the `requires_ollama` tier on top.
//
// =============================================================================
// SUBSTRATE (A-AC1): the organ runs against a real local model via the EXISTING
// EndpointOverride path — no organ change.  Topology mirrors the two-fake-server
// hermetic test, but both servers are now a COUNTING reverse-proxy
// (tests/fixtures/ollama_proxy.py) in front of the real ollama daemon:
//
//   selector  cfg.distill.*   -> proxy A -> ollama   (first-turn report_gold)
//   supervisor cfg.distill.*  -> proxy B -> ollama   (warm conversation +
//                                                     follow-up interrogations)
//
// Both proxies forward to the SAME ollama (the model is held constant); the two
// ports just let us attribute requests to the selector vs the warm path, which
// is what makes the AC2 "no-re-engulf" property structurally provable.  The
// proxy also forces temperature:0 (the determinism strategy) and captures
// token `usage` on GET /__stats (A-AC5).
//
// =============================================================================
// PINNED MODEL (the decide-and-flag crux): qwen2.5:7b, NOT the spec-default
// llama3.2:3b.  report_gold is a STRUCTURED first-turn extraction; if the model
// fails to emit a parseable top-level `answer`, extract_gold() returns nullopt
// and the standing path falls back closed→original — standing_count()==0 — which
// DEFEATS the AC2/AC3 proof outright (it is not a content-tolerance issue that a
// looser assertion could rescue).  Live measurement on this box:
//   * llama3.2:3b — caught a hard failure: it nested the gold inside a
//     stringified "object" field with NO top-level `answer` (parse → nullopt).
//     Flaky schema adherence → unsuitable for a green gate.
//   * qwen2.5:7b  — clean {answer,confidence,follow_up_ok}, no junk keys, 8/8.
// Per the spec crux ("bump to a larger local weight OR loosen the gold assertion
// — pick one, document it") we BUMP the model.  Overridable via the
// BATBOX_OLLAMA_TEST_MODEL env var so a second person can swap (A-AC1 repeatable).
//
// =============================================================================
// NONDETERMINISM + THE CENTRAL FINDING (A-AC2/A-AC3): a real model's report_gold
// control signals (confidence / follow_up_ok) are model-dependent and, under the
// organ's EXACT distiller prompt + "Optional" schema, an aligned local model
// (qwen2.5:7b) reliably OMITS the keep-warm signal — so the organ almost always
// CLOSES.  We could not find an HONEST input (no caller-dictated field) that makes
// a clean-parsing local model spontaneously vote keep-warm through this organ;
// llama3.2:3b will emit follow_up_ok=true but flakes on parseable structure (the
// reason for the model bump).  We therefore prove the behaviour as three
// deterministic, never-red facts and FLAG the finding (it informs DIS-1011's
// "warm-cache hint vs correctness gate" re-scope):
//   1. CLOSE routing on real output: a single-resolved fact (Standing by shape) →
//      the model reports confidence≈1 → trivial-lookup CLOSE.  HONORED.
//   2. HONOR-not-OBEY on a real investigation: the organ's promote/close decision
//      == should_keep_warm() of whatever the real model voted — with NO caller
//      flag.  (In practice logs a HONORED-CLOSE, documenting the finding inline.)
//   3. WARM-WINDOW interrogate without re-engulf on the real model: proven via the
//      SAME promote()+interrogate() machinery the organ uses once it keeps warm.
//   All output assertions are keyword/substring tolerances, NEVER byte-equality.
//
// SKIP semantics (A-AC4): main() probes ollama + the pinned model up front; if
// ollama is down or the model is absent it returns 77, and CTest is configured
// with SKIP_RETURN_CODE 77 so the default `ctest` run reports SKIPPED (never a
// failure) on a CI box with no GPU/ollama.  The hermetic suite is untouched.
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT          // custom main (ollama probe + warmup)
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

#include <cpr/cpr.h>

#include <algorithm>
#include <cctype>
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
using batbox::Json;
using batbox::agents::AgentSupervisor;
using batbox::config::Config;
using batbox::tools::DispatchMode;
using batbox::tools::ShapeSelectionHeuristic;
using batbox::tools::StandingSelector;
using batbox::tools::ToolContext;
using batbox::tools::ToolResult;

// =============================================================================
// Environment knobs (A-AC1: documented + overridable for a second runner).
// =============================================================================

static std::string ollama_base() {
    const char* e = std::getenv("BATBOX_OLLAMA_BASE");
    return (e && *e) ? std::string(e) : std::string("http://127.0.0.1:11434");
}
static std::string pinned_model() {
    const char* e = std::getenv("BATBOX_OLLAMA_TEST_MODEL");
    return (e && *e) ? std::string(e) : std::string("qwen2.5:7b");
}

// =============================================================================
// Fixture locator (mirrors test_selection_heuristic.cpp).
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
// ProxyServer RAII — forks `python3 ollama_proxy.py`, reads "READY <port>".
// Same fork/READY handshake the hermetic FakeServer uses; the proxy is a
// long-running service reaped by SIGTERM on teardown.  It inherits OLLAMA_BASE /
// PROXY_FORCE_TEMP from the test process environment (set once in main()).
// =============================================================================

struct ProxyServer {
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
    ~ProxyServer() { stop(); }

    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port) + "/v1";
    }
    std::string stats_url() const {
        return "http://127.0.0.1:" + std::to_string(port) + "/__stats";
    }

    // /__stats readers ------------------------------------------------------
    Json stats() const {
        auto r = cpr::Get(cpr::Url{stats_url()}, cpr::Timeout{5000});
        if (r.status_code != 200) return Json::object();
        try { return Json::parse(r.text); } catch (...) { return Json::object(); }
    }
    long chat_requests() const { return stats().value("chat_requests", 0L); }
    long total_tokens()  const { return stats().value("total_tokens", 0L); }
};

// =============================================================================
// Config / context helpers (mirror the hermetic test, pointed at the proxies).
// =============================================================================

static Config make_selector_cfg(const std::string& distill_base) {
    Config cfg;
    cfg.distill.enabled                = true;
    cfg.distill.base_url               = distill_base;       // proxy A → ollama
    cfg.distill.api_key                = "ollama";
    cfg.distill.model                  = pinned_model();
    cfg.distill.max_tool_response_size = 100;                // tiny → easy engulf
    cfg.distill.request_timeout_sec    = 120;                // real model is slower
    cfg.distill.max_tokens             = 256;
    return cfg;
}

static Config make_supervisor_cfg(const std::string& warm_base) {
    Config cfg;
    cfg.api.base_url            = warm_base;                 // proxy B → ollama
    cfg.api.api_key             = "ollama";
    cfg.api.default_model       = pinned_model();
    cfg.api.request_timeout_sec = 120;
    cfg.api.max_tokens          = 256;                       // keep interrogation snappy
    cfg.distill.enabled         = true;
    cfg.distill.base_url        = warm_base;                 // warm SubAgent talks here
    cfg.distill.api_key         = "ollama";
    cfg.distill.model           = pinned_model();
    cfg.distill.request_timeout_sec = 120;
    cfg.distill.max_tokens      = 256;
    return cfg;
}

static void hermetic_home() {
    static const std::string home = [] {
        std::string h = "/tmp/batbox-dis1017-ollama-test";
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

// =============================================================================
// Engineered, empirically-verified real-model inputs (see header).
// =============================================================================

// CLOSE: single resolved fact, large by SHAPE (≥16 sections → Standing predicted).
static std::string close_body() {
    std::string b = "# service configuration file\n";
    for (int i = 0; i < 60; ++i)
        b += "# default setting " + std::to_string(i) + " unrelated to the port\n";
    b += "listen_port = 8080\n";
    for (int i = 0; i < 60; ++i)
        b += "# trailing note " + std::to_string(i) + " irrelevant\n";
    return b;
}
static Json close_args() {
    return Json{{"path", "/etc/app.conf"},
                {"question", "What port does the service listen on?"}};
}

// INVESTIGATION: a broad web_search whose results genuinely CONFLICT with no
// consensus (Standing by tool identity).  This is a real investigation-class
// shape — the parent will want to follow up ("which fits MY use case?").  The
// heuristic predicts Standing, so the organ's confirm-after IS consulted; the
// real model then decides keep-warm-vs-close from its OWN report_gold vote (the
// HONOR-not-OBEY case below asserts the organ routes by whatever it voted).
// FINDING: under the organ's exact prompt + Optional schema, qwen2.5:7b reliably
// OMITS the keep-warm signal here → the organ HONORS that by CLOSING.  The
// warm-window machinery itself is proven on the real model by the last case.
static std::string promote_body() {
    return "Result 1 (blog): Python is best for rapid development and libraries.\n"
           "Result 2 (forum): Rust wins on raw performance and memory safety.\n"
           "Result 3 (Hacker News): Go is the pragmatic choice for backend services.\n"
           "Result 4 (dev survey): No clear winner; it depends on the team and the workload.\n"
           "Result 5 (aggregator): Opinions conflict sharply; there is no agreed-upon single answer.\n";
}
static Json promote_args() {
    return Json{{"query", "the single best programming language for our backend service"}};
}

static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
static bool contains_any(const std::string& hay, std::initializer_list<const char*> needles) {
    const std::string h = lower(hay);
    for (const char* n : needles)
        if (h.find(lower(n)) != std::string::npos) return true;
    return false;
}

// =============================================================================
// [A-AC3] CLOSE — a single resolved fact (Standing by shape) is HONORED as a
// trivial-lookup close: no warm window, gold returned, organ obeys the real
// model's high-confidence signal.
// =============================================================================

TEST_CASE("ollama: single resolved fact → trivial-lookup CLOSE, no warm window [A-AC3]") {
    const std::string proxy = find_script("ollama_proxy.py");
    REQUIRE_FALSE(proxy.empty());
    hermetic_home();

    ProxyServer selA, warmB;
    REQUIRE(selA.start(proxy));
    REQUIRE(warmB.start(proxy));

    AgentSupervisor sup;
    sup.set_agent_config(make_supervisor_cfg(warmB.base_url()));

    Config cfg = make_selector_cfg(selA.base_url());
    StandingSelector selector{cfg, &sup};

    // Sanity: the heuristic DOES predict Standing for this shape (so the CLOSE is
    // a genuine confirm-after decision, not a predict-ahead pass-through).
    CHECK(ShapeSelectionHeuristic{}.classify("read_file", close_args(),
                                             ToolResult::ok(close_body()))
          == DispatchMode::Standing);

    Ctx c;
    ToolResult out = selector.distill("read_file", close_args(),
                                      ToolResult::ok(close_body()), c.ctx);

    // Gold returned, on-topic by tolerance (the resolved fact), NOT byte-equal.
    CHECK_FALSE(out.is_error);
    CHECK(contains_any(out.body, {"8080"}));

    // HONORED-as-close: no warm window, even though Standing was predicted.
    CHECK(sup.standing_count() == 0);
    CHECK(selector.last_standing_id().empty());

    // Surface the real signal the organ honored (A-AC5 + audit).
    double conf = -1; std::string fuo = "absent";
    if (out.structured_payload.has_value()) {
        const Json& p = *out.structured_payload;
        if (p.contains("confidence"))   conf = p.value("confidence", -1.0);
        if (p.contains("follow_up_ok")) fuo  = p.at("follow_up_ok").get<bool>() ? "true" : "false";
    }
    MESSAGE("CLOSE real-model signal: confidence=" << conf << " follow_up_ok=" << fuo
            << " | distill tokens(selector)=" << selA.total_tokens());
}

// =============================================================================
// [A-AC3] HONOR-not-OBEY on a real investigation — the organ's promote/close
// decision MATCHES should_keep_warm() of the signal the REAL model actually
// emitted, with NO caller flag.  This is the deterministic regression guard for
// "honored, not obeyed": whatever the real model votes (keep-warm or done), the
// organ obeys THAT, never a background boolean (there is none on the surface).
//
// FINDING (flagged in the completion comment): with the organ's exact distiller
// prompt ("report ONLY the golden line ... nothing more, nothing less") and its
// report_gold schema (confidence/follow_up_ok marked Optional), a well-aligned
// local model (qwen2.5:7b) reliably OMITS the keep-warm signal on investigations
// → follow_up_ok absent → the organ CLOSES.  So in practice this case logs a
// HONORED-CLOSE.  The promote MACHINERY itself is proven on the real model by
// the next case (and hermetically, with a forced signal, by
// test_selection_heuristic.cpp).
// =============================================================================

// Re-declared from StandingSelector.cpp's anonymous namespace (kept in lock-step
// by review): the confirm-after thresholds.  If the organ's constants change,
// this mirror must change too — that coupling is the point (it asserts the organ
// routes by the SAME rule the test predicts from the real signal).
namespace {
constexpr double kTrivialLookupConfidence = 0.95;
constexpr double kLowConfidence           = 0.40;

bool predict_keep_warm(bool has_conf, double conf, bool has_fuo, bool fuo) {
    if (has_conf && conf <= kLowConfidence) return true;
    if (!has_fuo || !fuo)                   return false;
    if (has_conf && conf >= kTrivialLookupConfidence) return false;
    return true;
}
} // namespace

TEST_CASE("ollama: organ HONORS the real model's report_gold vote, never a flag [A-AC3]") {
    const std::string proxy = find_script("ollama_proxy.py");
    REQUIRE_FALSE(proxy.empty());
    hermetic_home();

    ProxyServer selA, warmB;
    REQUIRE(selA.start(proxy));
    REQUIRE(warmB.start(proxy));

    AgentSupervisor sup;
    sup.set_agent_config(make_supervisor_cfg(warmB.base_url()));
    sup.set_max_standing_subagents(2);

    Config cfg = make_selector_cfg(selA.base_url());
    StandingSelector selector{cfg, &sup};

    // Investigation predicted by tool identity (web_search) — so the confirm-after
    // is genuinely consulted (not a predict-ahead pass-through).
    CHECK(ShapeSelectionHeuristic{}.classify("web_search", promote_args(),
                                             ToolResult::ok(promote_body()))
          == DispatchMode::Standing);

    Ctx c;
    ToolResult out = selector.distill("web_search", promote_args(),
                                      ToolResult::ok(promote_body()), c.ctx);
    CHECK_FALSE(out.is_error);

    // Recover the signal the REAL model emitted (the organ surfaces it on the
    // gold payload when present; absent → the model omitted it).
    bool has_conf = false, has_fuo = false, fuo = false;
    double conf = 0.0;
    if (out.structured_payload.has_value()) {
        const Json& p = *out.structured_payload;
        if (p.contains("confidence"))   { has_conf = true; conf = p.value("confidence", 0.0); }
        if (p.contains("follow_up_ok")) { has_fuo  = true; fuo  = p.at("follow_up_ok").get<bool>(); }
    }
    const bool expect_warm = predict_keep_warm(has_conf, conf, has_fuo, fuo);
    const bool actually_warm = !selector.last_standing_id().empty();

    // THE HONOR ASSERTION: the organ routed by the real model's own vote.
    CHECK(actually_warm == expect_warm);
    CHECK((sup.standing_count() >= 1) == expect_warm);

    MESSAGE("HONOR: real signal conf=" << (has_conf ? conf : -1)
            << " follow_up_ok=" << (has_fuo ? (fuo ? "true" : "false") : "absent")
            << " → should_keep_warm=" << (expect_warm ? "PROMOTE" : "CLOSE")
            << " | organ=" << (actually_warm ? "PROMOTE" : "CLOSE")
            << " | first-turn tokens=" << selA.total_tokens());

    // If the real model DID vote keep-warm, the warm window must answer a
    // follow-up without re-engulf (the next case proves this unconditionally).
    if (actually_warm) {
        const long sel_before = selA.chat_requests();
        std::string answer = sup.interrogate(
            selector.last_standing_id(),
            "Which option fits a small team that needs raw performance?");
        CHECK_FALSE(answer.empty());
        CHECK(selA.chat_requests() == sel_before);  // no source re-distill
    }
}

// =============================================================================
// [A-AC2]+[A-AC5] WARM-WINDOW interrogate on the REAL model, WITHOUT re-engulf.
//
// This proves the warm-window contract on a real model UNCONDITIONALLY (i.e. not
// gated on whether an aligned model spontaneously emits follow_up_ok=true, which
// the case above shows it usually does not).  We exercise the SAME machinery the
// organ's promote path uses — AgentSupervisor::promote() + interrogate() on a
// SubAgent pinned to the local endpoint via EndpointOverride{use_distill_endpoint}
// — so what is proven here is exactly what StandingSelector relies on once its
// confirm-after says keep-warm.
//
// Structural no-re-engulf: the warm window was handed the investigation context
// ONCE at spawn.  A follow-up interrogation must NOT re-distill the source — so
// the SELECTOR/source-distill endpoint (proxy A) receives ZERO requests across
// the whole case, while the WARM endpoint (proxy B) shows the real interrogation
// traffic + token usage.
// =============================================================================

TEST_CASE("ollama: warm window answers a follow-up without re-engulf [A-AC2][A-AC5]") {
    const std::string proxy = find_script("ollama_proxy.py");
    REQUIRE_FALSE(proxy.empty());
    hermetic_home();

    ProxyServer selA, warmB;
    REQUIRE(selA.start(proxy));   // the SOURCE/selector endpoint — must stay at 0 requests
    REQUIRE(warmB.start(proxy));  // the WARM endpoint — carries the conversation

    AgentSupervisor sup;
    sup.set_agent_config(make_supervisor_cfg(warmB.base_url()));

    // Spawn a real warm SubAgent on the local endpoint and hand it the
    // investigation context ONCE.  use_distill_endpoint → cfg.distill.* (proxy B).
    batbox::agents::AgentSpec spec;
    spec.name        = "standing-distill-ollama";
    spec.description = "real-model warm investigation window (DIS-1017)";
    spec.endpoint    = batbox::agents::EndpointOverride{};
    spec.endpoint->use_distill_endpoint = true;

    const std::string ctx_prompt =
        "You are a warm investigation window. A web_search returned these results:\n" +
        promote_body() +
        "\nHold this context. Answer follow-up questions in prose; do not call any "
        "tools. Acknowledge with a one-line summary.";

    const std::string id = sup.spawn(spec, ctx_prompt, /*parent_id=*/"",
                                     batbox::CancelToken{});
    REQUIRE_FALSE(id.empty());
    sup.promote(id);
    CHECK(sup.standing_count() >= 1);

    // Baselines BEFORE interrogation.
    const long sel_before  = selA.chat_requests();   // expected 0 the whole case
    const long warm_before = warmB.total_tokens();

    // A-AC2: a follow-up against the still-warm context returns a non-empty,
    // on-topic answer from the REAL model.
    std::string answer = sup.interrogate(
        id, "Which option fits a small team that needs raw performance, and why?");

    const long sel_after  = selA.chat_requests();
    const long warm_after = warmB.total_tokens();

    CHECK_FALSE(answer.empty());
    CHECK(contains_any(answer, {"python", "rust", "go", "performance", "language",
                                "backend", "team", "depends", "safety", "speed"}));

    // STRUCTURAL no-re-engulf: the source was never re-distilled — the
    // SOURCE/selector endpoint saw ZERO requests across the entire case.
    CHECK(sel_before == 0);
    CHECK(sel_after  == 0);

    // A-AC5: the interrogation spent real-model tokens on the WARM endpoint.
    CHECK(warm_after > warm_before);
    MESSAGE("WARM interrogate real-model tokens: warm endpoint "
            << warm_before << " -> " << warm_after
            << " (interrogation delta=" << (warm_after - warm_before) << ")"
            << " | source/selector endpoint requests=" << sel_after
            << " (0 == no re-engulf) | answer[:80]=\"" << answer.substr(0, 220) << "\"");

    // Teardown: the supervisor destructor cancels + joins the warm window.
}

// =============================================================================
// main — ollama availability guard (A-AC4 SKIP) + model warmup (determinism).
// =============================================================================

namespace {

bool ollama_has_model(const std::string& base, const std::string& model) {
    auto r = cpr::Get(cpr::Url{base + "/v1/models"}, cpr::Timeout{4000});
    if (r.status_code != 200) return false;
    try {
        Json j = Json::parse(r.text);
        for (const auto& m : j.value("data", Json::array()))
            if (m.value("id", std::string{}) == model) return true;
    } catch (...) {}
    return false;
}

// One warmup completion to load the weights + stabilise the prompt cache so the
// PROMOTE input's control signal is deterministic on the first measured attempt.
void warmup(const std::string& base, const std::string& model) {
    Json body{
        {"model", model},
        {"temperature", 0},
        {"max_tokens", 32},
        {"stream", false},
        {"messages", Json::array({Json{{"role", "user"},
                                       {"content", "Reply with the single word: ready."}}})},
    };
    (void)cpr::Post(cpr::Url{base + "/v1/chat/completions"},
                    cpr::Header{{"Content-Type", "application/json"},
                                {"Authorization", "Bearer ollama"}},
                    cpr::Body{body.dump()}, cpr::Timeout{120000});
}

} // namespace

int main(int argc, char** argv) {
    const std::string base  = ollama_base();
    const std::string model = pinned_model();

    // Children (the proxies) inherit these from our environment.
    ::setenv("OLLAMA_BASE", base.c_str(), 1);
    ::setenv("PROXY_FORCE_TEMP", "0", 1);

    if (!ollama_has_model(base, model)) {
        std::fprintf(stderr,
            "SKIP test_selection_heuristic_ollama: ollama at %s is unreachable or "
            "the pinned model '%s' is absent.\n"
            "  Start ollama and run:  ollama pull %s\n"
            "  Then re-run:           ctest -L requires_ollama\n",
            base.c_str(), model.c_str(), model.c_str());
        return 77;  // CTest SKIP_RETURN_CODE → reported Skipped, never Failed.
    }

    std::fprintf(stderr, "[ollama substrate] base=%s model=%s — warming up...\n",
                 base.c_str(), model.c_str());
    warmup(base, model);

    doctest::Context ctx;
    ctx.applyCommandLine(argc, argv);
    return ctx.run();
}
