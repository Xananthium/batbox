#pragma once

// ---------------------------------------------------------------------------
// batbox/sidecar/SidecarState.hpp
//
// Atomic state machine for the Python Scrapling sidecar subprocess.
//
// States (matches F8 in pmdraft.md):
//   Disabled         - sidecar disabled by config / never requested
//   Cold             - not started; will spawn on next WebFetch/WebSearch
//   Starting         - posix_spawn called; polling /healthz
//   Running          - /healthz returned 200; ready to serve requests
//   CrashedRestarting - child process exited unexpectedly; respawn in progress
//
// Thread-safety:
//   SidecarStateMachine wraps std::atomic<SidecarState> with lock-free CAS
//   semantics. try_transition() is safe to call from multiple threads
//   concurrently; only one caller wins the race for any given transition.
//
// Usage:
//   SidecarStateMachine sm;
//   if (sm.try_transition(SidecarState::Cold, SidecarState::Starting)) {
//       // won the race — this thread drives the startup sequence
//   }
//   // On /doctor:
//   fmt::print("sidecar: {}\n", batbox::sidecar::to_string(sm.current()));
// ---------------------------------------------------------------------------

#include <atomic>
#include <cstdint>
#include <functional>
#include <string_view>

namespace batbox::sidecar {

// ---------------------------------------------------------------------------
// SidecarState — lifecycle states of the scrapling sidecar
// ---------------------------------------------------------------------------
enum class SidecarState : uint8_t {
    Disabled,          ///< Sidecar disabled; BATBOX_SIDECAR_AUTOSTART=false or shutdown
    Cold,              ///< Not started; spawns on first WebFetch/WebSearch call
    Starting,          ///< posix_spawn called; health-check poll in progress
    Running,           ///< /healthz OK; ready to serve requests
    CrashedRestarting, ///< Child exited unexpectedly; respawn pending
};

// ---------------------------------------------------------------------------
// to_string — human-readable label for /doctor display and status line
// The returned string_view is a pointer into static storage (zero allocation).
// ---------------------------------------------------------------------------
[[nodiscard]] std::string_view to_string(SidecarState state) noexcept;

// ---------------------------------------------------------------------------
// is_legal_transition — declarative table of allowed state transitions
//
// Allowed edges (directed):
//   Disabled         → Cold             (re-enable)
//   Cold             → Starting         (first web tool call triggers spawn)
//   Cold             → Disabled         (shutdown path)
//   Starting         → Running          (health-check passed)
//   Starting         → CrashedRestarting (spawn failed / health-check timeout)
//   Starting         → Disabled         (shutdown during startup)
//   Running          → CrashedRestarting (child exits unexpectedly)
//   Running          → Disabled         (graceful shutdown)
//   CrashedRestarting → Starting        (restart attempt)
//   CrashedRestarting → Disabled        (max retries hit; give up)
//
// All other pairs return false.
// ---------------------------------------------------------------------------
[[nodiscard]] bool is_legal_transition(SidecarState from, SidecarState to) noexcept;

// ---------------------------------------------------------------------------
// SidecarStateMachine — thread-safe atomic wrapper
//
// Holds the current state as std::atomic<SidecarState> and provides
// CAS-style try_transition() so concurrent callers race safely.
// An optional on_transition callback fires after every successful transition.
// ---------------------------------------------------------------------------
class SidecarStateMachine {
public:
    // Construct with an initial state (default: Cold).
    explicit SidecarStateMachine(SidecarState initial = SidecarState::Cold) noexcept;

    // Non-copyable, non-movable — the atomic is pinned.
    SidecarStateMachine(const SidecarStateMachine&) = delete;
    SidecarStateMachine& operator=(const SidecarStateMachine&) = delete;
    SidecarStateMachine(SidecarStateMachine&&) = delete;
    SidecarStateMachine& operator=(SidecarStateMachine&&) = delete;

    // -----------------------------------------------------------------------
    // current() — load the current state with sequential consistency.
    // -----------------------------------------------------------------------
    [[nodiscard]] SidecarState current() const noexcept;

    // -----------------------------------------------------------------------
    // try_transition(from, to)
    //
    // Atomically:
    //   1. Validates is_legal_transition(from, to) — returns false if illegal.
    //   2. compare_exchange_strong(from, to) — returns false if the current
    //      state no longer equals `from` (another thread raced ahead).
    //   3. If the CAS succeeds, fires the on_transition callback (if set)
    //      and returns true.
    //
    // The caller that receives true "owns" the transition and is responsible
    // for driving whatever side-effects the new state requires (e.g., spawning
    // the process, initiating health-check, etc.).
    // -----------------------------------------------------------------------
    [[nodiscard]] bool try_transition(SidecarState from, SidecarState to) noexcept;

    // -----------------------------------------------------------------------
    // set_on_transition(fn)
    //
    // Registers a callback invoked after every successful try_transition().
    // Signature: void(SidecarState old_state, SidecarState new_state)
    //
    // The callback is invoked from the thread that wins the CAS, after the
    // atomic store has committed. It must not call try_transition() re-entrantly
    // (deadlock-free design: the mutex is not held during the callback).
    //
    // Pass nullptr to clear a previously registered callback.
    // This method is NOT thread-safe relative to concurrent try_transition()
    // calls — set the callback before making the machine visible to other
    // threads (i.e., during construction/setup).
    // -----------------------------------------------------------------------
    void set_on_transition(
        std::function<void(SidecarState /*old*/, SidecarState /*new_state*/)> fn) noexcept;

private:
    std::atomic<SidecarState> state_;
    std::function<void(SidecarState, SidecarState)> on_transition_;
};

} // namespace batbox::sidecar
