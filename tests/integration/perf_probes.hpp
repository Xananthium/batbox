// tests/integration/perf_probes.hpp
// =============================================================================
// CPU + RSS measurement helpers for CPP T.7 performance budget tests.
//
// macOS path: proc_pidinfo(PROC_PIDTASKINFO) reads pti_total_user +
// pti_total_system (nanoseconds) and pti_resident_size (bytes) for an
// arbitrary pid without requiring elevated privileges.
//
// Linux path: reads /proc/<pid>/stat for CPU jiffies and /proc/<pid>/status
// for VmRSS.
//
// Windows: no-op path returns zeroes so the test file compiles but the test cases skip.
//
// probe() is a free function returning a ProcStats snapshot.
// measure_cpu_pct() wraps probe() with a sleep window.
// =============================================================================

#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>        // std::this_thread::sleep_for

#if defined(__APPLE__)
#  include <libproc.h>
#  include <sys/proc_info.h>
#elif defined(__linux__)
#  include <cstdlib>
#  include <fstream>
#  include <sstream>
#  include <unistd.h>  // sysconf
#endif

#include <sys/types.h>  // pid_t

namespace batbox::test {

// ---------------------------------------------------------------------------
// ProcStats — snapshot of one process's CPU and RSS.
// ---------------------------------------------------------------------------

/// CPU and memory snapshot for a running process.
struct ProcStats {
    double   cpu_user_sec{0.0};    ///< Cumulative user CPU seconds.
    double   cpu_sys_sec{0.0};     ///< Cumulative system CPU seconds.
    uint64_t rss_bytes{0};         ///< Resident set size (bytes).

    /// Total CPU seconds (user + system).
    [[nodiscard]] double cpu_total_sec() const noexcept {
        return cpu_user_sec + cpu_sys_sec;
    }
};

// ---------------------------------------------------------------------------
// probe(pid) — read a single CPU+RSS snapshot for the given PID.
// ---------------------------------------------------------------------------

/// Read a single CPU+RSS snapshot for the given PID.
///
/// Returns zeroes for all fields if the process is not accessible or has
/// already exited.
///
/// @param pid  The process ID to probe.
/// @returns    ProcStats with the current cumulative CPU and RSS.
[[nodiscard]] inline ProcStats probe(pid_t pid) noexcept {
    ProcStats s;

#if defined(__APPLE__)
    // -------------------------------------------------------------------------
    // macOS: proc_pidinfo with PROC_PIDTASKINFO.
    //
    // struct proc_taskinfo {
    //     uint64_t pti_virtual_size;      // bytes
    //     uint64_t pti_resident_size;     // bytes
    //     uint64_t pti_total_user;        // nanoseconds
    //     uint64_t pti_total_system;      // nanoseconds
    //     ...
    // }
    //
    // No elevated privileges required (works for processes owned by same user).
    // -------------------------------------------------------------------------
    struct proc_taskinfo ti{};
    const int rc = ::proc_pidinfo(
        static_cast<int>(pid),
        PROC_PIDTASKINFO,
        /*arg=*/0,
        &ti,
        static_cast<int>(sizeof(ti)));

    if (rc == static_cast<int>(sizeof(ti))) {
        // pti_total_user / pti_total_system are in nanoseconds.
        s.cpu_user_sec = static_cast<double>(ti.pti_total_user)   * 1.0e-9;
        s.cpu_sys_sec  = static_cast<double>(ti.pti_total_system) * 1.0e-9;
        s.rss_bytes    = ti.pti_resident_size;
    }

#elif defined(__linux__)
    // -------------------------------------------------------------------------
    // Linux: /proc/<pid>/stat for CPU jiffies; /proc/<pid>/status for VmRSS.
    // -------------------------------------------------------------------------
    const long hz = ::sysconf(_SC_CLK_TCK);

    // CPU from /proc/<pid>/stat
    {
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%d/stat", static_cast<int>(pid));
        std::ifstream f(path);
        if (f.is_open()) {
            std::string line;
            if (std::getline(f, line)) {
                // Find the end of the comm field (last ')' in the string).
                const auto rp = line.rfind(')');
                if (rp != std::string::npos) {
                    std::istringstream ss(line.substr(rp + 2));
                    // After comm: state ppid pgrp session tty tpgid
                    //             flags minflt cminflt majflt cmajflt
                    //             utime(11) stime(12)
                    unsigned long val{};
                    unsigned long utime{0}, stime{0};
                    for (int i = 0; i < 13; ++i) {
                        ss >> val;
                        if (i == 11) utime = val;
                        if (i == 12) stime = val;
                    }
                    if (hz > 0) {
                        s.cpu_user_sec = static_cast<double>(utime)
                                       / static_cast<double>(hz);
                        s.cpu_sys_sec  = static_cast<double>(stime)
                                       / static_cast<double>(hz);
                    }
                }
            }
        }
    }

    // RSS from /proc/<pid>/status
    {
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%d/status", static_cast<int>(pid));
        std::ifstream f(path);
        std::string line;
        while (std::getline(f, line)) {
            if (line.rfind("VmRSS:", 0) == 0) {
                unsigned long kb{};
                std::sscanf(line.c_str(), "VmRSS: %lu kB", &kb);
                s.rss_bytes = static_cast<uint64_t>(kb) * 1024ULL;
                break;
            }
        }
    }

#else
    // Unsupported platform: return zeroes; tests guard with platform_supports_probes().
    (void)pid;
#endif

    return s;
}

// ---------------------------------------------------------------------------
// measure_cpu_pct(pid, window)
// ---------------------------------------------------------------------------

/// Compute the average CPU percentage used by `pid` over `window`.
///
/// Samples probe() at the start and end of the sleep window, divides the
/// CPU delta by the wall-clock delta, and returns a percentage (0 – N×100
/// where N is core count; the pmdraft budget is expressed as % of one core).
///
/// @param pid     Process to measure.
/// @param window  Observation duration.
/// @returns       CPU%, averaged over the window.  0.0 on error or early exit.
template <class Rep, class Period>
[[nodiscard]] inline double measure_cpu_pct(
    pid_t pid,
    std::chrono::duration<Rep, Period> window) noexcept
{
    using Clock = std::chrono::steady_clock;

    const ProcStats before = probe(pid);
    const auto      t0     = Clock::now();

    // Sleep in 50 ms slices so the thread stays responsive to signals.
    auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(window);
    const auto slice_ns = std::chrono::nanoseconds{50'000'000};  // 50 ms
    while (remaining > std::chrono::nanoseconds{0}) {
        const auto s = std::min(remaining, slice_ns);
        std::this_thread::sleep_for(s);
        remaining -= s;
    }

    const auto      t1    = Clock::now();
    const ProcStats after = probe(pid);

    const double wall_sec = std::chrono::duration<double>(t1 - t0).count();
    if (wall_sec < 1.0e-9) return 0.0;

    const double cpu_delta = after.cpu_total_sec() - before.cpu_total_sec();
    if (cpu_delta < 0.0) return 0.0;  // process exited / counter reset

    return (cpu_delta / wall_sec) * 100.0;
}

} // namespace batbox::test
