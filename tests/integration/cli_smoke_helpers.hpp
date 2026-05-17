// tests/integration/cli_smoke_helpers.hpp
// =============================================================================
// CPP T.5 — End-to-end CLI smoke test helpers
//
// Provides RAII wrappers and helpers for spawning the batbox binary as a child
// process, feeding it stdin commands, reading its stdout/stderr, and shutting
// it down cleanly.
//
// Helpers:
//   find_fixture_dir()     — locate tests/fixtures/ directory
//   find_batbox_binary()   — locate the compiled batbox binary
//   FakeServer             — RAII wrapper around fake_openai_server.py
//   TempDir                — RAII temporary directory
//   BatboxProcess          — RAII handle for a spawned batbox child process
//   spawn_batbox()         — fork-exec batbox with given args + env
//   send_line()            — write a line to the child's stdin
//   read_until()           — drain stdout until a marker string is seen
//   wait_for_exit()        — wait for child to exit, drain stdout/stderr
//   kill_proc()            — send SIGTERM to child
//   strip_ansi()           — remove ANSI escape sequences from a string
// =============================================================================

#pragma once

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

#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// find_fixture_dir — locate the tests/fixtures/ directory.
// BATBOX_FIXTURE_DIR may be injected as a compile-time define by CMake.
// Falls back to walking up from cwd.
// ---------------------------------------------------------------------------
inline std::string find_fixture_dir() {
#ifdef BATBOX_FIXTURE_DIR
    {
        fs::path p(BATBOX_FIXTURE_DIR);
        if (fs::exists(p)) return p.string();
    }
#endif
    fs::path dir = fs::current_path();
    for (int depth = 0; depth < 8; ++depth) {
        fs::path candidate = dir / "tests" / "fixtures";
        if (fs::exists(candidate)) return candidate.string();
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    return {};
}

// ---------------------------------------------------------------------------
// find_batbox_binary — locate the batbox executable.
// BATBOX_BINARY_DIR may be injected as a compile-time define by CMake.
// Falls back to $PATH via which.
// ---------------------------------------------------------------------------
inline std::string find_batbox_binary() {
#ifdef BATBOX_BINARY_DIR
    {
        // Standard CMake build layout: binary dir has batbox under src/ or at root.
        fs::path p1 = fs::path(BATBOX_BINARY_DIR) / "src" / "batbox";
        if (fs::exists(p1)) return p1.string();
        fs::path p2 = fs::path(BATBOX_BINARY_DIR) / "batbox";
        if (fs::exists(p2)) return p2.string();
    }
#endif
    // Fallback: search PATH.
    FILE* f = ::popen("which batbox 2>/dev/null", "r");
    if (f) {
        char buf[512] = {};
        if (std::fgets(buf, sizeof(buf), f)) {
            std::string s(buf);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            ::pclose(f);
            if (!s.empty() && fs::exists(s)) return s;
            return {};
        }
        ::pclose(f);
    }
    return {};
}

// ---------------------------------------------------------------------------
// strip_ansi — remove ANSI CSI escape sequences (ESC [ ... m / H / J etc.)
// from a string.  Useful for asserting on terminal output.
// ---------------------------------------------------------------------------
inline std::string strip_ansi(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    bool in_esc = false;
    for (std::size_t i = 0; i < input.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(input[i]);
        if (in_esc) {
            // Consume characters until a 'final byte' in range 0x40-0x7E
            if (c >= 0x40 && c <= 0x7E) in_esc = false;
            continue;
        }
        if (c == '\033' && i + 1 < input.size() &&
            static_cast<unsigned char>(input[i + 1]) == '[') {
            in_esc = true;
            ++i; // skip '['
            continue;
        }
        out += static_cast<char>(c);
    }
    return out;
}

// ---------------------------------------------------------------------------
// TempDir — RAII temporary directory.
// Created via mkdtemp(); recursively removed on destruction.
// ---------------------------------------------------------------------------
struct TempDir {
    fs::path path;

    explicit TempDir(const char* prefix = "batbox_e2e_smoke_") {
        std::string tmpl = fs::temp_directory_path() / (std::string(prefix) + "XXXXXX");
        char* ret = ::mkdtemp(tmpl.data());
        if (ret) path = ret;
    }

    ~TempDir() {
        if (!path.empty()) {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    }

    bool valid() const { return !path.empty() && fs::exists(path); }

    // Disable copy; allow move.
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&& o) noexcept : path(std::move(o.path)) { o.path.clear(); }
    TempDir& operator=(TempDir&& o) noexcept {
        if (this != &o) { std::error_code ec; fs::remove_all(path, ec); path = std::move(o.path); o.path.clear(); }
        return *this;
    }
};

// ---------------------------------------------------------------------------
// FakeServer — RAII wrapper around fake_openai_server.py.
// Forks python3 and waits for "READY <port>" on stdout.
// ---------------------------------------------------------------------------
struct FakeServer {
    pid_t  pid{-1};
    int    port{0};
    int    pipe_read_fd{-1};

    bool start(const std::string& fixture_dir) {
        if (fixture_dir.empty()) return false;
        fs::path script = fs::path(fixture_dir) / "fake_openai_server.py";
        if (!fs::exists(script)) return false;

        int pipefd[2];
        if (::pipe(pipefd) != 0) return false;

        pid = ::fork();
        if (pid < 0) { ::close(pipefd[0]); ::close(pipefd[1]); return false; }

        if (pid == 0) {
            // Child: redirect stdout → pipe write end.
            ::close(pipefd[0]);
            ::dup2(pipefd[1], STDOUT_FILENO);
            ::close(pipefd[1]);
            int devnull = ::open("/dev/null", O_WRONLY);
            if (devnull >= 0) { ::dup2(devnull, STDERR_FILENO); ::close(devnull); }
            const char* argv[] = {"python3", script.c_str(), nullptr};
            ::execvp("python3", const_cast<char* const*>(argv));
            ::_exit(127);
        }

        // Parent: read "READY <port>\n" from pipe.
        ::close(pipefd[1]);
        pipe_read_fd = pipefd[0];

        // Non-blocking read with 10-second timeout.
        ::fcntl(pipe_read_fd, F_SETFL, ::fcntl(pipe_read_fd, F_GETFL) | O_NONBLOCK);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        std::string accum;
        char buf[256] = {};
        while (std::chrono::steady_clock::now() < deadline) {
            ssize_t n = ::read(pipe_read_fd, buf, sizeof(buf) - 1);
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
                    break; // unexpected line
                }
            } else if (n == 0) {
                break; // pipe closed prematurely
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        stop();
        return false;
    }

    void stop() {
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            int st = 0;
            ::waitpid(pid, &st, 0);
            pid = -1;
        }
        if (pipe_read_fd >= 0) { ::close(pipe_read_fd); pipe_read_fd = -1; }
        port = 0;
    }

    ~FakeServer() { stop(); }

    // Build the base environment vector for a batbox child that talks to this server.
    // sidecar_prewarm: set 0 to disable sidecar during tests.
    std::vector<std::string> base_env(int sidecar_prewarm = 0) const {
        return {
            "BATBOX_API_BASE_URL=http://127.0.0.1:" + std::to_string(port) + "/v1",
            "BATBOX_API_KEY=test-key-123",
            "BATBOX_NO_SPLASH=1",
            "BATBOX_SIDECAR_PREWARM=" + std::to_string(sidecar_prewarm),
            "BATBOX_SIDECAR_AUTOSTART=0",
        };
    }
};

// ---------------------------------------------------------------------------
// BatboxProcess — RAII handle for a spawned batbox child process.
//
// Fields:
//   pid        — child process ID (-1 if not running)
//   stdin_fd   — write end of the child's stdin pipe (-1 if closed)
//   stdout_fd  — read end of the child's stdout pipe (-1 if closed)
//   stderr_fd  — read end of the child's stderr pipe (-1 if closed)
// ---------------------------------------------------------------------------
struct BatboxProcess {
    pid_t pid{-1};
    int   stdin_fd{-1};
    int   stdout_fd{-1};
    int   stderr_fd{-1};

    ~BatboxProcess() { cleanup(); }

    // Non-copyable; movable.
    BatboxProcess() = default;
    BatboxProcess(const BatboxProcess&) = delete;
    BatboxProcess& operator=(const BatboxProcess&) = delete;
    BatboxProcess(BatboxProcess&& o) noexcept
        : pid(o.pid), stdin_fd(o.stdin_fd), stdout_fd(o.stdout_fd), stderr_fd(o.stderr_fd) {
        o.pid = -1; o.stdin_fd = -1; o.stdout_fd = -1; o.stderr_fd = -1;
    }
    BatboxProcess& operator=(BatboxProcess&& o) noexcept {
        if (this != &o) {
            cleanup();
            pid = o.pid; stdin_fd = o.stdin_fd; stdout_fd = o.stdout_fd; stderr_fd = o.stderr_fd;
            o.pid = -1; o.stdin_fd = -1; o.stdout_fd = -1; o.stderr_fd = -1;
        }
        return *this;
    }

    bool running() const { return pid > 0; }

    void cleanup() {
        if (stdin_fd >= 0)  { ::close(stdin_fd);  stdin_fd  = -1; }
        if (stdout_fd >= 0) { ::close(stdout_fd); stdout_fd = -1; }
        if (stderr_fd >= 0) { ::close(stderr_fd); stderr_fd = -1; }
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            int st = 0;
            ::waitpid(pid, &st, 0);
            pid = -1;
        }
    }
};

// ---------------------------------------------------------------------------
// spawn_batbox — fork-exec the batbox binary.
//
// @param batbox_path   Absolute path to batbox binary.
// @param argv_extra    Extra arguments (after "batbox").
// @param env_extra     Additional env key=value pairs; override parent env.
// @param provide_stdin If true, creates a pipe for stdin; child gets it.
//                      If false, child stdin is /dev/null.
// @return BatboxProcess (invalid on error).
// ---------------------------------------------------------------------------
inline BatboxProcess spawn_batbox(
    const std::string&              batbox_path,
    const std::vector<std::string>& argv_extra,
    const std::vector<std::string>& env_extra,
    bool                            provide_stdin = false)
{
    BatboxProcess proc;
    if (batbox_path.empty()) return proc;

    // Build argv.
    std::vector<const char*> argv_vec;
    argv_vec.push_back(batbox_path.c_str());
    for (const auto& a : argv_extra) argv_vec.push_back(a.c_str());
    argv_vec.push_back(nullptr);

    // Build environment: inherit parent + overlay env_extra.
    extern char** environ;
    std::vector<std::string> env_strings;
    for (char** e = environ; *e; ++e) {
        std::string kv(*e);
        bool skip = false;
        for (const auto& ov : env_extra) {
            auto eq = ov.find('=');
            if (eq == std::string::npos) continue;
            std::string key = ov.substr(0, eq + 1);
            if (kv.rfind(key, 0) == 0) { skip = true; break; }
        }
        if (!skip) env_strings.push_back(kv);
    }
    for (const auto& kv : env_extra) env_strings.push_back(kv);
    std::vector<const char*> envp;
    for (const auto& s : env_strings) envp.push_back(s.c_str());
    envp.push_back(nullptr);

    // Create stdout and stderr pipes.
    int stdout_pipe[2], stderr_pipe[2], stdin_pipe[2] = {-1, -1};
    if (::pipe(stdout_pipe) != 0 || ::pipe(stderr_pipe) != 0) return proc;
    if (provide_stdin && ::pipe(stdin_pipe) != 0) {
        ::close(stdout_pipe[0]); ::close(stdout_pipe[1]);
        ::close(stderr_pipe[0]); ::close(stderr_pipe[1]);
        return proc;
    }

    pid_t child = ::fork();
    if (child < 0) {
        ::close(stdout_pipe[0]); ::close(stdout_pipe[1]);
        ::close(stderr_pipe[0]); ::close(stderr_pipe[1]);
        if (provide_stdin) { ::close(stdin_pipe[0]); ::close(stdin_pipe[1]); }
        return proc;
    }

    if (child == 0) {
        // Child: wire pipes.
        ::dup2(stdout_pipe[1], STDOUT_FILENO);
        ::dup2(stderr_pipe[1], STDERR_FILENO);
        if (provide_stdin) {
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

    // Parent: keep read ends.
    ::close(stdout_pipe[1]);
    ::close(stderr_pipe[1]);
    if (provide_stdin) ::close(stdin_pipe[0]);

    proc.pid      = child;
    proc.stdout_fd = stdout_pipe[0];
    proc.stderr_fd = stderr_pipe[0];
    proc.stdin_fd  = provide_stdin ? stdin_pipe[1] : -1;

    // Make stdout/stderr non-blocking.
    ::fcntl(proc.stdout_fd, F_SETFL, ::fcntl(proc.stdout_fd, F_GETFL) | O_NONBLOCK);
    ::fcntl(proc.stderr_fd, F_SETFL, ::fcntl(proc.stderr_fd, F_GETFL) | O_NONBLOCK);

    return proc;
}

// ---------------------------------------------------------------------------
// send_line — write a line (appends '\n') to the child's stdin.
// Returns true on success.
// ---------------------------------------------------------------------------
inline bool send_line(BatboxProcess& proc, const std::string& line) {
    if (proc.stdin_fd < 0) return false;
    std::string data = line + "\n";
    const char* ptr = data.c_str();
    ssize_t remaining = static_cast<ssize_t>(data.size());
    while (remaining > 0) {
        ssize_t n = ::write(proc.stdin_fd, ptr, static_cast<std::size_t>(remaining));
        if (n <= 0) return false;
        ptr += n;
        remaining -= n;
    }
    return true;
}

// ---------------------------------------------------------------------------
// read_until — drain stdout until marker appears or timeout expires.
//
// Accumulates all stdout text into @out.  Returns true if marker was found
// within the timeout.
// ---------------------------------------------------------------------------
inline bool read_until(BatboxProcess& proc, const std::string& marker,
                       std::string& out,
                       std::chrono::milliseconds timeout = std::chrono::milliseconds(10000))
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    char buf[4096];
    while (std::chrono::steady_clock::now() < deadline) {
        ssize_t n = ::read(proc.stdout_fd, buf, sizeof(buf));
        if (n > 0) {
            out.append(buf, static_cast<std::size_t>(n));
            if (out.find(marker) != std::string::npos) return true;
        } else if (n == 0) {
            break; // EOF
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return out.find(marker) != std::string::npos;
}

// ---------------------------------------------------------------------------
// RunResult — captured output of a finished batbox run.
// ---------------------------------------------------------------------------
struct RunResult {
    int         exit_code{-1};
    std::string stdout_text;
    std::string stderr_text;
};

// ---------------------------------------------------------------------------
// wait_for_exit — wait for child to exit, draining stdout/stderr.
// Sends SIGTERM if timeout expires.
// Returns RunResult with captured output and exit code.
// ---------------------------------------------------------------------------
inline RunResult wait_for_exit(BatboxProcess& proc,
                               std::chrono::milliseconds timeout = std::chrono::milliseconds(30000))
{
    RunResult result;

    // Drain helper.
    auto drain = [&]() {
        char buf[4096];
        ssize_t n;
        while ((n = ::read(proc.stdout_fd, buf, sizeof(buf))) > 0)
            result.stdout_text.append(buf, static_cast<std::size_t>(n));
        while ((n = ::read(proc.stderr_fd, buf, sizeof(buf))) > 0)
            result.stderr_text.append(buf, static_cast<std::size_t>(n));
    };

    int wstatus = 0;
    auto deadline = std::chrono::steady_clock::now() + timeout;
    pid_t child = proc.pid;

    while (std::chrono::steady_clock::now() < deadline) {
        drain();
        pid_t w = ::waitpid(child, &wstatus, WNOHANG);
        if (w == child) { child = -1; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (child > 0) {
        ::kill(child, SIGTERM);
        ::waitpid(child, &wstatus, 0);
    }
    drain();

    // Prevent BatboxProcess destructor from double-reaping.
    proc.pid = -1;
    if (proc.stdout_fd >= 0) { ::close(proc.stdout_fd); proc.stdout_fd = -1; }
    if (proc.stderr_fd >= 0) { ::close(proc.stderr_fd); proc.stderr_fd = -1; }
    if (proc.stdin_fd  >= 0) { ::close(proc.stdin_fd);  proc.stdin_fd  = -1; }

    if (WIFEXITED(wstatus))   result.exit_code = WEXITSTATUS(wstatus);
    else if (WIFSIGNALED(wstatus)) result.exit_code = 128 + WTERMSIG(wstatus);

    return result;
}

// ---------------------------------------------------------------------------
// kill_proc — send SIGTERM to the child process without reaping.
// ---------------------------------------------------------------------------
inline void kill_proc(BatboxProcess& proc, int sig = SIGTERM) {
    if (proc.pid > 0) ::kill(proc.pid, sig);
}

// ---------------------------------------------------------------------------
// run_batbox_simple — convenience: spawn + wait, returning RunResult.
// Suitable for fire-and-forget test invocations (e.g. --print mode).
// ---------------------------------------------------------------------------
inline RunResult run_batbox_simple(
    const std::string&              batbox_path,
    const std::vector<std::string>& argv_extra,
    const std::vector<std::string>& env_extra,
    const std::string&              stdin_data = {},
    std::chrono::milliseconds       timeout    = std::chrono::milliseconds(30000))
{
    RunResult result;
    if (batbox_path.empty()) { result.exit_code = -2; result.stderr_text = "batbox not found"; return result; }

    // Build argv.
    std::vector<const char*> argv_vec;
    argv_vec.push_back(batbox_path.c_str());
    for (const auto& a : argv_extra) argv_vec.push_back(a.c_str());
    argv_vec.push_back(nullptr);

    // Build env.
    extern char** environ;
    std::vector<std::string> env_strings;
    for (char** e = environ; *e; ++e) {
        std::string kv(*e);
        bool skip = false;
        for (const auto& ov : env_extra) {
            auto eq = ov.find('=');
            if (eq == std::string::npos) continue;
            std::string key = ov.substr(0, eq + 1);
            if (kv.rfind(key, 0) == 0) { skip = true; break; }
        }
        if (!skip) env_strings.push_back(kv);
    }
    for (const auto& kv : env_extra) env_strings.push_back(kv);
    std::vector<const char*> envp;
    for (const auto& s : env_strings) envp.push_back(s.c_str());
    envp.push_back(nullptr);

    // Pipes.
    int stdout_pipe[2], stderr_pipe[2], stdin_pipe[2] = {-1,-1};
    bool has_stdin = !stdin_data.empty();
    if (::pipe(stdout_pipe) != 0 || ::pipe(stderr_pipe) != 0) {
        result.exit_code = -3; result.stderr_text = "pipe() failed"; return result;
    }
    if (has_stdin && ::pipe(stdin_pipe) != 0) {
        ::close(stdout_pipe[0]); ::close(stdout_pipe[1]);
        ::close(stderr_pipe[0]); ::close(stderr_pipe[1]);
        result.exit_code = -3; result.stderr_text = "stdin pipe() failed"; return result;
    }

    pid_t child = ::fork();
    if (child < 0) { result.exit_code = -4; result.stderr_text = "fork() failed"; return result; }

    if (child == 0) {
        ::dup2(stdout_pipe[1], STDOUT_FILENO);
        ::dup2(stderr_pipe[1], STDERR_FILENO);
        if (has_stdin) {
            ::dup2(stdin_pipe[0], STDIN_FILENO);
            ::close(stdin_pipe[0]); ::close(stdin_pipe[1]);
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

    if (has_stdin) {
        ::close(stdin_pipe[0]);
        const char* ptr = stdin_data.c_str();
        ssize_t rem = static_cast<ssize_t>(stdin_data.size());
        while (rem > 0) {
            ssize_t n = ::write(stdin_pipe[1], ptr, static_cast<std::size_t>(rem));
            if (n <= 0) break;
            ptr += n; rem -= n;
        }
        ::close(stdin_pipe[1]);
    }

    ::fcntl(stdout_pipe[0], F_SETFL, ::fcntl(stdout_pipe[0], F_GETFL) | O_NONBLOCK);
    ::fcntl(stderr_pipe[0], F_SETFL, ::fcntl(stderr_pipe[0], F_GETFL) | O_NONBLOCK);

    auto drain = [&]() {
        char buf[4096]; ssize_t n;
        while ((n = ::read(stdout_pipe[0], buf, sizeof(buf))) > 0) result.stdout_text.append(buf, static_cast<std::size_t>(n));
        while ((n = ::read(stderr_pipe[0], buf, sizeof(buf))) > 0) result.stderr_text.append(buf, static_cast<std::size_t>(n));
    };

    int wstatus = 0;
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        drain();
        pid_t w = ::waitpid(child, &wstatus, WNOHANG);
        if (w == child) { child = -1; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (child > 0) { ::kill(child, SIGTERM); ::waitpid(child, &wstatus, 0); }
    drain();

    ::close(stdout_pipe[0]);
    ::close(stderr_pipe[0]);

    if (WIFEXITED(wstatus))        result.exit_code = WEXITSTATUS(wstatus);
    else if (WIFSIGNALED(wstatus)) result.exit_code = 128 + WTERMSIG(wstatus);

    return result;
}
