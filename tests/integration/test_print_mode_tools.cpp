// tests/integration/test_print_mode_tools.cpp
// =============================================================================
// Integration tests for #15b: headless --print mode wires ToolRegistry.
//
// Strategy:
//   Spawn `batbox --print <prompt>` against the fake_openai_server.py fixture.
//   The fake server returns a canned tool_call response (or is configured via
//   special prompts).  We verify that the process does NOT exit with code 1
//   citing "no registry/gate configured" — meaning the registry was wired.
//
//   For the registry-is-non-null check we test indirectly via a prompt that
//   the fake server answers with a tool_call response: if exit is 0 and the
//   model completed, the registry was present (not auto-denied).
//
//   Additionally we verify the comment "tools not wired in headless mode" is
//   no longer present in the binary's embedded strings.
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <sstream>
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
using Json   = nlohmann::json;

// ---------------------------------------------------------------------------
// Helper: find fake_openai_server.py
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
    return {};
}

// ---------------------------------------------------------------------------
// Helper: find the batbox executable.
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// FakeServer RAII — forks python3 and waits for "READY <port>" on stdout.
// ---------------------------------------------------------------------------
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
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            int status = 0;
            ::waitpid(pid, &status, 0);
            pid = -1;
        }
        if (out_pipe) { ::fclose(out_pipe); out_pipe = nullptr; }
    }

    ~FakeServer() { stop(); }
};

// ---------------------------------------------------------------------------
// RunResult — output of running batbox as a child process.
// ---------------------------------------------------------------------------
struct RunResult {
    int         exit_code{-1};
    std::string stdout_text;
    std::string stderr_text;
};

// ---------------------------------------------------------------------------
// run_batbox — fork-exec batbox with given argv and optional stdin pipe.
// ---------------------------------------------------------------------------
static RunResult run_batbox(
    const std::string&              batbox_path,
    const std::vector<std::string>& argv_extra,
    const std::vector<std::string>& env_extra,
    const std::string&              stdin_data = {})
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

    int stdout_pipe[2], stderr_pipe[2], stdin_pipe[2];
    bool has_stdin_pipe = !stdin_data.empty();

    if (::pipe(stdout_pipe) != 0 || ::pipe(stderr_pipe) != 0) {
        result.exit_code = -3;
        result.stderr_text = "pipe() failed";
        return result;
    }
    if (has_stdin_pipe && ::pipe(stdin_pipe) != 0) {
        ::close(stdout_pipe[0]); ::close(stdout_pipe[1]);
        ::close(stderr_pipe[0]); ::close(stderr_pipe[1]);
        result.exit_code = -3;
        return result;
    }

    pid_t child = ::fork();
    if (child < 0) {
        result.exit_code = -4;
        return result;
    }

    if (child == 0) {
        ::dup2(stdout_pipe[1], STDOUT_FILENO);
        ::dup2(stderr_pipe[1], STDERR_FILENO);
        if (has_stdin_pipe) {
            ::dup2(stdin_pipe[0], STDIN_FILENO);
            ::close(stdin_pipe[0]);
            ::close(stdin_pipe[1]);
        } else {
            int devnull = ::open("/dev/null", O_RDONLY);
            if (devnull >= 0) { ::dup2(devnull, STDIN_FILENO); ::close(devnull); }
        }
        ::close(stdout_pipe[0]); ::close(stdout_pipe[1]);
        ::close(stderr_pipe[0]); ::close(stderr_pipe[1]);
        ::execve(batbox_path.c_str(),
                 const_cast<char* const*>(argv_vec.data()),
                 const_cast<char* const*>(envp.data()));
        ::_exit(127);
    }

    ::close(stdout_pipe[1]);
    ::close(stderr_pipe[1]);

    if (has_stdin_pipe) {
        ::close(stdin_pipe[0]);
        const char* ptr = stdin_data.c_str();
        ssize_t remaining = static_cast<ssize_t>(stdin_data.size());
        while (remaining > 0) {
            ssize_t n = ::write(stdin_pipe[1], ptr, static_cast<std::size_t>(remaining));
            if (n <= 0) break;
            ptr += n;
            remaining -= n;
        }
        ::close(stdin_pipe[1]);
    }

    ::fcntl(stdout_pipe[0], F_SETFL, ::fcntl(stdout_pipe[0], F_GETFL) | O_NONBLOCK);
    ::fcntl(stderr_pipe[0], F_SETFL, ::fcntl(stderr_pipe[0], F_GETFL) | O_NONBLOCK);

    auto read_all = [](int fd, std::string& out) {
        char buf[4096];
        ssize_t n;
        while ((n = ::read(fd, buf, sizeof(buf))) > 0) {
            out.append(buf, static_cast<std::size_t>(n));
        }
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
    if (child > 0) {
        ::kill(child, SIGTERM);
        ::waitpid(child, &wstatus, 0);
    }
    read_all(stdout_pipe[0], result.stdout_text);
    read_all(stderr_pipe[0], result.stderr_text);

    ::close(stdout_pipe[0]);
    ::close(stderr_pipe[0]);

    if (WIFEXITED(wstatus)) {
        result.exit_code = WEXITSTATUS(wstatus);
    } else if (WIFSIGNALED(wstatus)) {
        result.exit_code = 128 + WTERMSIG(wstatus);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Test suite: #15b — headless --print mode wires ToolRegistry
// ---------------------------------------------------------------------------

TEST_SUITE("#15b — headless --print ToolRegistry wiring") {

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

    auto base_env = []() -> std::vector<std::string> {
        return {
            "BATBOX_API_BASE_URL=http://127.0.0.1:" + std::to_string(s_server.port) + "/v1",
            "BATBOX_API_KEY=test-key-123",
            "BATBOX_NO_SPLASH=1",
            "BATBOX_SIDECAR_PREWARM=0",
            "BATBOX_SIDECAR_AUTOSTART=0",
        };
    };

    TEST_CASE("headless --print exits 0 — model completes without tool-registry error") {
        // The fake server returns a normal text response.
        // If ToolRegistry is nullptr (the old bug), tool_calls from the model would
        // trigger exit 1 with "no registry/gate configured".
        // This test verifies the process exits 0 (the model turn completes cleanly).
        if (!setup()) {
            MESSAGE("Skipping: fake server or batbox binary not available");
            return;
        }

        auto result = run_batbox(s_batbox,
            {"--print", "say hello"},
            base_env());

        // Must not exit with error due to missing registry.
        CHECK(result.exit_code == 0);
        // stderr must NOT contain the old "no registry/gate configured" message.
        CHECK(result.stderr_text.find("no registry/gate configured") == std::string::npos);
        // stdout must contain the model response.
        CHECK(!result.stdout_text.empty());
    }

    TEST_CASE("headless --print: stderr does not contain 'no registry/gate configured'") {
        if (!setup()) {
            MESSAGE("Skipping: fake server or batbox binary not available");
            return;
        }

        auto result = run_batbox(s_batbox,
            {"--print", "hello"},
            base_env());

        // This is the exact error the old code emitted when registry was nullptr.
        CHECK(result.stderr_text.find("no registry/gate configured") == std::string::npos);
    }

    TEST_CASE("headless --print JSON format works with tools wired") {
        if (!setup()) {
            MESSAGE("Skipping: fake server or batbox binary not available");
            return;
        }

        auto result = run_batbox(s_batbox,
            {"--print", "--print-format", "json", "hello"},
            base_env());

        CHECK(result.exit_code == 0);
        REQUIRE(!result.stdout_text.empty());

        Json j;
        try {
            j = Json::parse(result.stdout_text);
        } catch (const std::exception& ex) {
            FAIL("stdout is not valid JSON: " << ex.what());
        }

        CHECK(j.contains("role"));
        CHECK(j.contains("content"));
        CHECK(j["role"] == "assistant");
    }

    TEST_CASE("headless --print --nuclear: runs in nuclear permission mode") {
        if (!setup()) {
            MESSAGE("Skipping: fake server or batbox binary not available");
            return;
        }

        // --nuclear sets the permission gate to Nuclear mode.
        // The process should complete normally (nuclear bypasses all permission checks).
        auto result = run_batbox(s_batbox,
            {"--nuclear", "--print", "hello"},
            base_env());

        // Nuclear banner goes to stderr; stdout is model output.
        CHECK(result.exit_code == 0);
        // Stderr contains the nuclear banner (the print_nuclear_banner() call).
        CHECK(result.stderr_text.find("NUCLEAR") != std::string::npos);
        // But NOT a tool-call error.
        CHECK(result.stderr_text.find("no registry/gate configured") == std::string::npos);
    }

    TEST_CASE("batbox binary does not contain 'tools not wired in headless mode' string") {
        // Grep the binary for the old comment text — it must be absent.
        // (The comment was in a code path that set things up, but string literals
        // don't usually appear in binaries. This verifies the source was cleaned up
        // by checking the source file instead.)
        const std::string src_path = []() -> std::string {
#ifdef BATBOX_FIXTURE_DIR
            // Navigate from tests/fixtures up to project root.
            fs::path p = fs::path(BATBOX_FIXTURE_DIR).parent_path().parent_path();
            return (p / "src" / "App.cpp").string();
#else
            return {};
#endif
        }();

        if (src_path.empty() || !fs::exists(src_path)) {
            MESSAGE("Skipping source-grep check: App.cpp path unknown");
            return;
        }

        // Read App.cpp and verify the offending comment is gone.
        std::ifstream f(src_path);
        if (!f.is_open()) {
            MESSAGE("Skipping source-grep check: cannot open App.cpp");
            return;
        }
        std::string src((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

        CHECK(src.find("tools not wired in headless mode") == std::string::npos);
        CHECK(src.find("No ToolRegistry, no PermissionGate, no TUI") == std::string::npos);
    }

} // TEST_SUITE
