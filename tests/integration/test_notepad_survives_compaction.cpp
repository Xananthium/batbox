// tests/integration/test_notepad_survives_compaction.cpp
// =============================================================================
// Integration test (DIS-981, S6, AC3 — the decisive property):
//
//   The notepad survives compaction.
//
// The Compactor rewrites ONLY the message array (LLM summary + verbatim tail).
// The notepad lives OUT-OF-BAND (a disk file via NotepadStore — never a
// Message), so a real compaction pass cannot touch it.  That is exactly the
// property that makes aggressive context-trimming safe: you can prune raw tool
// output from the conversation and the gold you jotted to the pad persists.
//
// This test runs the REAL Compactor against the shared fake OpenAI server
// (proving messages are actually compacted — head replaced by a summary, tail
// kept verbatim) and then asserts the pad is byte-identical before and after.
//
// Build standalone (from repo root, x64-linux triplet):
//   c++ -std=c++20 -I include -I build/vcpkg_installed/x64-linux/include \
//       -DBATBOX_FIXTURE_DIR=\"$PWD/tests/fixtures\" \
//       tests/integration/test_notepad_survives_compaction.cpp \
//       src/tools/NotepadStore.cpp \
//       src/conversation/Compactor.cpp src/conversation/Message.cpp \
//       src/inference/Client.cpp src/inference/ChatRequest.cpp \
//       src/core/Uuid.cpp src/core/Paths.cpp src/core/Json.cpp \
//       src/core/CancelToken.cpp src/core/Logging.cpp \
//       build/vcpkg_installed/x64-linux/lib/libcpr.a \
//       build/vcpkg_installed/x64-linux/lib/libcurl.a \
//       build/vcpkg_installed/x64-linux/lib/libsimdjson.a \
//       build/vcpkg_installed/x64-linux/lib/libspdlog.a \
//       build/vcpkg_installed/x64-linux/lib/libfmt.a \
//       -o /tmp/test_notepad_survives_compaction \
//       && /tmp/test_notepad_survives_compaction
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/tools/NotepadStore.hpp>
#include <batbox/conversation/Compactor.hpp>
#include <batbox/conversation/Message.hpp>
#include <batbox/inference/Client.hpp>
#include <batbox/config/Config.hpp>
#include <batbox/core/CancelToken.hpp>

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
using namespace batbox::conversation;
using batbox::tools::NotepadStore;

// ---------------------------------------------------------------------------
// locate fake_openai_server.py (shared fixture — used read-only, not modified)
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
    FILE* stdout_pipe{nullptr};
    int   port{0};

    bool start(const std::string& script_path) {
        int pipefd[2];
        if (::pipe(pipefd) != 0) return false;

        pid = ::fork();
        if (pid < 0) { ::close(pipefd[0]); ::close(pipefd[1]); return false; }

        if (pid == 0) {
            ::dup2(pipefd[1], STDOUT_FILENO);
            ::close(pipefd[0]);
            ::close(pipefd[1]);
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

static batbox::config::Config make_test_config(const std::string& base_url) {
    batbox::config::Config cfg;
    cfg.api.base_url            = base_url;
    cfg.api.api_key             = "test-key-123";
    cfg.api.request_timeout_sec = 10;
    return cfg;
}

static std::vector<Message> make_conversation(int pair_count) {
    std::vector<Message> msgs;
    msgs.reserve(static_cast<size_t>(pair_count) * 2);
    for (int i = 0; i < pair_count; ++i) {
        Message user;  user.role = Role::User;
        user.content = "User message " + std::to_string(i + 1);
        Message asst;  asst.role = Role::Assistant;
        asst.content = "Assistant reply " + std::to_string(i + 1);
        msgs.push_back(std::move(user));
        msgs.push_back(std::move(asst));
    }
    return msgs;
}

namespace {
struct TempRoot {
    fs::path path;
    TempRoot() {
        path = fs::temp_directory_path() / "batbox_notepad_compaction_test";
        std::error_code ec; fs::remove_all(path, ec); fs::create_directories(path, ec);
    }
    ~TempRoot() { std::error_code ec; fs::remove_all(path, ec); }
};
} // namespace

TEST_SUITE("notepad survives compaction") {

    // -----------------------------------------------------------------------
    // AC3 (decisive): jot to the pad, run a REAL compaction over a long
    // conversation, and show the pad is byte-identical afterward.
    // -----------------------------------------------------------------------
    TEST_CASE("the notepad is intact after the Compactor rewrites the messages") {
        std::string script = find_fixture_script();
        REQUIRE_FALSE(script.empty());

        FakeServer srv;
        REQUIRE(srv.start(script));

        // --- Jot gold to the out-of-band pad BEFORE compaction. ---
        TempRoot tr;
        NotepadStore pad(tr.path);
        const std::string key = "compaction-session";
        REQUIRE(pad.append(key, "the gold I will still need 20 turns from now", "findings"));
        REQUIRE(pad.append(key, "decision: disk-backed pad, inspectable", "decisions"));
        const std::string pad_before = pad.read(key);
        REQUIRE_FALSE(pad_before.empty());

        // --- Run a real compaction over a long conversation. ---
        auto cfg = make_test_config(srv.base_url());
        batbox::inference::Client client{cfg};

        auto msgs = make_conversation(8);      // 16 messages
        REQUIRE(msgs.size() == 16);

        Compactor compactor{4};                // keep last 4 verbatim
        auto [src, ct] = batbox::CancelToken::make_root();
        auto result = compactor.compact(msgs, client, std::move(ct));
        REQUIRE(result.has_value());

        // The message array WAS rewritten: fewer messages, summary first.
        const auto& compacted = result.value();
        CHECK(compacted.size() < msgs.size());
        CHECK(compacted.front().role == Role::System);   // summary message

        // --- The decisive assertion: the pad is byte-identical. ---
        const std::string pad_after = pad.read(key);
        CHECK(pad_after == pad_before);
        CHECK(pad_after.find("the gold I will still need 20 turns from now")
              != std::string::npos);

        // And it is still queryable post-compaction.
        CHECK(pad.grep(key, "decision").find("disk-backed") != std::string::npos);
    }
}
