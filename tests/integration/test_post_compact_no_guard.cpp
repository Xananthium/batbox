// tests/integration/test_post_compact_no_guard.cpp
// =============================================================================
// Regression test for PEXT2 1.1: verifies that after successful compaction,
// run_turn() proceeds to inference rather than returning Err from a
// post-compact threshold re-check (which was an unauthorized PEXT 4.1 addition).
//
// Strategy:
//   Drive a 200-message Conversation with auto_compact_at_pct=80 and a small
//   explicit context window (5000 tokens, set via default_model_ctx_len which
//   takes priority over the ContextWindow table when > 4096) so compaction is
//   forced.  With keep_last_n_turns_verbatim=2, Compactor summarises the head
//   and keeps only the last 2 messages verbatim.  After compaction, run_turn()
//   must proceed to inference (FakeServer streams a valid stop response) and
//   return Ok.
//
//   If the unauthorized post-compact guard were still present, run_turn() would
//   return Err("ctx-budget: prompt still exceeds compact threshold after
//   compaction; consider /clear or a larger-ctx model") because the post-compact
//   request (sys_prompt + 2 verbatim + summary ~375 tokens) can exceed the
//   threshold when that guard is present.  With the guard removed, the wire
//   call determines whether the context actually overflows; the fake server
//   always returns Ok.
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/conversation/Conversation.hpp>
#include <batbox/conversation/Message.hpp>
#include <batbox/config/Config.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/core/Uuid.hpp>
#include <batbox/inference/Client.hpp>
#include <batbox/session/SessionStore.hpp>
#include <batbox/tools/ToolRegistry.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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
// FakeServer RAII — forks python3, waits for "READY <port>" on stdout.
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
// TmpDir RAII — creates and cleans up a temporary directory.
// =============================================================================

struct TmpDir {
    fs::path path;

    TmpDir() {
        auto base = fs::temp_directory_path()
                    / ("batbox_pcng_test_" + batbox::Uuid::v4().to_string());
        fs::create_directories(base);
        path = base;
    }

    ~TmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// =============================================================================
// Helper: build a minimal Config pointing at a given base_url.
// =============================================================================

static batbox::config::Config make_test_config(const std::string& base_url,
                                                const std::string& api_key = "test-key-123",
                                                int timeout_sec = 10) {
    batbox::config::Config cfg;
    cfg.api.base_url            = base_url;
    cfg.api.api_key             = api_key;
    cfg.api.request_timeout_sec = timeout_sec;
    cfg.api.default_model       = "gpt-4o";
    cfg.api.max_tokens          = 512;
    cfg.api.temperature         = 0.7;
    cfg.api.top_p               = 1.0;
    cfg.compact.auto_compact_at_pct        = 80;
    cfg.compact.keep_last_n_turns_verbatim = 4;
    return cfg;
}

// =============================================================================
// Test suite
// =============================================================================

TEST_SUITE("post_compact_no_guard regression") {

    // -------------------------------------------------------------------------
    // PEXT2-1.1-RT1: 200-message conversation with pct=80 compacts successfully
    // and run_turn() returns Ok (no Err from post-compact threshold guard).
    //
    // Setup:
    //   - ctx_len = 5000 (> 4096 so takes priority over ContextWindow table)
    //   - pct = 80  =>  threshold = ceil(5000*80/100) = 4000 tokens
    //   - 200 messages * ~100 bytes each = ~20000 bytes / 4 = ~5000 tokens
    //   - sys_prompt ~1500 bytes / 4 = ~375 tokens
    //   - pre-compact est ~5375 tokens > 4000  =>  compaction fires
    //   - keep_last_n = 2  =>  Compactor keeps last 2 turns verbatim + 1 summary
    //   - FakeServer handles the non-streaming summary request and the
    //     streaming main-turn request, both returning valid responses.
    // -------------------------------------------------------------------------
    TEST_CASE("run_turn proceeds after compaction with pct=80 and keep_last_n=2") {
        std::string script = find_fixture_script();
        REQUIRE_FALSE(script.empty());

        FakeServer srv;
        REQUIRE(srv.start(script));

        TmpDir tmp;
        auto cfg = make_test_config(srv.base_url());

        // Force a small explicit context window so pct=80 produces a low threshold.
        // default_model_ctx_len = 5000 (> 4096) causes the ctx-budget code to use
        // this value directly rather than the ContextWindow table (which returns
        // 128000 for gpt-4o).  Threshold = ceil(5000 * 80 / 100) = 4000 tokens.
        // 200 messages of 100 chars each: 20000 bytes / 4 = 5000 tokens, plus
        // sys_prompt ~375 tokens = ~5375 tokens total > 4000 threshold.
        cfg.api.default_model_ctx_len          = 5000;
        cfg.compact.auto_compact_at_pct        = 80;
        cfg.compact.keep_last_n_turns_verbatim = 2;

        batbox::inference::Client      client{cfg};
        batbox::session::SessionStore  store{tmp.path};

        Conversation conv{client, store, cfg, tmp.path};

        // Build 200 messages of ~100 chars each.  Total: ~20000 bytes / 4 = ~5000 tokens.
        // Combined with the system prompt (~375 tokens), the pre-compact estimate
        // (~5375 tokens) exceeds the 4000-token threshold, so compaction fires.
        for (int i = 0; i < 200; ++i) {
            conv.user_message("This is test message number " + std::to_string(i) +
                              " with extra content to reliably exceed the compaction threshold.");
        }

        const int count_before = static_cast<int>(conv.messages().size());
        CHECK(count_before == 200);

        auto [src, tok] = CancelToken::make_root();
        auto res = conv.run_turn(std::move(tok));

        // run_turn() MUST succeed.  If the unauthorized post-compact Err guard
        // were present, this would fail with "ctx-budget: prompt still exceeds
        // compact threshold after compaction; consider /clear or a larger-ctx model".
        REQUIRE_MESSAGE(res.has_value(),
            "run_turn returned Err: " << (res.has_value() ? std::string{} : res.error()));

        // After compaction: 1 summary + 2 verbatim + 1 assistant reply = 4 messages.
        // Without compaction it would be 201 (200 history + 1 assistant reply).
        const int count_after = static_cast<int>(conv.messages().size());
        CHECK(count_after < count_before);
    }

} // TEST_SUITE
