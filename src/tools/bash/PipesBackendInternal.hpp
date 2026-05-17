// src/tools/bash/PipesBackendInternal.hpp
//
// Internal header for PipesBackend — used only within the bash sub-library.
// Not installed to include/batbox/; callers use BashRunner's public API.
//
// Blueprint contract: CPP 5.9 — PipesBackend (file symbol,
//                     src/tools/bash/PipesBackend.cpp)

#pragma once

#include <batbox/core/CancelToken.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace batbox::tools::bash {

/// Result returned by pipes_run().
/// Shares the same shape as PtyResult so BashRunner can convert identically.
struct PipesResult {
    /// Combined stdout+stderr, ANSI-stripped, with trailer:
    ///   "[exit=N, duration=Xms]"
    /// If truncated, "(output truncated at N MB/KB)" precedes the trailer.
    std::string output;

    /// Process exit code; negative if killed by signal (-signum).
    int exit_code = 0;

    /// True if output was capped at max_output_bytes.
    bool truncated = false;

    /// Wall-clock duration of the run.
    std::chrono::milliseconds duration{0};
};

/// Fork + pipe-pair-based command execution backend.
///
/// Used when forkpty() is unavailable (EPERM / ENOTTY) or when
/// BATBOX_BASH_BACKEND=pipes is set.  No pty is allocated; stdout and
/// stderr of the child are both redirected to the write end of a single
/// pipe.  Output therefore contains no terminal-induced ANSI sequences
/// (prompts, colour, etc.) so the ANSI-strip pass is still applied for
/// safety but is usually a no-op.
///
/// @param command          Shell command string (passed to shell -c)
/// @param cwd              Working directory for the child
/// @param env_allowlist    Keys to keep in child environment (empty = defaults)
/// @param timeout_sec      Watchdog timeout; 0 = unlimited
/// @param max_output_bytes Output cap; 0 = unlimited
/// @param cancel_token     Fires SIGINT → pgid; double-cancel → SIGKILL
[[nodiscard]] PipesResult pipes_run(
    const std::string&              command,
    const std::filesystem::path&    cwd,
    const std::vector<std::string>& env_allowlist,
    int                             timeout_sec,
    std::size_t                     max_output_bytes,
    CancelToken&                    cancel_token);

} // namespace batbox::tools::bash
