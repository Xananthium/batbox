// tests/integration/test_subagent_distiller.cpp
// =============================================================================
// Integration tests for batbox::tools::SubagentDistiller + the S1+S4 closed
// tool-subagent wired through the real ToolRegistry::dispatch envelope (DIS-980).
//
// Hermetic: drives a fake LOCAL OpenAI-compatible endpoint
// (tests/fixtures/fake_distill_server.py) — no GPU, no network. The fixture's
// --mode selects which report_gold contract behaviour it returns so every
// robustness path is exercised deterministically.
//
// Coverage:
//   [AC3] gold path: a too-big result is engulfed into a one-shot call on the
//         LOCAL endpoint, the model is forced to emit via report_gold, and the
//         distilled gold is returned as the ToolResult.
//   [AC4] closed lifecycle: the call is one-shot (no standing state retained);
//         follow_up_ok is captured into structured_payload but not acted upon.
//   [AC5] robustness: unreachable endpoint / HTTP 500 / no report_gold call /
//         wrong tool / cancellation → distiller returns the ORIGINAL result.
//   [AC6] installed into the real ToolRegistry envelope (set_decider/set_distiller);
//         a huge dispatched result is distilled end-to-end through dispatch().
//   [AC7] no-regression: distillation disabled → dispatch is byte-identical to
//         S7 (no engulf); a below-threshold result is never engulfed.
//
// Build standalone (no CMake, from repo root; x64-linux triplet). Exact link set
// is reproduced in the DIS-980 handoff comment.
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/config/Config.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/tools/ITool.hpp>
#include <batbox/tools/ReportGoldTool.hpp>
#include <batbox/tools/SubagentDistiller.hpp>
#include <batbox/tools/ThresholdEngulfDecider.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolRegistry.hpp>
#include <batbox/tools/ToolResult.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using batbox::CancelSource;
using batbox::CancelToken;
using batbox::Json;
using batbox::config::Config;
using batbox::tools::install_subagent_distillation;
using batbox::tools::SubagentDistiller;
using batbox::tools::ToolContext;
using batbox::tools::ToolRegistry;
using batbox::tools::ToolResult;

// =============================================================================
// Locate fake_distill_server.py
// =============================================================================

static std::string find_fixture_script() {
#ifdef BATBOX_FIXTURE_DIR
    fs::path p = fs::path(BATBOX_FIXTURE_DIR) / "fake_distill_server.py";
    if (fs::exists(p)) return p.string();
#endif
    fs::path dir = fs::current_path();
    for (int depth = 0; depth < 8; ++depth) {
        fs::path candidate = dir / "tests" / "fixtures" / "fake_distill_server.py";
        if (fs::exists(candidate)) return candidate.string();
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    return "";
}

// =============================================================================
// FakeServer RAII — spawns the fixture in a given --mode, reads READY <port>.
// =============================================================================

struct FakeServer {
    pid_t pid{-1};
    int   port{0};
    FILE* stdout_pipe{nullptr};

    bool start(const std::string& script_path, const std::string& mode) {
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
            const char* argv[] = {"python3", script_path.c_str(),
                                  "--mode", mode.c_str(), nullptr};
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
// Helpers
// =============================================================================

// A distill config that points the LOCAL distill endpoint at base_url. Note
// cfg.api is left at its default (the cloud endpoint) ON PURPOSE — the distiller
// must hit cfg.distill.*, proving the endpoint separation.
static Config make_distill_cfg(const std::string& base_url, std::size_t threshold = 100) {
    Config cfg;
    cfg.distill.enabled                = true;
    cfg.distill.base_url               = base_url;
    cfg.distill.api_key                = "test-key-123";
    cfg.distill.model                  = "fake-distill-model";
    cfg.distill.max_tool_response_size = threshold;
    cfg.distill.request_timeout_sec    = 10;
    cfg.distill.max_tokens             = 256;
    return cfg;
}

struct Ctx {
    CancelSource src;
    ToolContext  ctx;
    Ctx() { ctx.cancel_token = src.token(); }
};

// A tool whose body is a fixed size — used to exercise dispatch + envelope.
class BigStubTool final : public batbox::tools::ITool {
public:
    BigStubTool(std::string n, std::size_t body_size)
        : name_(std::move(n)), body_(body_size, 'x') {}
    [[nodiscard]] std::string_view name()        const override { return name_; }
    [[nodiscard]] std::string_view description() const override { return "big stub"; }
    [[nodiscard]] Json schema_json() const override {
        return Json{{"name", name_}, {"description", "big stub"},
                    {"parameters", Json{{"type", "object"},
                                        {"properties", Json::object()},
                                        {"required", Json::array()}}}};
    }
    [[nodiscard]] ToolResult run(const Json&, ToolContext&) override {
        return ToolResult::ok(body_);
    }
    [[nodiscard]] bool is_read_only()          const override { return true; }
    [[nodiscard]] bool requires_confirmation() const override { return false; }
private:
    std::string name_;
    std::string body_;
};

static const std::string kGold = "DISTILLED_GOLD_LINE";

// =============================================================================
// [AC3] gold path — distilled gold returned; endpoint separation proven.
// =============================================================================

TEST_CASE("distiller: gold path returns the distilled golden line [AC3]") {
    std::string script = find_fixture_script();
    REQUIRE_FALSE(script.empty());

    FakeServer srv;
    REQUIRE(srv.start(script, "gold"));

    Config cfg = make_distill_cfg(srv.base_url());
    SubagentDistiller distiller{cfg};

    Ctx c;
    const std::string big(500, 'x');
    ToolResult out = distiller.distill("read_file", Json{{"path", "/big"}},
                                       ToolResult::ok(big), c.ctx);

    CHECK_FALSE(out.is_error);
    CHECK(out.body == kGold);
    REQUIRE(out.structured_payload.has_value());
    const Json& p = *out.structured_payload;
    CHECK(p.at("distilled") == true);
    CHECK(p.at("original_bytes") == big.size());
    CHECK(p.at("confidence") == doctest::Approx(0.91));
}

// =============================================================================
// [AC4] closed lifecycle — follow_up_ok captured, not acted upon; one-shot.
// =============================================================================

TEST_CASE("distiller: follow_up_ok captured into payload, no standing state [AC4]") {
    std::string script = find_fixture_script();
    REQUIRE_FALSE(script.empty());

    FakeServer srv;
    REQUIRE(srv.start(script, "gold"));

    Config cfg = make_distill_cfg(srv.base_url());
    SubagentDistiller distiller{cfg};
    Ctx c;

    ToolResult a = distiller.distill("grep", Json::object(),
                                     ToolResult::ok(std::string(500, 'a')), c.ctx);
    // follow_up_ok is recorded (the fixture reports true) but inert here.
    REQUIRE(a.structured_payload.has_value());
    CHECK(a.structured_payload->at("follow_up_ok") == true);

    // Closed: a second distill is fully independent (no retained window). The
    // distiller holds no per-call state, so the same instance distills again.
    ToolResult b = distiller.distill("grep", Json::object(),
                                     ToolResult::ok(std::string(800, 'b')), c.ctx);
    CHECK(b.body == kGold);
    REQUIRE(b.structured_payload.has_value());
    CHECK(b.structured_payload->at("original_bytes") == 800);
}

// =============================================================================
// [AC5] robustness — every failure path falls back to the ORIGINAL result.
// =============================================================================

TEST_CASE("distiller: unreachable endpoint → original result [AC5]") {
    std::string script = find_fixture_script();
    REQUIRE_FALSE(script.empty());

    // Start then stop a server to obtain a now-closed port.
    int dead_port = 0;
    {
        FakeServer s;
        REQUIRE(s.start(script, "gold"));
        dead_port = s.port;
    }
    Config cfg = make_distill_cfg("http://127.0.0.1:" + std::to_string(dead_port) + "/v1");
    cfg.distill.request_timeout_sec = 3;  // fail fast
    SubagentDistiller distiller{cfg};

    Ctx c;
    const std::string big(500, 'x');
    ToolResult out = distiller.distill("read_file", Json::object(),
                                       ToolResult::ok(big), c.ctx);
    CHECK_FALSE(out.is_error);
    CHECK(out.body == big);                       // data preserved intact
    CHECK_FALSE(out.structured_payload.has_value());
}

TEST_CASE("distiller: HTTP 500 → original result [AC5]") {
    std::string script = find_fixture_script();
    REQUIRE_FALSE(script.empty());
    FakeServer srv;
    REQUIRE(srv.start(script, "error"));

    Config cfg = make_distill_cfg(srv.base_url());
    SubagentDistiller distiller{cfg};
    Ctx c;
    const std::string big(500, 'x');
    ToolResult out = distiller.distill("read_file", Json::object(),
                                       ToolResult::ok(big), c.ctx);
    CHECK(out.body == big);
    CHECK_FALSE(out.structured_payload.has_value());
}

TEST_CASE("distiller: report_gold never called → original result [AC5]") {
    std::string script = find_fixture_script();
    REQUIRE_FALSE(script.empty());
    FakeServer srv;
    REQUIRE(srv.start(script, "notool"));

    Config cfg = make_distill_cfg(srv.base_url());
    SubagentDistiller distiller{cfg};
    Ctx c;
    const std::string big(500, 'x');
    ToolResult out = distiller.distill("read_file", Json::object(),
                                       ToolResult::ok(big), c.ctx);
    CHECK(out.body == big);
    CHECK_FALSE(out.structured_payload.has_value());
}

TEST_CASE("distiller: wrong tool call → original result [AC5]") {
    std::string script = find_fixture_script();
    REQUIRE_FALSE(script.empty());
    FakeServer srv;
    REQUIRE(srv.start(script, "wrongtool"));

    Config cfg = make_distill_cfg(srv.base_url());
    SubagentDistiller distiller{cfg};
    Ctx c;
    const std::string big(500, 'x');
    ToolResult out = distiller.distill("read_file", Json::object(),
                                       ToolResult::ok(big), c.ctx);
    CHECK(out.body == big);
    CHECK_FALSE(out.structured_payload.has_value());
}

TEST_CASE("distiller: pre-cancelled context → original result, endpoint untouched [AC5]") {
    std::string script = find_fixture_script();
    REQUIRE_FALSE(script.empty());
    FakeServer srv;
    REQUIRE(srv.start(script, "gold"));

    Config cfg = make_distill_cfg(srv.base_url());
    SubagentDistiller distiller{cfg};

    Ctx c;
    c.src.request_stop();  // cancelled before distillation
    const std::string big(500, 'x');
    ToolResult out = distiller.distill("read_file", Json::object(),
                                       ToolResult::ok(big), c.ctx);
    CHECK(out.body == big);                       // returns original
    CHECK_FALSE(out.structured_payload.has_value());
}

// =============================================================================
// [AC6] installed into the real ToolRegistry envelope; distilled via dispatch().
// =============================================================================

TEST_CASE("dispatch: huge result distilled end-to-end through the envelope [AC6]") {
    std::string script = find_fixture_script();
    REQUIRE_FALSE(script.empty());
    FakeServer srv;
    REQUIRE(srv.start(script, "gold"));

    ToolRegistry reg;
    reg.register_tool(std::make_unique<BigStubTool>("big_tool", 500));

    Config cfg = make_distill_cfg(srv.base_url(), /*threshold=*/100);
    install_subagent_distillation(reg, cfg);

    Ctx c;
    auto r = reg.dispatch("big_tool", Json::object(), c.ctx);
    REQUIRE(r.has_value());
    CHECK(r.value().body == kGold);  // distilled through dispatch + S7 seam
}

// =============================================================================
// [AC7] no-regression — disabled = identical to S7; below-threshold not engulfed.
// =============================================================================

TEST_CASE("dispatch: distillation disabled → pass-through (S7-identical) [AC7]") {
    // No server needed: nothing should be engulfed.
    ToolRegistry reg;
    reg.register_tool(std::make_unique<BigStubTool>("big_tool", 5000));

    Config cfg = make_distill_cfg("http://127.0.0.1:9/v1");
    cfg.distill.enabled = false;             // disabled
    install_subagent_distillation(reg, cfg); // no-op

    Ctx c;
    auto r = reg.dispatch("big_tool", Json::object(), c.ctx);
    REQUIRE(r.has_value());
    CHECK(r.value().body.size() == 5000);    // unchanged, never distilled
    CHECK_FALSE(r.value().structured_payload.has_value());
}

TEST_CASE("dispatch: below-threshold result is not engulfed [AC7]") {
    std::string script = find_fixture_script();
    REQUIRE_FALSE(script.empty());
    FakeServer srv;
    REQUIRE(srv.start(script, "gold"));

    ToolRegistry reg;
    reg.register_tool(std::make_unique<BigStubTool>("small_tool", 50));

    Config cfg = make_distill_cfg(srv.base_url(), /*threshold=*/100);
    install_subagent_distillation(reg, cfg);

    Ctx c;
    auto r = reg.dispatch("small_tool", Json::object(), c.ctx);
    REQUIRE(r.has_value());
    CHECK(r.value().body.size() == 50);      // small → inlined unchanged
    CHECK(r.value().body != kGold);
}
