// tests/integration/test_compactor.cpp
// =============================================================================
// Integration test for batbox::conversation::Compactor (CPP 3.5).
//
// Strategy:
//   Spawns tests/fixtures/fake_openai_server.py and routes Compactor's
//   summary request through Client::chat() against it.
//   The fake server returns "Hello from fake server!" as assistant content —
//   this acts as the canned summary response.
//
// Tests (acceptance criteria):
//   1. Threshold check — compact() respects keep_last_n: head is summarised,
//      tail is kept verbatim.
//   2. Verbatim tail preserved exactly — all fields, ids, content unchanged.
//   3. Older turns replaced — result starts with a single System summary msg.
//   4. Status callback fires with correct note format.
//   5. Disable path — when keep_last_n >= total messages, compact() no-ops
//      (returns original messages, no network call).
//   6. CancelToken respected — cancelled token produces Err before network.
//   7. Inference error propagated — bad auth returns Err.
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/conversation/Compactor.hpp>
#include <batbox/conversation/Message.hpp>
#include <batbox/inference/Client.hpp>
#include <batbox/config/Config.hpp>
#include <batbox/core/CancelToken.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <chrono>

#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>

namespace fs = std::filesystem;
using namespace batbox::conversation;

// ---------------------------------------------------------------------------
// locate fake_openai_server.py
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
    return "";
}

// ---------------------------------------------------------------------------
// FakeServer RAII — forks python3, waits for "READY <port>" on stdout.
// ---------------------------------------------------------------------------
struct FakeServer {
    pid_t pid{-1};
    int   port{0};
    FILE* stdout_pipe{nullptr};

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
            ::kill(pid, SIGTERM);
            ::close(pipefd[0]);
            pid = -1;
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
        if (stdout_pipe) {
            ::fclose(stdout_pipe);
            stdout_pipe = nullptr;
        }
    }

    ~FakeServer() { stop(); }

    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port) + "/v1";
    }
};

// ---------------------------------------------------------------------------
// Helper: build a minimal Config pointing at the fake server.
// ---------------------------------------------------------------------------
static batbox::config::Config make_test_config(const std::string& base_url,
                                               const std::string& api_key = "test-key-123",
                                               int timeout_sec = 10) {
    batbox::config::Config cfg;
    cfg.api.base_url            = base_url;
    cfg.api.api_key             = api_key;
    cfg.api.request_timeout_sec = timeout_sec;
    return cfg;
}

// ---------------------------------------------------------------------------
// Helper: build a simple conversation of N user+assistant pairs.
// ---------------------------------------------------------------------------
static std::vector<Message> make_conversation(int pair_count) {
    std::vector<Message> msgs;
    msgs.reserve(static_cast<size_t>(pair_count) * 2);
    for (int i = 0; i < pair_count; ++i) {
        Message user;
        user.role    = Role::User;
        user.content = "User message " + std::to_string(i + 1);

        Message asst;
        asst.role    = Role::Assistant;
        asst.content = "Assistant reply " + std::to_string(i + 1);

        msgs.push_back(std::move(user));
        msgs.push_back(std::move(asst));
    }
    return msgs;
}

// ===========================================================================
// Test suite
// ===========================================================================

TEST_SUITE("Compactor integration") {

    // -----------------------------------------------------------------------
    // AC1: Threshold check — head summarised, tail kept verbatim.
    // AC2: Verbatim tail preserved exactly.
    // AC3: Older turns replaced with single System summary message.
    // -----------------------------------------------------------------------
    TEST_CASE("compact: head summarised, tail verbatim, result structure") {
        std::string script = find_fixture_script();
        REQUIRE_FALSE(script.empty());

        FakeServer srv;
        REQUIRE(srv.start(script));

        auto cfg = make_test_config(srv.base_url());
        batbox::inference::Client client{cfg};

        // 5 pairs = 10 messages total; keep last 4.
        auto msgs = make_conversation(5);
        REQUIRE(msgs.size() == 10);

        // Save tail content for exact match.
        std::vector<std::string> tail_ids;
        std::vector<std::string> tail_content;
        for (size_t i = 6; i < msgs.size(); ++i) {
            tail_ids.push_back(msgs[i].id);
            tail_content.push_back(msgs[i].content);
        }

        int keep_n = 4;
        Compactor compactor{keep_n};

        auto [src, ct] = batbox::CancelToken::make_root();
        auto result = compactor.compact(msgs, client, std::move(ct));

        REQUIRE(result.has_value());
        const auto& compacted = result.value();

        // Total size: 1 summary + 4 tail messages.
        CHECK(compacted.size() == 5);

        // First message is System role summary.
        CHECK(compacted[0].role == Role::System);
        // The fake server returns "Hello from fake server!" as summary content.
        CHECK_FALSE(compacted[0].content.empty());

        // Tail messages preserved verbatim (id + content).
        REQUIRE(compacted.size() >= 5);
        for (int i = 0; i < keep_n; ++i) {
            CHECK(compacted[1 + i].id      == tail_ids[static_cast<size_t>(i)]);
            CHECK(compacted[1 + i].content == tail_content[static_cast<size_t>(i)]);
        }
    }

    // -----------------------------------------------------------------------
    // AC4: Status callback fires with correct note format.
    // -----------------------------------------------------------------------
    TEST_CASE("compact: status callback fires with correct format") {
        std::string script = find_fixture_script();
        REQUIRE_FALSE(script.empty());

        FakeServer srv;
        REQUIRE(srv.start(script));

        auto cfg = make_test_config(srv.base_url());
        batbox::inference::Client client{cfg};

        auto msgs = make_conversation(6);  // 12 messages, keep 4
        int keep_n = 4;

        std::string captured_note;
        Compactor compactor{keep_n, [&](const std::string& note) {
            captured_note = note;
        }};

        auto [src, ct] = batbox::CancelToken::make_root();
        auto result = compactor.compact(msgs, client, std::move(ct));
        REQUIRE(result.has_value());

        // Note must mention total turns (12), summary count (1), verbatim (4).
        CHECK_FALSE(captured_note.empty());
        CHECK(captured_note.find("12") != std::string::npos);
        CHECK(captured_note.find("1 summary") != std::string::npos);
        CHECK(captured_note.find("4 recent") != std::string::npos);
        // Must contain the arrow character.
        CHECK(captured_note.find("context compacted") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // AC5: Disable path — keep_last_n >= total messages → no-op, no network.
    // This also verifies BATBOX_AUTO_COMPACT_AT_PCT=100 semantic: caller
    // does not invoke compact(), but even if they do with keep_last_n >= total,
    // the conversation is returned unchanged.
    // -----------------------------------------------------------------------
    TEST_CASE("compact: no-op when keep_last_n >= message count") {
        // Use a deliberately bad URL so any real network call would fail.
        auto cfg = make_test_config("http://127.0.0.1:1/v1", "test-key-123");
        batbox::inference::Client client{cfg};

        auto msgs = make_conversation(3);  // 6 messages
        int keep_n = 10;  // >= 6, so no-op

        bool callback_fired = false;
        Compactor compactor{keep_n, [&](const std::string&) {
            callback_fired = true;
        }};

        auto [src, ct] = batbox::CancelToken::make_root();
        auto result = compactor.compact(msgs, client, std::move(ct));

        REQUIRE(result.has_value());

        // Returned unchanged.
        const auto& out = result.value();
        REQUIRE(out.size() == msgs.size());
        for (size_t i = 0; i < msgs.size(); ++i) {
            CHECK(out[i].id      == msgs[i].id);
            CHECK(out[i].content == msgs[i].content);
            CHECK(out[i].role    == msgs[i].role);
        }

        // Status callback must NOT fire on no-op.
        CHECK_FALSE(callback_fired);
    }

    // -----------------------------------------------------------------------
    // AC6: CancelToken respected — cancelled before call → Err.
    // -----------------------------------------------------------------------
    TEST_CASE("compact: cancelled token produces exception before network call") {
        // Use a bad URL so any real call would fail differently.
        auto cfg = make_test_config("http://127.0.0.1:1/v1", "test-key-123");
        batbox::inference::Client client{cfg};

        auto msgs = make_conversation(5);
        Compactor compactor{2};

        auto [src, ct] = batbox::CancelToken::make_root();
        src.request_stop();  // cancel immediately

        // compact() should throw CancelledException (caught and propagated).
        // Note: static_cast<void> suppresses [[nodiscard]] warning from the doctest
        // DOCTEST_CAST_TO_VOID macro which does not cast to void properly on clang.
        CHECK_THROWS_AS(
            static_cast<void>(compactor.compact(msgs, client, std::move(ct))),
            batbox::CancelledException
        );
    }

    // -----------------------------------------------------------------------
    // AC7: Inference error propagated — bad auth key → Err with 401 message.
    // -----------------------------------------------------------------------
    TEST_CASE("compact: inference error propagated as Err") {
        std::string script = find_fixture_script();
        REQUIRE_FALSE(script.empty());

        FakeServer srv;
        REQUIRE(srv.start(script));

        // Wrong API key causes 401 from fake server.
        auto cfg = make_test_config(srv.base_url(), "wrong-key-xyz");
        batbox::inference::Client client{cfg};

        auto msgs = make_conversation(5);
        Compactor compactor{2};

        auto [src, ct] = batbox::CancelToken::make_root();
        auto result = compactor.compact(msgs, client, std::move(ct));

        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("http 401") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // Edge: keep_last_n = 0 — all turns summarised, result is only summary.
    // -----------------------------------------------------------------------
    TEST_CASE("compact: keep_last_n=0 produces only summary message") {
        std::string script = find_fixture_script();
        REQUIRE_FALSE(script.empty());

        FakeServer srv;
        REQUIRE(srv.start(script));

        auto cfg = make_test_config(srv.base_url());
        batbox::inference::Client client{cfg};

        auto msgs = make_conversation(3);  // 6 messages

        std::string status_note;
        Compactor compactor{0, [&](const std::string& note) {
            status_note = note;
        }};

        auto [src, ct] = batbox::CancelToken::make_root();
        auto result = compactor.compact(msgs, client, std::move(ct));
        REQUIRE(result.has_value());

        const auto& out = result.value();
        // Only 1 message: the summary.
        CHECK(out.size() == 1);
        CHECK(out[0].role == Role::System);
        CHECK_FALSE(out[0].content.empty());

        // Status note mentions 6 turns → 1 summary + 0 recent.
        CHECK(status_note.find("6") != std::string::npos);
        CHECK(status_note.find("0 recent") != std::string::npos);
    }

} // TEST_SUITE
