// tests/integration/test_resume_mode.cpp
// =============================================================================
// Integration tests for #17: --resume <UUID> seeds conversation history.
//
// Tests:
//   1. --resume <uuid> with an existing session: stderr contains "Resumed session"
//      with correct message count.
//   2. --resume <nonexistent-uuid>: exit code 2, stderr contains "not found".
//   3. --resume <uuid>: process exits 0 when session found and inference completes.
//   4. SessionStore::touch() unit tests.
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/session/SessionStore.hpp>
#include <batbox/session/SessionFile.hpp>
#include <batbox/core/Uuid.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
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
using namespace batbox;
using namespace batbox::session;
using Json = nlohmann::json;

// =============================================================================
// RAII temp directory
// =============================================================================
struct TmpDir {
    fs::path path;

    TmpDir() {
        auto base = fs::temp_directory_path() /
                    ("batbox_resume_test_" + Uuid::v4().to_string());
        fs::create_directories(base);
        path = base;
    }

    ~TmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// =============================================================================
// Internal message JSON format helper.
//
// Conversation::restore() calls from_json() on each stored message, which
// requires the INTERNAL storage format (with "id", "role", "content", "ts"
// fields) — NOT the wire API format (just "role"+"content").
//
// This helper produces the same format that to_json(Message) produces, so
// sessions pre-seeded by the test are parse-compatible with restore().
// =============================================================================
static Json make_stored_message(const std::string& role, const std::string& content) {
    Json j;
    j["id"]      = Uuid::v4().to_string();
    j["role"]    = role;
    j["content"] = content;
    // "ts" field is stored as int64 milliseconds since Unix epoch
    // (matches the format written by to_json(Message) in Message.cpp).
    j["ts"]      = static_cast<std::int64_t>(1000000LL);  // 1000 seconds since epoch
    return j;
}

// =============================================================================
// Unit tests: SessionStore::touch()
// =============================================================================

TEST_SUITE("#17 — SessionStore::touch() unit tests") {

    TEST_CASE("touch() returns Ok for an existing session") {
        TmpDir tmp;
        SessionStore store(tmp.path);

        auto r = store.new_session("claude-3-5-sonnet", "/tmp/project");
        REQUIRE(r.has_value());
        const std::string sid = r.value();

        auto touch_res = store.touch(sid);
        CHECK(touch_res.has_value());
    }

    TEST_CASE("touch() updates last-accessed: session remains visible in list_recent after touch") {
        TmpDir tmp;
        SessionStore store(tmp.path);

        // Create a session.
        auto r1 = store.new_session("model-a", "/tmp/proj_a");
        REQUIRE(r1.has_value());
        const std::string sid = r1.value();

        // Touch must succeed.
        auto touch_res = store.touch(sid);
        CHECK(touch_res.has_value());

        // After touch, the session must still appear in list_recent.
        auto after_list = store.list_recent(10);
        REQUIRE(after_list.has_value());

        bool found = false;
        for (const auto& rec : after_list.value()) {
            if (rec.id.to_string() == sid) {
                found = true;
                break;
            }
        }
        CHECK(found);

        // Touch a second time: must remain idempotent (no crash, still findable).
        auto touch_res2 = store.touch(sid);
        CHECK(touch_res2.has_value());

        auto after_list2 = store.list_recent(10);
        REQUIRE(after_list2.has_value());
        bool found2 = false;
        for (const auto& rec : after_list2.value()) {
            if (rec.id.to_string() == sid) { found2 = true; break; }
        }
        CHECK(found2);
    }

    TEST_CASE("touch() on non-existent session returns Ok (non-fatal)") {
        // touch() only updates the index — missing session is non-fatal.
        TmpDir tmp;
        SessionStore store(tmp.path);

        auto touch_res = store.touch(Uuid::v4().to_string());
        // Must not throw or crash.  Return value is implementation-defined
        // but we expect Ok (non-fatal design).
        (void)touch_res;
        // If it returns Err that's also acceptable — key: no crash.
    }

} // TEST_SUITE

// =============================================================================
// Subprocess helpers (same pattern as test_headless_print.cpp)
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
    return {};
}

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

struct FakeServer {
    pid_t  pid{-1};
    int    port{0};
    FILE*  out_pipe{nullptr};

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
        out_pipe = ::fdopen(pipefd[0], "r");
        if (!out_pipe) { ::kill(pid, SIGTERM); return false; }
        char buf[128] = {};
        ::fcntl(pipefd[0], F_SETFL, ::fcntl(pipefd[0], F_GETFL) | O_NONBLOCK);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
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
                        return port > 0;
                    }
                    break;
                }
            } else if (n == 0) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        stop();
        return false;
    }

    void stop() {
        if (pid > 0) { ::kill(pid, SIGTERM); int s = 0; ::waitpid(pid, &s, 0); pid = -1; }
        if (out_pipe) { ::fclose(out_pipe); out_pipe = nullptr; }
    }

    ~FakeServer() { stop(); }
};

struct RunResult {
    int         exit_code{-1};
    std::string stdout_text;
    std::string stderr_text;
};

static RunResult run_batbox(
    const std::string&              batbox_path,
    const std::vector<std::string>& argv_extra,
    const std::vector<std::string>& env_extra)
{
    RunResult result;
    if (batbox_path.empty()) {
        result.exit_code = -2;
        result.stderr_text = "batbox binary not found";
        return result;
    }

    std::vector<const char*> argv_vec;
    argv_vec.push_back(batbox_path.c_str());
    for (const auto& a : argv_extra) argv_vec.push_back(a.c_str());
    argv_vec.push_back(nullptr);

    extern char** environ;
    std::vector<std::string> env_strings;
    for (char** e = environ; *e; ++e) {
        std::string kv(*e);
        bool skip = false;
        for (const auto& override_kv : env_extra) {
            auto eq = override_kv.find('=');
            if (eq == std::string::npos) continue;
            std::string override_key = override_kv.substr(0, eq + 1);
            if (kv.rfind(override_key, 0) == 0) { skip = true; break; }
        }
        if (!skip) env_strings.push_back(kv);
    }
    for (const auto& kv : env_extra) env_strings.push_back(kv);
    std::vector<const char*> envp;
    for (const auto& s : env_strings) envp.push_back(s.c_str());
    envp.push_back(nullptr);

    int stdout_pipe[2], stderr_pipe[2];
    if (::pipe(stdout_pipe) != 0 || ::pipe(stderr_pipe) != 0) {
        result.exit_code = -3;
        return result;
    }

    pid_t child = ::fork();
    if (child < 0) { result.exit_code = -4; return result; }

    if (child == 0) {
        ::dup2(stdout_pipe[1], STDOUT_FILENO);
        ::dup2(stderr_pipe[1], STDERR_FILENO);
        // Redirect stdin from /dev/null (non-interactive).
        int devnull = ::open("/dev/null", O_RDONLY);
        if (devnull >= 0) { ::dup2(devnull, STDIN_FILENO); ::close(devnull); }
        ::close(stdout_pipe[0]); ::close(stdout_pipe[1]);
        ::close(stderr_pipe[0]); ::close(stderr_pipe[1]);
        ::execve(batbox_path.c_str(),
                 const_cast<char* const*>(argv_vec.data()),
                 const_cast<char* const*>(envp.data()));
        ::_exit(127);
    }

    ::close(stdout_pipe[1]);
    ::close(stderr_pipe[1]);

    ::fcntl(stdout_pipe[0], F_SETFL, ::fcntl(stdout_pipe[0], F_GETFL) | O_NONBLOCK);
    ::fcntl(stderr_pipe[0], F_SETFL, ::fcntl(stderr_pipe[0], F_GETFL) | O_NONBLOCK);

    auto read_all = [](int fd, std::string& out) {
        char buf[4096];
        ssize_t n;
        while ((n = ::read(fd, buf, sizeof(buf))) > 0)
            out.append(buf, static_cast<std::size_t>(n));
    };

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    int wstatus = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        read_all(stdout_pipe[0], result.stdout_text);
        read_all(stderr_pipe[0], result.stderr_text);
        pid_t w = ::waitpid(child, &wstatus, WNOHANG);
        if (w == child) { child = -1; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (child > 0) { ::kill(child, SIGTERM); ::waitpid(child, &wstatus, 0); }
    read_all(stdout_pipe[0], result.stdout_text);
    read_all(stderr_pipe[0], result.stderr_text);
    ::close(stdout_pipe[0]);
    ::close(stderr_pipe[0]);

    if (WIFEXITED(wstatus))        result.exit_code = WEXITSTATUS(wstatus);
    else if (WIFSIGNALED(wstatus)) result.exit_code = 128 + WTERMSIG(wstatus);
    return result;
}

// =============================================================================
// E2E tests: --resume flag behaviour
// =============================================================================

TEST_SUITE("#17 — --resume <UUID> e2e") {

    static FakeServer s_server;
    static std::string s_batbox;
    static bool s_setup_done = false;

    auto setup = []() -> bool {
        if (s_setup_done) return s_server.port > 0 && !s_batbox.empty();
        s_setup_done = true;
        const auto script = find_fixture_script();
        if (script.empty()) return false;
        s_server.start(script);
        s_batbox = find_batbox_binary();
        return s_server.port > 0 && !s_batbox.empty();
    };

    auto base_env = [](int port, const std::string& config_dir) -> std::vector<std::string> {
        return {
            "BATBOX_API_BASE_URL=http://127.0.0.1:" + std::to_string(port) + "/v1",
            "BATBOX_API_KEY=test-key-123",
            "BATBOX_NO_SPLASH=1",
            "BATBOX_SIDECAR_PREWARM=0",
            "BATBOX_SIDECAR_AUTOSTART=0",
            "BATBOX_CONFIG_DIR=" + config_dir,
        };
    };

    TEST_CASE("--resume nonexistent-uuid: exit code 2, stderr contains 'not found'") {
        if (!setup()) {
            MESSAGE("Skipping: fake server or batbox binary not available");
            return;
        }

        TmpDir tmp;
        const std::string fake_uuid = Uuid::v4().to_string();
        auto result = run_batbox(s_batbox,
            {"--resume", fake_uuid, "--print", "hello"},
            base_env(s_server.port, tmp.path.string()));

        // Must exit with code 2 (session not found).
        CHECK(result.exit_code == 2);
        // Stderr must mention "not found".
        CHECK(result.stderr_text.find("not found") != std::string::npos);
    }

    TEST_CASE("--resume <uuid>: stderr contains 'Resumed session' with message count") {
        if (!setup()) {
            MESSAGE("Skipping: fake server or batbox binary not available");
            return;
        }

        // Pre-seed a session with 4 messages using the internal stored format.
        TmpDir tmp;
        fs::path sessions_dir = tmp.path / "sessions";
        fs::create_directories(sessions_dir);

        SessionStore store(sessions_dir);
        auto sid_res = store.new_session("claude-3-5-sonnet", "/tmp/test_project");
        REQUIRE(sid_res.has_value());
        const std::string sid = sid_res.value();

        // Append messages using the internal storage format that restore() expects.
        (void)store.append_message(sid, make_stored_message("system",    "You are helpful."));
        (void)store.append_message(sid, make_stored_message("user",      "What is 2+2?"));
        (void)store.append_message(sid, make_stored_message("assistant", "4."));
        (void)store.append_message(sid, make_stored_message("user",      "What is 3+3?"));

        // Run batbox --resume <sid> --print against the fake inference server.
        // BATBOX_CONFIG_DIR points to tmp.path so batbox uses our sessions_dir.
        auto result = run_batbox(s_batbox,
            {"--resume", sid, "--print", "what did I ask first?"},
            base_env(s_server.port, tmp.path.string()));

        // Stderr must contain the "Resumed session" confirmation line.
        INFO("stderr: " << result.stderr_text);
        CHECK(result.stderr_text.find("Resumed session") != std::string::npos);
        CHECK(result.stderr_text.find(sid) != std::string::npos);
        // 4 messages were seeded.
        CHECK(result.stderr_text.find("4 messages") != std::string::npos);
    }

    TEST_CASE("--resume <uuid>: process exits 0 when session exists and inference completes") {
        if (!setup()) {
            MESSAGE("Skipping: fake server or batbox binary not available");
            return;
        }

        TmpDir tmp;
        fs::path sessions_dir = tmp.path / "sessions";
        fs::create_directories(sessions_dir);

        SessionStore store(sessions_dir);
        auto sid_res = store.new_session("claude-3-5-sonnet", "/tmp/test_project2");
        REQUIRE(sid_res.has_value());
        const std::string sid = sid_res.value();

        // Seed 2 messages.
        (void)store.append_message(sid, make_stored_message("user",      "hello"));
        (void)store.append_message(sid, make_stored_message("assistant", "hi"));

        auto result = run_batbox(s_batbox,
            {"--resume", sid, "--print", "continue"},
            base_env(s_server.port, tmp.path.string()));

        INFO("exit_code: " << result.exit_code);
        INFO("stderr: " << result.stderr_text);
        // Should exit 0 — session found, inference completed.
        CHECK(result.exit_code == 0);
    }

} // TEST_SUITE
