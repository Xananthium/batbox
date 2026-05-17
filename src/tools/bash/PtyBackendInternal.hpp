// src/tools/bash/PtyBackendInternal.hpp
//
// Internal header for PtyBackend — used only within the bash sub-library.

#pragma once

#include <batbox/core/CancelToken.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace batbox::tools::bash {

/// Result returned by pty_run().
struct PtyResult {
    /// Combined stdout+stderr, ANSI-stripped, with trailer appended:
    ///   "[exit=N, duration=Xms]"
    /// If truncated, a "(output truncated at N MB)" line precedes the trailer.
    std::string output;

    /// Process exit code; negative if killed by signal (-signum).
    int exit_code = 0;

    /// True if output was truncated due to max_output_bytes.
    bool truncated = false;

    /// Wall-clock duration of the run (includes watchdog/cancel overhead).
    std::chrono::milliseconds duration{0};
};

/// Allocate a pty via forkpty(), exec the command, drain output, return result.
///
/// @param command          Shell command string (passed to shell -c)
/// @param cwd              Working directory for the child
/// @param env_allowlist    Keys to keep in child environment (empty = defaults)
/// @param timeout_sec      Watchdog timeout; 0 = unlimited
/// @param max_output_bytes Output cap; 0 = unlimited
/// @param cancel_token     Fires SIGINT → pgid; double-cancel → SIGKILL
[[nodiscard]] PtyResult pty_run(
    const std::string&              command,
    const std::filesystem::path&    cwd,
    const std::vector<std::string>& env_allowlist,
    int                             timeout_sec,
    std::size_t                     max_output_bytes,
    CancelToken&                    cancel_token);

} // namespace batbox::tools::bash
