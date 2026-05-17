// include/batbox/tools/bash/BashRunner.hpp
//
// batbox::tools::bash::BashRunner — forkpty-based shell command executor.
//
// Blueprint contract: CPP 5.8 — BashRunner (file symbol, src/tools/bash/BashRunner.cpp)
//
// Design (ned-cpp.md §2.C5 BashTool):
//   BashRunner::run(command, cwd, env_allowlist, timeout_sec, max_output_bytes, cancel_token)
//     → BashResult
//
//   Internally delegates to PtyBackend (forkpty) for all execution.
//   EnvScrub filters the environment before child exec.
//   AnsiStrip removes escape sequences from captured output.
//
// Signal handling:
//   - Watchdog fires SIGTERM at timeout_sec, SIGKILL 2 s later.
//   - cancel_token fires SIGINT to process group; second cancel within 2 s → SIGKILL.
//
// Output cap:
//   - Parent reader thread accumulates bytes; once max_output_bytes is hit,
//     remaining bytes are discarded but the pty fd is drained to avoid blocking child.
//   - A truncation notice "(output truncated at N MB)" is appended to body.
//
// Plan-mode guard:
//   - run() returns BashResult::plan_mode_error() immediately if plan_mode == true.

#pragma once

#include <batbox/core/CancelToken.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace batbox::tools::bash {

// =============================================================================
// BashResult — return type from BashRunner::run()
// =============================================================================

struct BashResult {
    /// Combined stdout+stderr from the child (ANSI-stripped).
    /// Ends with "\n[exit=N, duration=Xms]" trailer.
    std::string body;

    /// True when the process exited with a non-zero status or was killed.
    bool is_error = false;

    /// Raw exit status from waitpid() (or -1 if killed by signal).
    int exit_code = 0;

    /// Wall-clock duration of the run.
    std::chrono::milliseconds duration{0};

    // -----------------------------------------------------------------------
    // Named constructors
    // -----------------------------------------------------------------------

    [[nodiscard]] static BashResult plan_mode_error() {
        BashResult r;
        r.body     = "Error: BashRunner is not available in plan mode.";
        r.is_error = true;
        r.exit_code = -1;
        return r;
    }
};

// =============================================================================
// BashRunner — stateless executor; all state lives in the run() call stack.
// =============================================================================

class BashRunner {
public:
    BashRunner()  = default;
    ~BashRunner() = default;

    // Non-copyable, non-movable (stateless singleton-style usage).
    BashRunner(const BashRunner&)            = delete;
    BashRunner& operator=(const BashRunner&) = delete;
    BashRunner(BashRunner&&)                 = delete;
    BashRunner& operator=(BashRunner&&)      = delete;

    // -------------------------------------------------------------------------
    // run()
    //
    // @param command          Shell command string passed to /bin/sh -c
    // @param cwd              Working directory for the child process
    // @param env_allowlist    Environment variable names to preserve; all
    //                         others are removed (EnvScrub). Pass empty to
    //                         use the default allowlist
    //                         {PATH, HOME, USER, LANG, TERM, SHELL}.
    // @param timeout_sec      Seconds before watchdog fires SIGTERM+SIGKILL.
    //                         0 = no timeout.
    // @param max_output_bytes Maximum accumulated output bytes before truncation.
    //                         0 = no limit.
    // @param cancel_token     Cooperative cancellation; fires SIGINT → pgid
    // @param plan_mode        When true, refuses to execute (returns error body).
    //
    // @returns BashResult with body, is_error, exit_code, duration.
    // -------------------------------------------------------------------------
    [[nodiscard]] BashResult run(
        const std::string&                command,
        const std::filesystem::path&      cwd,
        const std::vector<std::string>&   env_allowlist,
        int                               timeout_sec,
        std::size_t                       max_output_bytes,
        CancelToken&                      cancel_token,
        bool                              plan_mode = false
    ) const;
};

} // namespace batbox::tools::bash
