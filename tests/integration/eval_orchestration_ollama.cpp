// tests/integration/eval_orchestration_ollama.cpp
// =============================================================================
// DIS-1018 (DIS-1012 Child B) — Claude-vs-batbox SAME-MODEL orchestration eval.
//
// Cass's bar: "use ollama+Claude and ollama+batbox to run the same task, compare
// output and token usage ... they'll both be running the same model so it'll all
// be the harness doing the lifting."  The MODEL is held constant (one pinned
// ollama weight, qwen2.5:7b) so the ONLY variable is the ORCHESTRATION SHAPE.
//
// Two arms, identical model, identical token accounting:
//
//   * batbox arm  — the standing-window organ: a warm SubAgent is seeded with the
//     source ONCE (AgentSupervisor::spawn), promote()d, then interrogate()d for
//     each follow-up.  This is exactly the machinery DIS-1017 AC2 proved answers a
//     follow-up WITHOUT re-distilling the source.  For the lookup task the organ's
//     StandingSelector::distill() is driven directly and is expected to stay
//     CLOSED (single resolved fact) — proving the organ adds no warm-window
//     overhead on a flat lookup.
//
//   * Claude arm  — the definitional subagent-dispatch shape WITHOUT a standing
//     window: every follow-up question is a FRESH, stateless chat completion handed
//     the full raw source + the question.  This is what "driving subagents" means
//     when there is no warm-window organ to reuse — a new context each turn.  No
//     batbox organ is involved; it is a plain OpenAI-compatible call to the same
//     ollama model.
//
// TOKEN ACCOUNTING — DEFINED ONCE, APPLIED TO BOTH ARMS (the spec's hard rule):
//   each arm runs behind its OWN tests/fixtures/ollama_proxy.py instance.  The
//   proxy counts POST /v1/chat/completions and accumulates ollama's `usage`
//   token totals, exposed on GET /__stats.  "Total token usage" for an arm ==
//   that proxy's /__stats.total_tokens, summed across every model call the arm
//   made.  Same proxy, same field, both arms — so the delta is meaningful.  The
//   proxy also forces temperature:0 (determinism) for BOTH arms equally.
//
// TASK SET (B-AC5: >=1 investigation-class, >=1 lookup-class):
//   1. INVESTIGATION — a conflicting multi-source research body + two follow-up
//      questions.  Exercises the standing/interrogate organ (the novel thing).
//   2. LOOKUP — a single resolved fact buried in a config file + one question.
//      The organ should stay CLOSED; the eval checks batbox does not regress here.
//
// HONESTY NOTE (carried into the writeup): chat completions are stateless at the
// HTTP layer, so BOTH arms re-send context over the wire each turn.  This harness
// does not assume where the win is — it MEASURES the real per-arm token totals and
// reports whatever they are (win, lose, or wash).  The investigation also records
// per-turn token deltas so the mechanism (warm-history growth vs per-question
// re-engulf) is visible and the crossover point can be reasoned about.
//
// SKIP semantics (mirrors DIS-1017): main() probes ollama + the pinned model and
// returns 77 when either is absent; CTest SKIP_RETURN_CODE 77 → reported SKIPPED
// on a CI box with no GPU/ollama.  The hermetic suite is untouched.
//
// Artifacts: a markdown table + a CSV are written to ${BATBOX_EVAL_OUT}
// (default /tmp/batbox-dis1018-eval) AND echoed to stdout, for the writeup.
// =============================================================================

#include <batbox/agents/AgentSpec.hpp>
#include <batbox/agents/AgentSupervisor.hpp>
#include <batbox/config/Config.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/tools/SelectionHeuristic.hpp>
#include <batbox/tools/StandingSelector.hpp>
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
#include <fstream>
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
using batbox::agents::AgentSpec;
using batbox::agents::AgentSupervisor;
using batbox::agents::EndpointOverride;
using batbox::config::Config;
using batbox::tools::DispatchMode;
using batbox::tools::ShapeSelectionHeuristic;
using batbox::tools::StandingSelector;
using batbox::tools::ToolContext;
using batbox::tools::ToolResult;

// =============================================================================
// Environment knobs (documented + overridable for a second runner — B-AC1).
// =============================================================================

static std::string ollama_base() {
    const char* e = std::getenv("BATBOX_OLLAMA_BASE");
    return (e && *e) ? std::string(e) : std::string("http://127.0.0.1:11434");
}
static std::string pinned_model() {
    const char* e = std::getenv("BATBOX_OLLAMA_TEST_MODEL");
    return (e && *e) ? std::string(e) : std::string("qwen2.5:7b");
}
static std::string eval_out_dir() {
    const char* e = std::getenv("BATBOX_EVAL_OUT");
    return (e && *e) ? std::string(e) : std::string("/tmp/batbox-dis1018-eval");
}

// =============================================================================
// Fixture locator (mirrors test_selection_heuristic_ollama.cpp).
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
// Identical to the DIS-1017 substrate's fixture (counting + temp-0 proxy).
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
    Json stats() const {
        auto r = cpr::Get(cpr::Url{stats_url()}, cpr::Timeout{5000});
        if (r.status_code != 200) return Json::object();
        try { return Json::parse(r.text); } catch (...) { return Json::object(); }
    }
    long chat_requests() const { return stats().value("chat_requests", 0L); }
    long total_tokens()  const { return stats().value("total_tokens", 0L); }
};

// =============================================================================
// Config helpers (mirror the DIS-1017 substrate, pointed at one arm's proxy).
// =============================================================================

static Config make_supervisor_cfg(const std::string& warm_base) {
    Config cfg;
    cfg.api.base_url            = warm_base;
    cfg.api.api_key             = "ollama";
    cfg.api.default_model       = pinned_model();
    cfg.api.request_timeout_sec = 120;
    cfg.api.max_tokens          = 256;
    cfg.distill.enabled         = true;
    cfg.distill.base_url        = warm_base;
    cfg.distill.api_key         = "ollama";
    cfg.distill.model           = pinned_model();
    cfg.distill.request_timeout_sec = 120;
    cfg.distill.max_tokens      = 256;
    return cfg;
}

static Config make_selector_cfg(const std::string& distill_base) {
    Config cfg;
    cfg.distill.enabled                = true;
    cfg.distill.base_url               = distill_base;
    cfg.distill.api_key                = "ollama";
    cfg.distill.model                  = pinned_model();
    cfg.distill.max_tool_response_size = 100;
    cfg.distill.request_timeout_sec    = 120;
    cfg.distill.max_tokens             = 256;
    return cfg;
}

static void hermetic_home() {
    static const std::string home = [] {
        std::string h = "/tmp/batbox-dis1018-eval-home";
        fs::create_directories(h);
        return h;
    }();
    ::setenv("HOME", home.c_str(), 1);
}

// =============================================================================
// Text tolerances (keyword/substring — never byte-equality on a real model).
// =============================================================================

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
// Naive "Claude arm" subagent dispatch: one FRESH, stateless chat completion
// handed the full raw source + the question.  No warm window, no batbox organ.
// Returns the assistant message content (empty on failure).
// =============================================================================

static std::string naive_subagent_answer(const std::string& base_url,
                                          const std::string& source,
                                          const std::string& question) {
    Json body{
        {"model", pinned_model()},
        {"temperature", 0},
        {"max_tokens", 256},
        {"stream", false},
        {"messages", Json::array({
            Json{{"role", "system"},
                 {"content", "You are a focused research subagent. Answer the user's "
                             "question using ONLY the source material provided. Be concise."}},
            Json{{"role", "user"},
                 {"content", "--- BEGIN SOURCE ---\n" + source +
                             "\n--- END SOURCE ---\n\nQuestion: " + question}},
        })},
    };
    auto r = cpr::Post(cpr::Url{base_url + "/chat/completions"},
                       cpr::Header{{"Content-Type", "application/json"},
                                   {"Authorization", "Bearer ollama"}},
                       cpr::Body{body.dump()}, cpr::Timeout{120000});
    if (r.status_code != 200) return "";
    try {
        Json j = Json::parse(r.text);
        return j.at("choices").at(0).at("message").value("content", std::string{});
    } catch (...) { return ""; }
}

// =============================================================================
// Task material.
// =============================================================================

// INVESTIGATION source: a sizeable conflicting research body (no single answer),
// the shape a parent genuinely follows up on ("which fits MY use case?").
static std::string investigation_source() {
    return
        "Survey: choosing a backend language for a new service. Twelve sources, no consensus.\n"
        "Result 1 (blog, 2024): Python is best for rapid development — huge library ecosystem,\n"
        "  fast to prototype, easy hiring. Weakness: raw runtime performance and the GIL.\n"
        "Result 2 (forum): Rust wins decisively on raw performance and memory safety; zero-cost\n"
        "  abstractions, no GC pauses. Weakness: steep learning curve, slower initial development.\n"
        "Result 3 (Hacker News): Go is the pragmatic middle — good performance, trivial deploys,\n"
        "  great concurrency story, fast compile. Weakness: less expressive, verbose error handling.\n"
        "Result 4 (developer survey, n=4000): No clear winner; the right choice depends on team\n"
        "  expertise, latency budget, and time-to-market pressure more than the language itself.\n"
        "Result 5 (aggregator): Opinions conflict sharply. Performance-critical paths favour Rust;\n"
        "  iteration-speed-critical products favour Python; operationally-simple services favour Go.\n"
        "Result 6 (case study, fintech): migrated a hot path from Python to Rust, p99 latency fell\n"
        "  8x, but the rewrite took two engineers four months.\n"
        "Result 7 (case study, startup): shipped the MVP in Python in three weeks; would not have\n"
        "  hit the market window in any compiled language.\n"
        "Result 8 (benchmark): on a CPU-bound workload Rust ~ C, Go ~2-3x slower than Rust,\n"
        "  Python ~30-50x slower than Rust without native extensions.\n";
}
static std::vector<std::string> inv_questions_short() {
    return {
        "Which single option best fits a small team that needs raw runtime performance, and why?",
        "And if rapid development speed and time-to-market matter most instead, which fits best?",
    };
}
// Deeper variant: more follow-ups against the SAME warm context, to expose how
// the batbox-vs-naive token gap scales with conversation length.
static std::vector<std::string> inv_questions_deep() {
    return {
        "Which single option best fits a small team that needs raw runtime performance, and why?",
        "And if rapid development speed and time-to-market matter most instead, which fits best?",
        "Which option is the pragmatic middle ground for operational simplicity?",
        "Summarize the one-sentence tradeoff a tech lead should remember from all of this.",
    };
}
// Rubric keyword set per question index (tolerance, never byte-equality).
static bool inv_rubric(const std::vector<std::string>& answers) {
    if (answers.size() < 2) return false;
    bool ok = contains_any(answers[0], {"rust", "performance", "memory safety"}) &&
              contains_any(answers[1], {"python", "rapid", "prototype", "time-to-market", "speed"});
    if (answers.size() >= 3)
        ok = ok && contains_any(answers[2], {"go", "pragmatic", "middle", "concurrency", "deploy"});
    return ok;
}

// LOOKUP source: one resolved fact buried in a long config file (Standing by
// shape — many sections — so the organ's confirm-after is genuinely consulted
// and then stays CLOSED on the high-confidence single fact).
static std::string lookup_source() {
    std::string b = "# service configuration file\n";
    for (int i = 0; i < 60; ++i)
        b += "# default setting " + std::to_string(i) + " unrelated to the port\n";
    b += "listen_port = 8080\n";
    for (int i = 0; i < 60; ++i)
        b += "# trailing note " + std::to_string(i) + " irrelevant\n";
    return b;
}
static Json lookup_args() {
    return Json{{"path", "/etc/app.conf"},
                {"question", "What port does the service listen on?"}};
}
static std::string lookup_q() { return "What port does the service listen on?"; }

// =============================================================================
// Per-arm result record.
// =============================================================================

struct ArmResult {
    std::string arm;          // "batbox" | "claude"
    long  total_tokens = 0;
    long  chat_requests = 0;
    bool  rubric_pass = false;
    bool  ran_ok = false;     // orchestration produced non-empty output(s)
    std::string note;         // mechanism / per-turn breakdown
};

// =============================================================================
// INVESTIGATION task — both arms answer the SAME two follow-ups on the SAME
// source.  batbox: spawn warm window (source once) → interrogate q1 → q2.
// claude: fresh stateless dispatch per question (source re-sent each time).
// =============================================================================

static ArmResult run_investigation_batbox(const std::string& proxy_script,
                                          const std::vector<std::string>& questions) {
    ArmResult res; res.arm = "batbox";
    ProxyServer px;
    if (!px.start(proxy_script)) { res.note = "proxy start failed"; return res; }

    AgentSupervisor sup;
    sup.set_agent_config(make_supervisor_cfg(px.base_url()));

    AgentSpec spec;
    spec.name        = "standing-investigation";
    spec.description = "warm investigation window (DIS-1018 eval)";
    spec.endpoint    = EndpointOverride{};
    spec.endpoint->use_distill_endpoint = true;

    const std::string ctx_prompt =
        "You are a warm investigation window. A web_search returned the research below.\n" +
        investigation_source() +
        "\nHold this context. Answer follow-up questions in prose using ONLY this "
        "research; do not call any tools. Acknowledge with a one-line summary.";

    const std::string id = sup.spawn(spec, ctx_prompt, /*parent_id=*/"", CancelToken{});
    if (id.empty()) { res.note = "spawn failed"; return res; }
    sup.promote(id);

    std::vector<std::string> answers;
    std::string per_turn;
    long prev = px.total_tokens();
    bool all_nonempty = true;
    for (size_t i = 0; i < questions.size(); ++i) {
        std::string a = sup.interrogate(id, questions[i]);
        all_nonempty = all_nonempty && !a.empty();
        answers.push_back(a);
        long now = px.total_tokens();
        per_turn += " q" + std::to_string(i + 1) + "+=" + std::to_string(now - prev);
        prev = now;
    }

    res.total_tokens  = px.total_tokens();
    res.chat_requests = px.chat_requests();
    res.ran_ok        = all_nonempty;
    res.rubric_pass   = inv_rubric(answers);
    res.note = "per-turn tokens (incl. spawn turn in q1):" + per_turn +
               " | a1[:55]=\"" + (answers.empty() ? "" : answers[0].substr(0, 55)) + "\"";
    return res;
}

static ArmResult run_investigation_claude(const std::string& proxy_script,
                                          const std::vector<std::string>& questions) {
    ArmResult res; res.arm = "claude";
    ProxyServer px;
    if (!px.start(proxy_script)) { res.note = "proxy start failed"; return res; }

    const std::string src = investigation_source();
    std::vector<std::string> answers;
    std::string per_turn;
    long prev = px.total_tokens();
    bool all_nonempty = true;
    for (size_t i = 0; i < questions.size(); ++i) {
        std::string a = naive_subagent_answer(px.base_url(), src, questions[i]);
        all_nonempty = all_nonempty && !a.empty();
        answers.push_back(a);
        long now = px.total_tokens();
        per_turn += " q" + std::to_string(i + 1) + "=" + std::to_string(now - prev);
        prev = now;
    }

    res.total_tokens  = px.total_tokens();
    res.chat_requests = px.chat_requests();
    res.ran_ok        = all_nonempty;
    res.rubric_pass   = inv_rubric(answers);
    res.note = "per-turn tokens:" + per_turn +
               " | a1[:55]=\"" + (answers.empty() ? "" : answers[0].substr(0, 55)) + "\"";
    return res;
}

// =============================================================================
// LOOKUP task — batbox: StandingSelector::distill (expected to stay CLOSED);
// claude: one fresh dispatch.  Both answer the same single-fact question.
// =============================================================================

static ArmResult run_lookup_batbox(const std::string& proxy_script) {
    ArmResult res; res.arm = "batbox";
    ProxyServer px;
    if (!px.start(proxy_script)) { res.note = "proxy start failed"; return res; }
    hermetic_home();

    AgentSupervisor sup;
    sup.set_agent_config(make_supervisor_cfg(px.base_url()));
    Config cfg = make_selector_cfg(px.base_url());
    StandingSelector selector{cfg, &sup};

    CancelSource csrc;
    ToolContext tctx;
    tctx.cancel_token = csrc.token();

    ToolResult out = selector.distill("read_file", lookup_args(),
                                      ToolResult::ok(lookup_source()), tctx);

    res.total_tokens  = px.total_tokens();
    res.chat_requests = px.chat_requests();
    res.ran_ok        = !out.is_error && !out.body.empty();
    res.rubric_pass   = contains_any(out.body, {"8080"});
    const bool stayed_closed = (sup.standing_count() == 0) && selector.last_standing_id().empty();
    res.note = std::string("stayed_closed=") + (stayed_closed ? "yes" : "NO") +
               " standing_count=" + std::to_string(sup.standing_count()) +
               " | answer[:80]=\"" + out.body.substr(0, 80) + "\"";
    return res;
}

static ArmResult run_lookup_claude(const std::string& proxy_script) {
    ArmResult res; res.arm = "claude";
    ProxyServer px;
    if (!px.start(proxy_script)) { res.note = "proxy start failed"; return res; }

    std::string ans = naive_subagent_answer(px.base_url(), lookup_source(), lookup_q());
    res.total_tokens  = px.total_tokens();
    res.chat_requests = px.chat_requests();
    res.ran_ok        = !ans.empty();
    res.rubric_pass   = contains_any(ans, {"8080"});
    res.note = "answer[:80]=\"" + ans.substr(0, 80) + "\"";
    return res;
}

// =============================================================================
// Reporting.
// =============================================================================

struct TaskRow {
    std::string task_class;   // "investigation" | "lookup"
    std::string task_name;
    ArmResult   batbox;
    ArmResult   claude;
};

static std::string pct_delta(long batbox, long claude) {
    // token-spend delta of batbox RELATIVE TO claude: negative => batbox cheaper.
    if (claude == 0) return "n/a";
    double d = 100.0 * (double)(batbox - claude) / (double)claude;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%+.1f%%", d);
    return std::string(buf);
}

static void emit_report(const std::vector<TaskRow>& rows) {
    const std::string out = eval_out_dir();
    fs::create_directories(out);

    std::string md;
    md += "# DIS-1018 — batbox-vs-Claude same-model orchestration eval (results)\n\n";
    md += "Model held constant: `" + pinned_model() + "` (temperature 0, via counting "
          "ollama proxy). Token accounting identical for both arms: sum of "
          "`usage.total_tokens` over all model calls, read from the proxy's `/__stats`.\n\n";
    md += "| Task | Class | Arm | Outcome | Tokens | Model calls | Token Δ vs Claude |\n";
    md += "|------|-------|-----|---------|--------|-------------|-------------------|\n";

    std::string csv = "task,class,arm,outcome,tokens,model_calls\n";

    for (const auto& r : rows) {
        auto add = [&](const ArmResult& a) {
            std::string outcome = !a.ran_ok ? "ERROR" : (a.rubric_pass ? "PASS" : "fail-rubric");
            std::string delta = (a.arm == "batbox") ? pct_delta(a.total_tokens, r.claude.total_tokens) : "—";
            md += "| " + r.task_name + " | " + r.task_class + " | " + a.arm + " | " +
                  outcome + " | " + std::to_string(a.total_tokens) + " | " +
                  std::to_string(a.chat_requests) + " | " + delta + " |\n";
            csv += r.task_name + "," + r.task_class + "," + a.arm + "," + outcome + "," +
                   std::to_string(a.total_tokens) + "," + std::to_string(a.chat_requests) + "\n";
        };
        add(r.batbox);
        add(r.claude);
    }

    md += "\n## Per-arm mechanism notes\n\n";
    for (const auto& r : rows) {
        md += "**" + r.task_name + " [" + r.task_class + "]**\n";
        md += "- batbox: " + r.batbox.note + "\n";
        md += "- claude: " + r.claude.note + "\n\n";
    }

    std::ofstream(out + "/results.md")  << md;
    std::ofstream(out + "/results.csv") << csv;

    std::fprintf(stdout, "\n%s\n", md.c_str());
    std::fprintf(stdout, "[artifacts] %s/results.md  %s/results.csv\n",
                 out.c_str(), out.c_str());
}

// =============================================================================
// ollama availability guard + warmup (mirrors the DIS-1017 substrate).
// =============================================================================

static bool ollama_has_model(const std::string& base, const std::string& model) {
    auto r = cpr::Get(cpr::Url{base + "/v1/models"}, cpr::Timeout{4000});
    if (r.status_code != 200) return false;
    try {
        Json j = Json::parse(r.text);
        for (const auto& m : j.value("data", Json::array()))
            if (m.value("id", std::string{}) == model) return true;
    } catch (...) {}
    return false;
}

static void warmup(const std::string& base, const std::string& model) {
    Json body{
        {"model", model}, {"temperature", 0}, {"max_tokens", 32}, {"stream", false},
        {"messages", Json::array({Json{{"role", "user"},
                                       {"content", "Reply with the single word: ready."}}})},
    };
    (void)cpr::Post(cpr::Url{base + "/chat/completions"},
                    cpr::Header{{"Content-Type", "application/json"},
                                {"Authorization", "Bearer ollama"}},
                    cpr::Body{body.dump()}, cpr::Timeout{120000});
}

int main() {
    const std::string base  = ollama_base();
    const std::string model = pinned_model();

    ::setenv("OLLAMA_BASE", base.c_str(), 1);
    ::setenv("PROXY_FORCE_TEMP", "0", 1);

    const std::string proxy = find_script("ollama_proxy.py");
    if (proxy.empty()) {
        std::fprintf(stderr, "SKIP eval_orchestration_ollama: ollama_proxy.py fixture not found.\n");
        return 77;
    }
    if (!ollama_has_model(base, model)) {
        std::fprintf(stderr,
            "SKIP eval_orchestration_ollama: ollama at %s is unreachable or the pinned "
            "model '%s' is absent.\n  ollama pull %s ; then: ctest -L requires_ollama\n",
            base.c_str(), model.c_str(), model.c_str());
        return 77;
    }

    std::fprintf(stderr, "[eval] base=%s model=%s — warming up...\n", base.c_str(), model.c_str());
    warmup(base, model);

    std::vector<TaskRow> rows;

    std::fprintf(stderr, "[eval] investigation (2 follow-ups) — batbox arm...\n");
    TaskRow inv; inv.task_class = "investigation"; inv.task_name = "lang-choice-2q";
    inv.batbox = run_investigation_batbox(proxy, inv_questions_short());
    std::fprintf(stderr, "[eval] investigation (2 follow-ups) — claude arm...\n");
    inv.claude = run_investigation_claude(proxy, inv_questions_short());
    rows.push_back(inv);

    std::fprintf(stderr, "[eval] deep investigation (4 follow-ups) — batbox arm...\n");
    TaskRow invd; invd.task_class = "investigation"; invd.task_name = "lang-choice-4q-deep";
    invd.batbox = run_investigation_batbox(proxy, inv_questions_deep());
    std::fprintf(stderr, "[eval] deep investigation (4 follow-ups) — claude arm...\n");
    invd.claude = run_investigation_claude(proxy, inv_questions_deep());
    rows.push_back(invd);

    std::fprintf(stderr, "[eval] lookup task — batbox arm...\n");
    TaskRow lk; lk.task_class = "lookup"; lk.task_name = "config-port-lookup";
    lk.batbox = run_lookup_batbox(proxy);
    std::fprintf(stderr, "[eval] lookup task — claude arm...\n");
    lk.claude = run_lookup_claude(proxy);
    rows.push_back(lk);

    emit_report(rows);

    // Gate: every arm must have produced output (orchestration didn't crash).
    // Rubric quality is REPORTED, not gated — a fail-rubric is a data point about
    // the model, not a harness defect.
    bool all_ran = true;
    for (const auto& r : rows) {
        if (!r.batbox.ran_ok) { std::fprintf(stderr, "[eval] FAIL: %s batbox arm produced no output\n", r.task_name.c_str()); all_ran = false; }
        if (!r.claude.ran_ok) { std::fprintf(stderr, "[eval] FAIL: %s claude arm produced no output\n", r.task_name.c_str()); all_ran = false; }
    }
    return all_ran ? 0 : 1;
}
