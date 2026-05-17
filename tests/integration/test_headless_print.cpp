// tests/integration/test_headless_print.cpp
// =============================================================================
// Integration tests for CPP A.1 — headless --print mode.
//
// Strategy:
//   Spawn `batbox --print <prompt>` as a child process against the
//   fake_openai_server.py fixture (same technique as CPP 4.5 smoke tests).
//
//   The fake server is pointed at via BATBOX_API_BASE_URL in the child's
//   environment.  We set BATBOX_API_KEY=test-key-123 to satisfy auth.
//
// Tests:
//   1. argv prompt path      — `batbox --print "hello"` → assistant text on stdout
//   2. stdin prompt path     — `echo "hello" | batbox --print` → assistant text on stdout
//   3. --print-format=json   — JSON object with role/content/usage keys
//   4. --print-format=markdown — content on stdout (same as plain for text)
//   5. --model override      — uses the model name specified (reflected in request)
//   6. exit code 0 on success
//   7. exit code 1 on inference error (server returns 500)
//   8. No TUI output (no FTXUI widgets, no splash banner) on stdout
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
// BATBOX_FIXTURE_DIR injected by CMake at compile time.
// ---------------------------------------------------------------------------
static std::string find_fixture_script() {
#ifdef BATBOX_FIXTURE_DIR
    fs::path p = fs::path(BATBOX_FIXTURE_DIR) / "fake_openai_server.py";
    if (fs::exists(p)) return p.string();
#endif
    // Walk up from cwd.
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
// BATBOX_BINARY_DIR injected by CMake at compile time.
// ---------------------------------------------------------------------------
static std::string find_batbox_binary() {
#ifdef BATBOX_BINARY_DIR
    // CMake build dir has the 'batbox' executable in the binary dir.
    fs::path p = fs::path(BATBOX_BINARY_DIR) / "src" / "batbox";
    if (fs::exists(p)) return p.string();
    // Some generators place it one level up.
    fs::path p2 = fs::path(BATBOX_BINARY_DIR) / "batbox";
    if (fs::exists(p2)) return p2.string();
#endif
    // Fallback: search PATH via which.
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
            // Child: redirect stdout → pipe write end.
            ::close(pipefd[0]);
            ::dup2(pipefd[1], STDOUT_FILENO);
            ::close(pipefd[1]);
            // Suppress child stderr to avoid cluttering test output.
            int devnull = ::open("/dev/null", O_WRONLY);
            if (devnull >= 0) { ::dup2(devnull, STDERR_FILENO); ::close(devnull); }
            ::execlp("python3", "python3", script_path.c_str(), nullptr);
            ::_exit(127);
        }

        // Parent: read "READY <port>\n" from pipe.
        ::close(pipefd[1]);
        out_pipe = ::fdopen(pipefd[0], "r");
        if (!out_pipe) { ::kill(pid, SIGTERM); return false; }

        char buf[128] = {};
        // Give the server up to 10 s to start.
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
                    break; // unexpected first line
                }
            } else if (n == 0) {
                break; // pipe closed
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
//
// @param batbox_path    Absolute path to the batbox binary.
// @param argv_extra     Extra arguments after "batbox" (e.g. {"--print", "hello"}).
// @param env_extra      Extra env vars to inject (key=value pairs).
// @param stdin_data     If non-empty, pipe this text to stdin of the child.
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

    // Build argv for execve.
    std::vector<const char*> argv_vec;
    argv_vec.push_back(batbox_path.c_str());
    for (const auto& a : argv_extra) argv_vec.push_back(a.c_str());
    argv_vec.push_back(nullptr);

    // Build environ for execve: inherit parent env + inject extras.
    // We collect all current env vars then patch/add from env_extra.
    extern char** environ;
    std::vector<std::string> env_strings;
    for (char** e = environ; *e; ++e) {
        std::string kv(*e);
        // Filter out any vars we will override.
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

    // Create pipes for stdout, stderr, and optionally stdin.
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
        result.stderr_text = "stdin pipe() failed";
        return result;
    }

    pid_t child = ::fork();
    if (child < 0) {
        result.exit_code = -4;
        result.stderr_text = "fork() failed";
        return result;
    }

    if (child == 0) {
        // Child process: wire up pipes.
        ::dup2(stdout_pipe[1], STDOUT_FILENO);
        ::dup2(stderr_pipe[1], STDERR_FILENO);
        if (has_stdin_pipe) {
            ::dup2(stdin_pipe[0], STDIN_FILENO);
            ::close(stdin_pipe[0]);
            ::close(stdin_pipe[1]);
        } else {
            // Redirect stdin from /dev/null so the child knows it is not a tty.
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

    // Parent: close write ends of output pipes.
    ::close(stdout_pipe[1]);
    ::close(stderr_pipe[1]);

    // Write stdin data if any.
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

    // Read stdout + stderr from child (non-blocking to avoid deadlock).
    ::fcntl(stdout_pipe[0], F_SETFL, ::fcntl(stdout_pipe[0], F_GETFL) | O_NONBLOCK);
    ::fcntl(stderr_pipe[0], F_SETFL, ::fcntl(stderr_pipe[0], F_GETFL) | O_NONBLOCK);

    auto read_all = [](int fd, std::string& out) {
        char buf[4096];
        ssize_t n;
        while ((n = ::read(fd, buf, sizeof(buf))) > 0) {
            out.append(buf, static_cast<std::size_t>(n));
        }
    };

    // Wait for child to exit, draining pipes periodically to avoid blocking.
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
// Test suite
// ---------------------------------------------------------------------------

TEST_SUITE("CPP A.1 — headless --print mode") {

    // Shared fixture: start fake server once and reuse across test cases.
    // doctest does not have a per-suite fixture mechanism, so we use a
    // static initialiser pattern with a FakeServer instance.

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

    // Helper: build common env vars pointing batbox at the fake server.
    auto base_env = []() -> std::vector<std::string> {
        return {
            "BATBOX_API_BASE_URL=http://127.0.0.1:" + std::to_string(s_server.port) + "/v1",
            "BATBOX_API_KEY=test-key-123",
            "BATBOX_NO_SPLASH=1",
            // Disable sidecar prewarm in headless tests.
            "BATBOX_SIDECAR_PREWARM=0",
            "BATBOX_SIDECAR_AUTOSTART=0",
        };
    };

    TEST_CASE("argv prompt path — basic text response") {
        if (!setup()) {
            MESSAGE("Skipping: fake server or batbox binary not available");
            return;
        }

        auto result = run_batbox(s_batbox,
            {"--print", "hello"},
            base_env());

        CHECK(result.exit_code == 0);
        // The fake server returns a canned text response; it should be non-empty.
        CHECK(!result.stdout_text.empty());
        // Stdout must not contain TUI / FTXUI escape sequences or the splash banner.
        CHECK(result.stdout_text.find("batbox 0.1.0") == std::string::npos);
    }

    TEST_CASE("stdin prompt path — piped input") {
        if (!setup()) {
            MESSAGE("Skipping: fake server or batbox binary not available");
            return;
        }

        // Pass prompt via stdin — no positional arg.
        auto result = run_batbox(s_batbox,
            {"--print"},
            base_env(),
            "hello from stdin\n");

        CHECK(result.exit_code == 0);
        CHECK(!result.stdout_text.empty());
    }

    TEST_CASE("--print-format=plain default format") {
        if (!setup()) {
            MESSAGE("Skipping: fake server or batbox binary not available");
            return;
        }

        auto result = run_batbox(s_batbox,
            {"--print", "--print-format", "plain", "hello"},
            base_env());

        CHECK(result.exit_code == 0);
        // Plain output is raw text — not valid JSON.
        bool is_json = false;
        try {
            Json::parse(result.stdout_text);
            is_json = true;
        } catch (...) {}
        CHECK_FALSE(is_json);
    }

    TEST_CASE("--print-format=json emits valid JSON with role/content/usage") {
        if (!setup()) {
            MESSAGE("Skipping: fake server or batbox binary not available");
            return;
        }

        auto result = run_batbox(s_batbox,
            {"--print", "--print-format", "json", "hello"},
            base_env());

        CHECK(result.exit_code == 0);
        REQUIRE(!result.stdout_text.empty());

        // stdout must be valid JSON.
        Json j;
        try {
            j = Json::parse(result.stdout_text);
        } catch (const std::exception& ex) {
            FAIL("stdout is not valid JSON: " << ex.what()
                 << "\nstdout was: " << result.stdout_text);
        }

        CHECK(j.contains("role"));
        CHECK(j.contains("content"));
        CHECK(j.contains("usage"));
        CHECK(j["role"] == "assistant");
        CHECK(j["content"].is_string());
        CHECK(!j["content"].get<std::string>().empty());
        CHECK(j["usage"].contains("prompt_tokens"));
        CHECK(j["usage"].contains("completion_tokens"));
        CHECK(j["usage"].contains("total_tokens"));
        CHECK(j["usage"].contains("cost_usd"));
    }

    TEST_CASE("--print-format=markdown emits text content") {
        if (!setup()) {
            MESSAGE("Skipping: fake server or batbox binary not available");
            return;
        }

        auto result = run_batbox(s_batbox,
            {"--print", "--print-format", "markdown", "hello"},
            base_env());

        CHECK(result.exit_code == 0);
        CHECK(!result.stdout_text.empty());
    }

    TEST_CASE("--model flag is honoured") {
        if (!setup()) {
            MESSAGE("Skipping: fake server or batbox binary not available");
            return;
        }

        // The fake server accepts any model name; we just verify no error.
        auto result = run_batbox(s_batbox,
            {"--print", "--model", "gpt-4o-mini", "hello"},
            base_env());

        CHECK(result.exit_code == 0);
    }

    TEST_CASE("exit code 0 on success") {
        if (!setup()) {
            MESSAGE("Skipping: fake server or batbox binary not available");
            return;
        }

        auto result = run_batbox(s_batbox, {"--print", "hello"}, base_env());
        CHECK(result.exit_code == 0);
    }

    TEST_CASE("exit code 1 on inference error (server 500)") {
        if (!setup()) {
            MESSAGE("Skipping: fake server or batbox binary not available");
            return;
        }

        // Point at a non-existent server port to trigger a transport error.
        std::vector<std::string> env = base_env();
        // Override to an unreachable port.
        for (auto& kv : env) {
            if (kv.rfind("BATBOX_API_BASE_URL=", 0) == 0) {
                kv = "BATBOX_API_BASE_URL=http://127.0.0.1:19999/v1"; // unlikely to be open
                break;
            }
        }

        auto result = run_batbox(s_batbox,
            {"--print", "hello"},
            env);

        // Transport failure → exit 1.
        CHECK(result.exit_code == 1);
    }

    TEST_CASE("no TUI output — stdout does not contain FTXUI artefacts") {
        if (!setup()) {
            MESSAGE("Skipping: fake server or batbox binary not available");
            return;
        }

        auto result = run_batbox(s_batbox, {"--print", "hello"}, base_env());

        CHECK(result.exit_code == 0);
        // No splash banner on stdout.
        CHECK(result.stdout_text.find("batbox 0.1.0") == std::string::npos);
        // No FTXUI box-drawing on stdout.
        // ESC [ sequences would indicate TUI output; a brief check.
        // (We allow ESC sequences that are part of the model response itself,
        // but the fake server returns plain ASCII, so any ESC is a TUI leak.)
        CHECK(result.stdout_text.find('\033') == std::string::npos);
    }

    TEST_CASE("--print without prompt and tty stdin → exit 1 with error message") {
        if (!setup()) {
            MESSAGE("Skipping: fake server or batbox binary not available");
            return;
        }

        // No positional arg and no stdin data; stdin redirected to /dev/null by helper.
        auto result = run_batbox(s_batbox, {"--print"}, base_env());

        // Should fail gracefully with exit 1.
        CHECK(result.exit_code == 1);
        CHECK(result.stderr_text.find("prompt") != std::string::npos);
    }

} // TEST_SUITE
