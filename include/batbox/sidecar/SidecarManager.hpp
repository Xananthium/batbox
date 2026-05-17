#pragma once

// ---------------------------------------------------------------------------
// include/batbox/sidecar/SidecarManager.hpp
//
// SidecarManager — lifecycle controller for the Python Scrapling sidecar.
//
// Responsibilities:
//   - Pick a free ephemeral port (bind(0)/getsockname/close)
//   - posix_spawn python -m scrapling_server --port <N> with the correct envp
//     (VIRTUAL_ENV, PATH prefix, PYTHONUNBUFFERED=1, SCRAPLING_PORT)
//   - Set the child process-group (POSIX_SPAWN_SETPGROUP / setpgid) so the
//     whole subtree can be killed with killpg()
//   - Read child stderr on a background thread → spdlog [sidecar]
//   - Poll /healthz at 100 ms intervals until HTTP 200 or startup_timeout_sec
//   - Transition Cold→Starting→Running (or →CrashedRestarting on timeout)
//   - Honour restart cap (3 per session); fourth failure → Disabled
//   - Graceful shutdown: POST /shutdown → SIGTERM → SIGKILL → waitpid
//   - Template request() helper: ensure_started → ScraplingClient call
//
// Thread-safety:
//   ensure_started() uses the SidecarStateMachine CAS so concurrent callers
//   are safe; only the thread that wins Cold→Starting drives the spawn.
//   Concurrent request() calls are safe once Running.
//   shutdown() must be called from one thread only (e.g. main teardown).
//
// Namespace: batbox::sidecar
// ---------------------------------------------------------------------------

#include <batbox/config/Config.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/core/Logging.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/sidecar/ScraplingClient.hpp>
#include <batbox/sidecar/SidecarState.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace batbox::sidecar {

// ---------------------------------------------------------------------------
// SidecarManager
// ---------------------------------------------------------------------------
class SidecarManager {
public:
    // -----------------------------------------------------------------------
    // Constructor
    //
    // cfg — SidecarConfig read from Config::sidecar.  SidecarManager does NOT
    //       hold a reference to the Config; it copies the fields it needs at
    //       construction time.  Hot-reload of sidecar fields requires a
    //       shutdown + re-construction (print notice to user).
    // -----------------------------------------------------------------------
    explicit SidecarManager(const batbox::config::SidecarConfig& cfg);

    // Non-copyable, non-movable — owns threads and raw pid.
    SidecarManager(const SidecarManager&) = delete;
    SidecarManager& operator=(const SidecarManager&) = delete;
    SidecarManager(SidecarManager&&) = delete;
    SidecarManager& operator=(SidecarManager&&) = delete;

    ~SidecarManager();

    // -----------------------------------------------------------------------
    // ensure_started(ct)
    //
    // Ensures the sidecar is Running before returning.
    //
    // State transitions driven by this call:
    //   Cold             → Starting (this thread wins the CAS and spawns)
    //   Starting         → Running  (healthz poll succeeds)
    //   Starting         → CrashedRestarting (timeout)
    //   CrashedRestarting→ Starting (restart attempt, if cap not hit)
    //   CrashedRestarting→ Disabled (restart cap exhausted)
    //
    // If another thread is already driving Starting, this call blocks and
    // polls state until Running or a terminal error.
    //
    // Returns:
    //   Ok(void)         — sidecar is Running
    //   Err(std::string) — sidecar could not be started (disabled, cap hit,
    //                      spawn error, timeout); message suitable for display
    //
    // Cancellable: ct fires → returns Err("cancelled") after cleaning up.
    // -----------------------------------------------------------------------
    [[nodiscard]] Result<void> ensure_started(CancelToken ct);

    // -----------------------------------------------------------------------
    // shutdown()
    //
    // Graceful shutdown sequence:
    //   1. POST /shutdown to the sidecar (1 s timeout) — best-effort
    //   2. If child still alive: killpg(pgid, SIGTERM), wait up to 2 s
    //   3. If still alive: killpg(pgid, SIGKILL)
    //   4. waitpid() to reap zombie
    //   5. Join stderr reader thread
    //   6. Transition state → Disabled
    //
    // Safe to call even if sidecar never started (no-op).
    // Must be called from one thread only (main teardown).
    // -----------------------------------------------------------------------
    void shutdown();

    // -----------------------------------------------------------------------
    // request<Req, Resp>(endpoint, req, ct)
    //
    // Template helper that:
    //   1. Calls ensure_started(ct) if state != Running
    //   2. Constructs a ScraplingClient on the active port
    //   3. Calls client.post_json(endpoint, req.to_json(), ct)
    //   4. Returns parsed Resp or Err
    //
    // Usage:
    //   auto res = mgr.request<proto::FetchRequest, proto::FetchResponse>(
    //       "/fetch", req, std::move(ct));
    // -----------------------------------------------------------------------
    template <typename Req, typename Resp>
    [[nodiscard]] Result<Resp> request(std::string_view endpoint,
                                       const Req&        req,
                                       CancelToken       ct);

    // -----------------------------------------------------------------------
    // current_state() — observe current lifecycle state.
    // -----------------------------------------------------------------------
    [[nodiscard]] SidecarState current_state() const noexcept {
        return state_.current();
    }

    // -----------------------------------------------------------------------
    // port() — the port the sidecar is (or was last) bound to.
    // Returns 0 if never started.
    // -----------------------------------------------------------------------
    [[nodiscard]] uint16_t port() const noexcept {
        return port_.load(std::memory_order_relaxed);
    }

    // -----------------------------------------------------------------------
    // restart_count() — number of restart attempts this session.
    // -----------------------------------------------------------------------
    [[nodiscard]] int restart_count() const noexcept {
        return restart_count_.load(std::memory_order_relaxed);
    }

    // Maximum number of restart attempts per session before giving up.
    static constexpr int kMaxRestarts = 3;

    // -----------------------------------------------------------------------
    // prewarm_async(ct)
    //
    // Launches a background std::async task that calls ensure_started(ct) so
    // the sidecar is already Running by the time the first WebFetch/WebSearch
    // arrives.  Returns immediately — the caller does NOT block.
    //
    // The status_cb, if provided, is called with a human-readable label:
    //   "prewarming" — spawned, health-check poll in progress
    //   "ready"      — sidecar reached Running state
    //   "failed: …"  — prewarm failed (first WebFetch will cold-start instead)
    //
    // Thread-safety: safe to call once before any ensure_started() calls.
    // Calling it more than once is harmless (second call is a no-op if
    // the prewarm future is already in-flight or complete).
    //
    // Cancellable: if ct fires before the future completes, the background
    // task exits and the future stores an Err("cancelled") result.
    // -----------------------------------------------------------------------
    void prewarm_async(CancelToken ct,
                       std::function<void(std::string_view)> status_cb = nullptr);

    // -----------------------------------------------------------------------
    // wait_prewarm()
    //
    // If a prewarm future is in-flight, blocks until it completes and returns
    // the result.  If no prewarm was launched, returns Ok immediately.
    //
    // Called by:
    //   - ensure_started() — to consume any in-flight prewarm before attempting
    //     a cold start (so we don't race against the background thread)
    //   - SidecarManager destructor — to join the background thread safely
    //     before shutdown() tears down the process
    // -----------------------------------------------------------------------
    [[nodiscard]] Result<void> wait_prewarm();

    // -----------------------------------------------------------------------
    // abort_startup() — CPP 7.6: second Ctrl+C kills the sidecar process group
    //
    // Sends SIGTERM to the child process group and transitions state to
    // CrashedRestarting.  Called by the App-level double-tap SIGINT handler
    // (main thread, not the signal handler itself — see App.cpp).
    //
    // Safe to call when the sidecar is Starting or Running.
    // No-op if no child process is alive.
    // -----------------------------------------------------------------------
    void abort_startup() noexcept;

private:
    // ---- Configuration (copied at construction) ----------------------------
    std::string           python_bin_;       // e.g. "python3" or full path
    std::string           venv_dir_;         // expanded ~/.batbox/sidecar/.venv
    int                   startup_timeout_sec_;

    // ---- State machine -------------------------------------------------------
    SidecarStateMachine   state_;            // starts Cold

    // ---- Child process -------------------------------------------------------
    std::atomic<int>      child_pid_{-1};    // pid_t is int on POSIX
    std::atomic<int>      child_pgid_{-1};   // process group id
    std::atomic<uint16_t> port_{0};          // ephemeral port picked at spawn time

    // ---- Restart accounting --------------------------------------------------
    std::atomic<int>      restart_count_{0};

    // ---- Stderr reader -------------------------------------------------------
    int                   stderr_read_fd_{-1};  // read end of child stderr pipe
    std::thread           stderr_reader_thread_;

    // ---- Prewarm future ------------------------------------------------------
    // Set by prewarm_async(); waited on by wait_prewarm() and the destructor.
    // Valid (non-default) only while a prewarm is in-flight or has completed.
    std::future<Result<void>>  prewarm_future_;
    // Optional status callback stored during prewarm (used by the async lambda).
    std::function<void(std::string_view)> prewarm_status_cb_;

    // ---- Helpers -------------------------------------------------------------

    // Pick a free ephemeral TCP port on 127.0.0.1.
    // Returns 0 on failure.
    [[nodiscard]] static uint16_t pick_free_port() noexcept;

    // Build the null-terminated envp array for posix_spawn.
    // Overlays VIRTUAL_ENV, PATH prefix, PYTHONUNBUFFERED=1, SCRAPLING_PORT
    // on top of the parent's environ.
    // Returns heap-allocated strings; caller owns them via the vector<string>.
    [[nodiscard]] static std::vector<std::string>
    build_envp(const std::string& venv_dir, uint16_t port);

    // Core spawn: picks port, builds argv+envp, calls posix_spawn.
    // Returns Err on any failure.
    [[nodiscard]] Result<void> do_spawn();

    // Health-check poll loop.  Polls /healthz every 100 ms until:
    //   - HTTP 200 → transitions Starting→Running, returns Ok
    //   - startup_timeout_sec_ elapsed → transitions Starting→CrashedRestarting,
    //     returns Err("startup timeout")
    //   - ct cancelled → transitions Starting→Disabled, returns Err("cancelled")
    [[nodiscard]] Result<void> wait_for_healthy(CancelToken ct);

    // Reap child if it has exited (non-blocking waitpid).
    // Returns true if the child is gone.
    bool try_reap_child() noexcept;

    // Start the stderr reader background thread.
    void start_stderr_reader(int fd);

    // Send SIGTERM then SIGKILL with a grace period; waitpid.
    void kill_and_reap(std::chrono::milliseconds sigterm_grace) noexcept;
};

// ---------------------------------------------------------------------------
// request<Req, Resp> — template implementation (must be in the header)
//
// Uses ScraplingClient for the actual HTTP POST so that transport logic
// (cancellation bridge, error classification) lives in one place.
// ScraplingClient::post_json is private; instead we route through the
// specialised public methods for the known proto types.
//
// For unknown Req/Resp pairs (e.g. test doubles), a direct cpr POST is
// performed via a free-function helper defined in SidecarManager.cpp and
// declared here.
// ---------------------------------------------------------------------------

// Forward-declared helper: POST body_json to http://127.0.0.1:<port><endpoint>
// with a 30 s default timeout.  Returns raw response body string or Err.
// Defined in SidecarManager.cpp (not a member so it can be called from the
// template without requiring access to ScraplingClient internals).
Result<std::string> sidecar_post_json_raw(
    uint16_t            port,
    std::string_view    endpoint,
    const batbox::Json& body,
    CancelToken         ct);

template <typename Req, typename Resp>
Result<Resp> SidecarManager::request(std::string_view endpoint,
                                     const Req&        req,
                                     CancelToken       ct)
{
    // Ensure the sidecar is running.
    if (state_.current() != SidecarState::Running) {
        auto [child_src, child_tok] = ct.child();
        (void)child_src;
        auto start_res = ensure_started(std::move(child_tok));
        if (!start_res.has_value()) {
            return batbox::Err(start_res.error());
        }
    }

    const uint16_t p = port_.load(std::memory_order_acquire);
    if (p == 0) {
        return batbox::Err(std::string("sidecar port not set after ensure_started"));
    }

    auto raw = sidecar_post_json_raw(p, endpoint, req.to_json(), std::move(ct));
    if (!raw.has_value()) {
        if (try_reap_child()) {
            (void)state_.try_transition(SidecarState::Running, SidecarState::CrashedRestarting);
        }
        return batbox::Err(raw.error());
    }

    auto parsed = batbox::parse(raw.value());
    if (!parsed.has_value()) {
        return batbox::Err("json parse error: " + parsed.error());
    }

    try {
        return Resp::from_json(parsed.value());
    } catch (const std::exception& e) {
        return batbox::Err(std::string("json parse error: ") + e.what());
    }
}

} // namespace batbox::sidecar
