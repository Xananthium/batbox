// src/tools/bash/PtyBackend.cpp
//
// batbox::tools::bash::pty_run — forkpty-based command execution backend.
//
// Blueprint contract: CPP 5.8 — PtyBackend (file symbol, src/tools/bash/PtyBackend.cpp)
// Pseudocode: forkpty-based pty allocation; child: setpgid+chdir+scrub_env+execvp;
//             parent: drain reader thread
//
// Sequence:
//   1. forkpty() allocates a pty and forks.
//   2. Child  (pid==0): setpgid(0,0), chdir(cwd), build null-terminated envp from
//      scrubbed env, execvp(shell, {"-c", command}).
//   3. Parent: starts reader_thread and watchdog_thread.
//      reader_thread  — polls master_fd with select(); pushes chunks into
//                       output_buf (protected by mutex) up to max_output_bytes;
//                       discards excess but keeps draining to avoid child block.
//      watchdog_thread— if timeout_sec > 0, sleeps until timeout; fires
//                       killpg(pgid, SIGTERM), waits 2 s, killpg(pgid, SIGKILL).
//   4. cancel_token callback: on_cancel fires killpg(pgid, SIGINT); if triggered
//      again within 2 s fires killpg(pgid, SIGKILL).
//   5. waitpid(pid, &wstatus) collects exit code.
//   6. Returns PtyResult{output, exit_code, truncated, duration}.

#include "PtyBackendInternal.hpp"
#include "AnsiStripInternal.hpp"
#include "EnvScrubInternal.hpp"

#include <batbox/core/CancelToken.hpp>

// POSIX / macOS headers
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

// forkpty() lives in different headers per platform:
//   Linux (glibc)   → <pty.h>
//   macOS / *BSD    → <util.h>
#if defined(__linux__)
#  include <pty.h>
#else
#  include <util.h>
#endif

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace batbox::tools::bash {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

/// Build a null-terminated argv array for execvp.
/// Returns the array; the char* pointers point into @p cmd and kDashC.
std::vector<char*> build_argv(const std::string& shell, const std::string& cmd) {
    static const char kDashC[] = "-c";
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(shell.c_str()));
    argv.push_back(const_cast<char*>(kDashC));
    argv.push_back(const_cast<char*>(cmd.c_str()));
    argv.push_back(nullptr);
    return argv;
}

/// Build a null-terminated envp array from a vector of "KEY=VALUE" strings.
std::vector<char*> build_envp(const std::vector<std::string>& env) {
    std::vector<char*> envp;
    envp.reserve(env.size() + 1);
    for (const auto& kv : env) {
        envp.push_back(const_cast<char*>(kv.c_str()));
    }
    envp.push_back(nullptr);
    return envp;
}

/// Detect the shell to use: $SHELL from the env, defaulting to /bin/sh.
std::string detect_shell(const std::vector<std::string>& scrubbed_env) {
    for (const auto& kv : scrubbed_env) {
        if (kv.size() > 6 && kv.substr(0, 6) == "SHELL=") {
            return kv.substr(6);
        }
    }
    return "/bin/sh";
}

/// Close an fd if valid, ignoring errors.
void safe_close(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

/// Set a file descriptor to non-blocking mode.
void set_nonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// pty_run()
// ---------------------------------------------------------------------------
PtyResult pty_run(
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
    const std::string shell = detect_shell(scrubbed_env);

    // ------------------------------------------------------------------
    // Set up terminal dimensions (80x24 is a safe default).
    // ------------------------------------------------------------------
    struct winsize ws{};
    ws.ws_row = 24;
    ws.ws_col = 80;

    int master_fd = -1;
    pid_t child_pid = ::forkpty(&master_fd, nullptr, nullptr, &ws);

    if (child_pid < 0) {
        // forkpty() failed — return error immediately.
        PtyResult r;
        r.output     = "forkpty() failed: " + std::string(std::strerror(errno));
        r.exit_code  = -1;
        r.truncated  = false;
        r.duration   = std::chrono::duration_cast<std::chrono::milliseconds>(
                           Clock::now() - start_time);
        return r;
    }

    if (child_pid == 0) {
        // ==============================================================
        // CHILD PROCESS
        // ==============================================================

        // Create a new process group so we can signal the whole group.
        ::setpgid(0, 0);

        // Change to requested working directory; on failure the command
        // will simply run in whatever cwd forkpty left us.
        if (!cwd.empty()) {
            ::chdir(cwd.c_str());
        }

        // Build argv and envp.
        auto argv_vec = build_argv(shell, command);
        auto envp_vec = build_envp(scrubbed_env);

        // execve: replace this child image with the shell.
        // execvpe is Linux-only; on macOS we use execve with the resolved
        // shell path (which scrub_env/detect_shell already validated from SHELL env).
        ::execve(argv_vec[0], argv_vec.data(), envp_vec.data());

        // If execve fails, the shell path may not be absolute — try execvp
        // which does PATH search. We pass the scrubbed envp via environ trick:
        // set environ pointer and use execvp (POSIX extension on many platforms).
        // Fall back: use /bin/sh.
        argv_vec[0] = const_cast<char*>("/bin/sh");
        ::execve("/bin/sh", argv_vec.data(), envp_vec.data());

        // If both fail, write error and exit.
        const char* err_msg = "exec failed\n";
        ::write(STDOUT_FILENO, err_msg, std::strlen(err_msg));
        ::_exit(127);
    }

    // ==================================================================
    // PARENT PROCESS
    // ==================================================================

    // Remember child's pgid (== child_pid because child called setpgid(0,0)).
    const pid_t child_pgid = child_pid;

    // Set master_fd non-blocking so the reader thread can poll without
    // getting stuck on a blocking read after the child exits.
    set_nonblocking(master_fd);

    // ------------------------------------------------------------------
    // Shared state between parent, reader thread, and watchdog thread.
    // ------------------------------------------------------------------
    std::mutex          buf_mutex;
    std::string         output_buf;
    output_buf.reserve(std::min(max_output_bytes > 0 ? max_output_bytes : std::size_t(65536),
                                std::size_t(1024 * 1024)));
    bool                truncated      = false;
    std::size_t         total_bytes    = 0;

    // Atomic flag: set to true when the child process has been reaped so
    // the reader thread knows to stop.
    std::atomic<bool>   child_done{false};

    // ------------------------------------------------------------------
    // Cancel-token callback: SIGINT → pgid; second cancel → SIGKILL.
    // ------------------------------------------------------------------
    std::atomic<int>    sigint_count{0};
    auto cancel_handle = cancel_token.on_cancel([&]() {
        int count = ++sigint_count;
        if (count == 1) {
            ::killpg(child_pgid, SIGINT);
            // Schedule a SIGKILL if a second cancel comes within 2 s on a
            // detached thread. We use a simple detached thread here — it
            // will finish regardless of what happens to the parent.
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
    (void)cancel_handle; // keep alive until function returns

    // ------------------------------------------------------------------
    // Reader thread — drains master_fd.
    // ------------------------------------------------------------------
    std::thread reader_thread([&]() {
        char read_buf[4096];

        while (!child_done.load(std::memory_order_relaxed)) {
            // Use select() with a short timeout so we can check child_done.
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(master_fd, &rfds);
            struct timeval tv{};
            tv.tv_sec  = 0;
            tv.tv_usec = 20'000; // 20 ms poll

            int sel = ::select(master_fd + 1, &rfds, nullptr, nullptr, &tv);
            if (sel <= 0) continue; // timeout or EINTR

            ssize_t n = ::read(master_fd, read_buf, sizeof(read_buf));
            if (n <= 0) {
                // EIO is normal on macOS when pty slave is closed.
                break;
            }

            std::lock_guard<std::mutex> lk(buf_mutex);
            total_bytes += static_cast<std::size_t>(n);

            if (max_output_bytes == 0 || total_bytes <= max_output_bytes) {
                output_buf.append(read_buf, static_cast<std::size_t>(n));
            } else if (!truncated) {
                truncated = true;
                // Keep whatever we already buffered; discard the rest.
            }
            // Keep reading (and discarding) so the child is never blocked
            // on a full pty buffer.
        }

        // Drain any remaining bytes after child exits.
        while (true) {
            ssize_t n = ::read(master_fd, read_buf, sizeof(read_buf));
            if (n <= 0) break;
            std::lock_guard<std::mutex> lk(buf_mutex);
            total_bytes += static_cast<std::size_t>(n);
            if (max_output_bytes == 0 || total_bytes <= max_output_bytes) {
                output_buf.append(read_buf, static_cast<std::size_t>(n));
            }
        }
    });

    // ------------------------------------------------------------------
    // Watchdog thread — SIGTERM at timeout, SIGKILL 2 s later.
    // ------------------------------------------------------------------
    std::atomic<bool> watchdog_arm{timeout_sec > 0};
    std::thread watchdog_thread([&]() {
        if (!watchdog_arm.load()) return;

        // Sleep in 50 ms intervals so we can detect early child exit.
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

    // Disarm watchdog.
    watchdog_arm.store(false);

    // Close master fd so the reader thread's select/read sees EOF.
    safe_close(master_fd);

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
    // Post-process: ANSI strip.
    // ------------------------------------------------------------------
    std::string stripped;
    {
        std::lock_guard<std::mutex> lk(buf_mutex);
        stripped = ansi_strip(output_buf);
    }

    // ------------------------------------------------------------------
    // Build result.
    // ------------------------------------------------------------------
    PtyResult result;
    result.exit_code  = exit_code;
    result.truncated  = truncated;
    result.duration   = std::chrono::duration_cast<std::chrono::milliseconds>(
                            Clock::now() - start_time);

    // Normalise trailing whitespace then append trailer.
    // Remove trailing newlines/spaces so the trailer is on its own line.
    while (!stripped.empty()
           && (stripped.back() == '\n' || stripped.back() == '\r'
               || stripped.back() == ' ')) {
        stripped.pop_back();
    }
    if (!stripped.empty()) stripped += '\n';

    if (truncated) {
        // Format truncation notice (round down to nearest MB or KB).
        const std::size_t cap_bytes = total_bytes;
        std::string trunc_notice;
        if (cap_bytes >= 1024 * 1024) {
            trunc_notice = "(output truncated at "
                         + std::to_string(cap_bytes / (1024 * 1024))
                         + " MB)";
        } else {
            trunc_notice = "(output truncated at "
                         + std::to_string(cap_bytes / 1024)
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
