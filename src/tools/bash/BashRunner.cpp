// src/tools/bash/BashRunner.cpp
//
// batbox::tools::bash::BashRunner — implementation.
//
// Blueprint contract: CPP 5.8 — BashRunner (file symbol, src/tools/bash/BashRunner.cpp)
// Blueprint contract: CPP 5.9 — PipesBackend (file symbol, src/tools/bash/PipesBackend.cpp)
//
// Backend selection (CPP 5.9):
//   Env var BATBOX_BASH_BACKEND controls which backend runs:
//     "pty"   — always use PtyBackend (forkpty)
//     "pipes" — always use PipesBackend (fork + pipe pair)
//     "auto"  — try PtyBackend first; if forkpty() fails with EPERM or
//               ENOTTY (no pty in container / sandbox), retry with PipesBackend
//     (unset) — same as "auto"
//
// BashRunner::run() is the single public entry point. It:
//   1. Returns BashResult::plan_mode_error() immediately in plan mode.
//   2. Reads BATBOX_BASH_BACKEND and selects/falls-back as above.
//   3. Converts PtyResult / PipesResult → BashResult.

#include <batbox/tools/bash/BashRunner.hpp>
#include "PtyBackendInternal.hpp"
#include "PipesBackendInternal.hpp"

#include <cerrno>
#include <cstdlib>   // std::getenv
#include <cstring>   // std::strcmp
#include <filesystem>
#include <string>
#include <vector>

namespace batbox::tools::bash {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

/// Read BATBOX_BASH_BACKEND from the environment.
/// Returns one of: "pty", "pipes", "auto".
std::string read_backend_env()
{
    const char* val = std::getenv("BATBOX_BASH_BACKEND");
    if (val == nullptr || val[0] == '\0') {
        return "auto";
    }
    if (std::strcmp(val, "pty") == 0)   return "pty";
    if (std::strcmp(val, "pipes") == 0) return "pipes";
    return "auto"; // treat any unknown value as auto
}

/// Convert a PtyResult into a BashResult.
BashResult pty_to_bash(PtyResult pty)
{
    BashResult result;
    result.body      = std::move(pty.output);
    result.exit_code = pty.exit_code;
    result.duration  = pty.duration;
    result.is_error  = (pty.exit_code != 0);
    return result;
}

/// Convert a PipesResult into a BashResult.
BashResult pipes_to_bash(PipesResult pr)
{
    BashResult result;
    result.body      = std::move(pr.output);
    result.exit_code = pr.exit_code;
    result.duration  = pr.duration;
    result.is_error  = (pr.exit_code != 0);
    return result;
}

/// Returns true if a forkpty() failure error string indicates a "no-pty"
/// environment (EPERM or ENOTTY) rather than a transient failure.
bool is_no_pty_error(const std::string& pty_output)
{
    // pty_run() encodes the errno string in its error output when forkpty fails:
    //   "forkpty() failed: Operation not permitted"   (EPERM)
    //   "forkpty() failed: Inappropriate ioctl..."    (ENOTTY)
    // We check for the canonical errno descriptions from strerror().
    if (pty_output.find("forkpty() failed:") == std::string::npos) {
        return false;
    }
    // EPERM description on macOS/Linux
    if (pty_output.find("Operation not permitted") != std::string::npos) {
        return true;
    }
    // ENOTTY description
    if (pty_output.find("Inappropriate ioctl") != std::string::npos) {
        return true;
    }
    // ENXIO — no such device or address (some container runtimes)
    if (pty_output.find("No such device") != std::string::npos) {
        return true;
    }
    return false;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// BashRunner::run()
// ---------------------------------------------------------------------------

BashResult BashRunner::run(
    const std::string&              command,
    const std::filesystem::path&    cwd,
    const std::vector<std::string>& env_allowlist,
    int                             timeout_sec,
    std::size_t                     max_output_bytes,
    CancelToken&                    cancel_token,
    bool                            plan_mode) const
{
    if (plan_mode) {
        return BashResult::plan_mode_error();
    }

    const std::string backend = read_backend_env();

    // ------------------------------------------------------------------
    // "pipes" — skip pty entirely, go straight to pipe backend.
    // ------------------------------------------------------------------
    if (backend == "pipes") {
        PipesResult pr = pipes_run(
            command, cwd, env_allowlist,
            timeout_sec, max_output_bytes, cancel_token);
        return pipes_to_bash(std::move(pr));
    }

    // ------------------------------------------------------------------
    // "pty" — use forkpty only; do not fall back.
    // ------------------------------------------------------------------
    if (backend == "pty") {
        PtyResult pty = pty_run(
            command, cwd, env_allowlist,
            timeout_sec, max_output_bytes, cancel_token);
        return pty_to_bash(std::move(pty));
    }

    // ------------------------------------------------------------------
    // "auto" — try PtyBackend; fall back to PipesBackend on EPERM/ENOTTY.
    //
    // pty_run() returns exit_code == -1 and output starts with
    // "forkpty() failed: ..." when forkpty() itself fails.
    // We detect that sentinel and retry with pipes.
    // ------------------------------------------------------------------
    PtyResult pty = pty_run(
        command, cwd, env_allowlist,
        timeout_sec, max_output_bytes, cancel_token);

    if (pty.exit_code == -1 && is_no_pty_error(pty.output)) {
        // forkpty unavailable in this environment — retry with pipes.
        PipesResult pr = pipes_run(
            command, cwd, env_allowlist,
            timeout_sec, max_output_bytes, cancel_token);
        return pipes_to_bash(std::move(pr));
    }

    return pty_to_bash(std::move(pty));
}

} // namespace batbox::tools::bash
