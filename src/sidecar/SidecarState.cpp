// ---------------------------------------------------------------------------
// src/sidecar/SidecarState.cpp
//
// Implementation of SidecarState atomic enum, to_string(), is_legal_transition(),
// and SidecarStateMachine.
// ---------------------------------------------------------------------------

#include "batbox/sidecar/SidecarState.hpp"

#include <array>
#include <cstddef>

namespace batbox::sidecar {

// ---------------------------------------------------------------------------
// to_string — static lookup; zero allocation
// ---------------------------------------------------------------------------
std::string_view to_string(SidecarState state) noexcept {
    // Labels match pmdraft.md F8 /doctor display strings:
    //   cold / prewarming(starting) / running / crashed-restarting / disabled
    switch (state) {
        case SidecarState::Disabled:          return "disabled";
        case SidecarState::Cold:              return "cold";
        case SidecarState::Starting:          return "starting";
        case SidecarState::Running:           return "running";
        case SidecarState::CrashedRestarting: return "crashed-restarting";
    }
    // Unreachable for a well-formed enum value, but satisfies the compiler
    // on paths where it cannot prove exhaustiveness.
    return "unknown";
}

// ---------------------------------------------------------------------------
// is_legal_transition — declarative adjacency table
//
// Encoded as a flat array of (from, to) pairs rather than a nested switch so
// the full transition graph is visible in one place and easy to audit.
// ---------------------------------------------------------------------------
bool is_legal_transition(SidecarState from, SidecarState to) noexcept {
    // Each pair is {from, to}.
    struct Edge {
        SidecarState from;
        SidecarState to;
    };

    static constexpr std::array<Edge, 10> kAllowedEdges{{
        // Re-enable path
        {SidecarState::Disabled,          SidecarState::Cold},

        // Normal startup path
        {SidecarState::Cold,              SidecarState::Starting},

        // Explicit shutdown from cold (user disables sidecar without ever using it)
        {SidecarState::Cold,              SidecarState::Disabled},

        // Health-check succeeded
        {SidecarState::Starting,          SidecarState::Running},

        // Spawn failed or health-check timed out
        {SidecarState::Starting,          SidecarState::CrashedRestarting},

        // Shutdown requested while still starting (Ctrl+C during warm-up)
        {SidecarState::Starting,          SidecarState::Disabled},

        // Child process exited while running
        {SidecarState::Running,           SidecarState::CrashedRestarting},

        // Graceful batbox shutdown from running state
        {SidecarState::Running,           SidecarState::Disabled},

        // Restart attempt after a crash
        {SidecarState::CrashedRestarting, SidecarState::Starting},

        // Max retries exhausted; give up
        {SidecarState::CrashedRestarting, SidecarState::Disabled},
    }};

    for (const auto& edge : kAllowedEdges) {
        if (edge.from == from && edge.to == to) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// SidecarStateMachine implementation
// ---------------------------------------------------------------------------

SidecarStateMachine::SidecarStateMachine(SidecarState initial) noexcept
    : state_(initial)
    , on_transition_(nullptr)
{}

SidecarState SidecarStateMachine::current() const noexcept {
    return state_.load(std::memory_order_seq_cst);
}

bool SidecarStateMachine::try_transition(SidecarState from, SidecarState to) noexcept {
    // Guard: reject illegal edges without touching the atomic.
    if (!is_legal_transition(from, to)) {
        return false;
    }

    // CAS: atomically transition from → to, but only if the current value is
    // still `from`.  Another thread may have already advanced the state.
    SidecarState expected = from;
    const bool swapped = state_.compare_exchange_strong(
        expected,
        to,
        std::memory_order_seq_cst,   // success ordering
        std::memory_order_seq_cst    // failure ordering
    );

    if (swapped && on_transition_) {
        // Fire the callback outside of any lock; the atomic has already
        // committed so subsequent current() calls see `to`.
        on_transition_(from, to);
    }

    return swapped;
}

void SidecarStateMachine::set_on_transition(
    std::function<void(SidecarState, SidecarState)> fn) noexcept
{
    on_transition_ = std::move(fn);
}

} // namespace batbox::sidecar
