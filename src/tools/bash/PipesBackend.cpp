// src/tools/bash/PipesBackend.cpp
//
// batbox::tools::bash::pipes_run — pipe-pair-based command execution backend.
//
// Blueprint contract: CPP 5.9 — PipesBackend (file symbol,
//                     src/tools/bash/PipesBackend.cpp)
// Pseudocode: pipe fallback backend when forkpty unavailable
//
// Sequence:
//   1. Create a single pipe (read_fd, write_fd).
//      read_fd: O_NONBLOCK + FD_CLOEXEC.
//      write_fd: FD_CLOEXEC so it auto-closes in the parent after fork.
//   2. fork().
//   3. Child  (pid==0):
//        setpgid(0,0)           — new process group
//        chdir(cwd)             — working directory
//        dup2(write_fd, STDOUT) — redirect stdout
//        dup2(write_fd, STDERR) — redirect stderr (both share same pipe)
//        close(write_fd)        — no extra reference
//        close(read_fd)         — child does not read
//        build scrubbed envp via scrub_env()
//        execve(shell, {"-c", command}, envp)
//        fallback to /bin/sh if execve fails
//        _exit(127) if all exec attempts fail
//   4. Parent:
//        close(write_fd)        — must close so EOF arrives on read_fd
//        starts reader_thread and watchdog_thread (same structure as PtyBackend)
//        reader_thread  — select() on read_fd; accumulates into output_buf up to
//                         max_output_bytes; keeps draining to avoid pipe-full stall.
//        watchdog_thread— SIGTERM at timeout_sec, SIGKILL 2 s later.
//   5. cancel_token callback: on_cancel → killpg(pgid, SIGINT);
//      second cancel within 2 s → SIGKILL (same as PtyBackend).
//   6. waitpid(pid) collects exit code.
//   7. Drain remaining pipe bytes; ANSI-strip; append trailer; return PipesResult.

#include "PipesBackendInternal.hpp"
#include "AnsiStripInternal.hpp"
#include "EnvScrubInternal.hpp"

#include <batbox/core/CancelToken.hpp>

// POSIX headers
#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace batbox::tools::bash {

// ---------------------------------------------------------------------------
// Internal helpers  (anonymous namespace — same style as PtyBackend)
// ---------------------------------------------------------------------------

namespace {

/// Build a null-terminated argv array for execve.
std::vector<char*> pipes_build_argv(const std::string& shell,
                                     const std::string& cmd)
{
    static const char kDashC[] = "-c";
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(shell.c_str()));
    argv.push_back(const_cast<char*>(kDashC));
    argv.push_back(const_cast<char*>(cmd.c_str()));
    argv.push_back(nullptr);
    return argv;
}

/// Build a null-terminated envp array from a vector of "KEY=VALUE" strings.
std::vector<char*> pipes_build_envp(const std::vector<std::string>& env)
{
    std::vector<char*> envp;
    envp.reserve(env.size() + 1);
    for (const auto& kv : env) {
        envp.push_back(const_cast<char*>(kv.c_str()));
    }
    envp.push_back(nullptr);
    return envp;
}

/// Detect the shell from the scrubbed environment, defaulting to /bin/sh.
std::string pipes_detect_shell(const std::vector<std::string>& scrubbed_env)
{
    for (const auto& kv : scrubbed_env) {
        if (kv.size() > 6 && kv.substr(0, 6) == "SHELL=") {
            return kv.substr(6);
        }
    }
    return "/bin/sh";
}

/// Close an fd if valid, ignoring errors.
void pipes_safe_close(int& fd)
{
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

/// Set a file descriptor to non-blocking mode.
void pipes_set_nonblocking(int fd)
{
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

/// Set FD_CLOEXEC on a file descriptor.
void pipes_set_cloexec(int fd)
{
    int flags = ::fcntl(fd, F_GETFD, 0);
    if (flags >= 0) {
        ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// pipes_run()
// ---------------------------------------------------------------------------

PipesResult pipes_run(
    const std::string&              command,
    const std::filesystem::path&    cwd,
    const std::vector<std::string>& env_allowlist,
    int                             timeout_sec,
    std::size_t                     max_output_bytes,
    CancelToken&                    cancel_token)
{
    using Clock = std::chrono::steady_clock;
    const auto start_time = Clock::now();

    // ------------------------------------------------------------------
    // Scrub environment before forking.
    // ------------------------------------------------------------------
    std::vector<std::string> scrubbed_env = scrub_env(env_allowlist);
    const std::string shell = pipes_detect_shell(scrubbed_env);

    // ------------------------------------------------------------------
    // Create the output pipe.
    //   out_pipe[0] = read end  (parent reads child output)
    //   out_pipe[1] = write end (child writes stdout+stderr)
    //
    // pipe2() is Linux-only; on macOS we use pipe() + fcntl().
    // ------------------------------------------------------------------
    int out_pipe[2] = {-1, -1};
    if (::pipe(out_pipe) != 0) {
        PipesResult r;
        r.output    = "pipe() failed: " + std::string(std::strerror(errno));
        r.exit_code = -1;
        r.truncated = false;
        r.duration  = std::chrono::duration_cast<std::chrono::milliseconds>(
                          Clock::now() - start_time);
        return r;
    }

    // Read end: non-blocking + FD_CLOEXEC (parent side; child closes it).
    pipes_set_nonblocking(out_pipe[0]);
    pipes_set_cloexec(out_pipe[0]);

    // Write end: FD_CLOEXEC so it is closed automatically in the parent
    // after fork() without needing an explicit close in the parent's
    // exec path.  The child clears this flag via dup2 (dup2 does not
    // inherit FD_CLOEXEC on the new fd).
    pipes_set_cloexec(out_pipe[1]);

    // ------------------------------------------------------------------
    // fork()
    // ------------------------------------------------------------------
    pid_t child_pid = ::fork();

    if (child_pid < 0) {
        // fork() failed — close pipe fds and return error.
        pipes_safe_close(out_pipe[0]);
        pipes_safe_close(out_pipe[1]);

        PipesResult r;
        r.output    = "fork() failed: " + std::string(std::strerror(errno));
        r.exit_code = -1;
        r.truncated = false;
        r.duration  = std::chrono::duration_cast<std::chrono::milliseconds>(
                          Clock::now() - start_time);
        return r;
    }

    if (child_pid == 0) {
        // ==============================================================
        // CHILD PROCESS
        // ==============================================================

        // New process group so we can signal the whole group.
        ::setpgid(0, 0);

        // Change to requested working directory.
        if (!cwd.empty()) {
            ::chdir(cwd.c_str());
        }

        // Redirect stdout and stderr to write end of the pipe.
        // dup2() clears FD_CLOEXEC on the new fd number automatically.
        ::dup2(out_pipe[1], STDOUT_FILENO);
        ::dup2(out_pipe[1], STDERR_FILENO);

        // Close original pipe fds — dup2 already transferred them.
        // out_pipe[1] close: safe because dup2 duplicated it.
        // out_pipe[0] close: child does not read.
        ::close(out_pipe[1]);
        ::close(out_pipe[0]);

        // Build argv and envp.
        auto argv_vec = pipes_build_argv(shell, command);
        auto envp_vec = pipes_build_envp(scrubbed_env);

        // Execute the shell.
        ::execve(argv_vec[0], argv_vec.data(), envp_vec.data());

        // execve failed — try /bin/sh as absolute fallback.
        argv_vec[0] = const_cast<char*>("/bin/sh");
        ::execve("/bin/sh", argv_vec.data(), envp_vec.data());

        // Both failed — write error to the pipe (which is now stdout).
        const char* err_msg = "exec failed\n";
        ::write(STDOUT_FILENO, err_msg, std::strlen(err_msg));
        ::_exit(127);
    }

    // ==================================================================
    // PARENT PROCESS
    // ==================================================================

    // Close the write end — critical: without this, read() on the read
    // end never sees EOF (the parent holds a reference to the write end).
    pipes_safe_close(out_pipe[1]);

    // Child's pgid == child_pid because child called setpgid(0,0).
    const pid_t child_pgid = child_pid;
    const int   read_fd    = out_pipe[0];

    // ------------------------------------------------------------------
    // Shared state between parent, reader thread, and watchdog thread.
    // ------------------------------------------------------------------
    std::mutex        buf_mutex;
    std::string       output_buf;
    output_buf.reserve(std::min(
        max_output_bytes > 0 ? max_output_bytes : std::size_t(65536),
        std::size_t(1024 * 1024)));
    bool              truncated    = false;
    std::size_t       total_bytes  = 0;

    std::atomic<bool> child_done{false};

    // ------------------------------------------------------------------
    // Cancel-token callback: SIGINT → pgid; second cancel → SIGKILL.
    // (Identical semantics to PtyBackend.)
    // ------------------------------------------------------------------
    std::atomic<int> sigint_count{0};
    auto cancel_handle = cancel_token.on_cancel([&]() {
        int count = ++sigint_count;
        if (count == 1) {
            ::killpg(child_pgid, SIGINT);
            std::thread([child_pgid, &sigint_count]() {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                if (sigint_count.load() >= 2) {
                    ::killpg(child_pgid, SIGKILL);
                }
            }).detach();
        } else {
            ::killpg(child_pgid, SIGKILL);
        }
    });
    (void)cancel_handle; // keep RAII handle alive until function returns

    // ------------------------------------------------------------------
    // Reader thread — drains read_fd.
    // ------------------------------------------------------------------
    std::thread reader_thread([&]() {
        char read_buf[4096];

        while (!child_done.load(std::memory_order_relaxed)) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(read_fd, &rfds);
            struct timeval tv{};
            tv.tv_sec  = 0;
            tv.tv_usec = 20'000; // 20 ms poll

            int sel = ::select(read_fd + 1, &rfds, nullptr, nullptr, &tv);
            if (sel <= 0) continue; // timeout or EINTR

            ssize_t n = ::read(read_fd, read_buf, sizeof(read_buf));
            if (n <= 0) {
                // EOF (n==0) or error: child closed its write end.
                break;
            }

            std::lock_guard<std::mutex> lk(buf_mutex);
            total_bytes += static_cast<std::size_t>(n);

            if (max_output_bytes == 0 || total_bytes <= max_output_bytes) {
                output_buf.append(read_buf, static_cast<std::size_t>(n));
            } else if (!truncated) {
                truncated = true;
                // Keep draining to avoid blocking the child.
            }
        }

        // Drain any remaining bytes after child exits.
        while (true) {
            ssize_t n = ::read(read_fd, read_buf, sizeof(read_buf));
            if (n <= 0) break;
            std::lock_guard<std::mutex> lk(buf_mutex);
            total_bytes += static_cast<std::size_t>(n);
            if (max_output_bytes == 0 || total_bytes <= max_output_bytes) {
                output_buf.append(read_buf, static_cast<std::size_t>(n));
            }
        }
    });

    // ------------------------------------------------------------------
    // Watchdog thread — SIGTERM at timeout_sec, SIGKILL 2 s later.
    // (Identical structure to PtyBackend watchdog.)
    // ------------------------------------------------------------------
    std::atomic<bool> watchdog_arm{timeout_sec > 0};
    std::thread watchdog_thread([&]() {
        if (!watchdog_arm.load()) return;

        const auto deadline = Clock::now() + std::chrono::seconds(timeout_sec);
        while (Clock::now() < deadline) {
            if (child_done.load(std::memory_order_relaxed)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        if (!child_done.load(std::memory_order_relaxed)) {
            ::killpg(child_pgid, SIGTERM);
            std::this_thread::sleep_for(std::chrono::seconds(2));
            if (!child_done.load(std::memory_order_relaxed)) {
                ::killpg(child_pgid, SIGKILL);
            }
        }
    });

    // ------------------------------------------------------------------
    // Wait for child.
    // ------------------------------------------------------------------
    int wstatus = 0;
    ::waitpid(child_pid, &wstatus, 0);
    child_done.store(true, std::memory_order_release);

    // Disarm watchdog so it exits its loop.
    watchdog_arm.store(false);

    // Close read_fd so the reader thread's select/read sees EOF.
    int read_fd_copy = read_fd;
    pipes_safe_close(read_fd_copy);

    // Join threads.
    if (reader_thread.joinable())   reader_thread.join();
    if (watchdog_thread.joinable()) watchdog_thread.join();

    // ------------------------------------------------------------------
    // Compute exit code.
    // ------------------------------------------------------------------
    int exit_code = -1;
    if (WIFEXITED(wstatus)) {
        exit_code = WEXITSTATUS(wstatus);
    } else if (WIFSIGNALED(wstatus)) {
        exit_code = -(WTERMSIG(wstatus)); // negative = killed by signal N
    }

    // ------------------------------------------------------------------
    // Post-process: ANSI strip (defensive; pipe output rarely has ANSI).
    // ------------------------------------------------------------------
    std::string stripped;
    {
        std::lock_guard<std::mutex> lk(buf_mutex);
        stripped = ansi_strip(output_buf);
    }

    // ------------------------------------------------------------------
    // Build result — same trailer format as PtyBackend.
    // ------------------------------------------------------------------
    PipesResult result;
    result.exit_code = exit_code;
    result.truncated = truncated;
    result.duration  = std::chrono::duration_cast<std::chrono::milliseconds>(
                           Clock::now() - start_time);

    // Normalise trailing whitespace then append trailer.
    while (!stripped.empty()
           && (stripped.back() == '\n' || stripped.back() == '\r'
               || stripped.back() == ' ')) {
        stripped.pop_back();
    }
    if (!stripped.empty()) stripped += '\n';

    if (truncated) {
        std::string trunc_notice;
        if (total_bytes >= 1024 * 1024) {
            trunc_notice = "(output truncated at "
                         + std::to_string(total_bytes / (1024 * 1024))
                         + " MB)";
        } else {
            trunc_notice = "(output truncated at "
                         + std::to_string(total_bytes / 1024)
                         + " KB)";
        }
        stripped += trunc_notice + "\n";
    }

    stripped += "[exit=" + std::to_string(exit_code)
              + ", duration=" + std::to_string(result.duration.count()) + "ms]";

    result.output = std::move(stripped);
    return result;
}

} // namespace batbox::tools::bash
