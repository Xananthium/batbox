// src/conversation/PlanMode.cpp
//
// batbox::conversation::PlanMode — state machine implementation.
//
// The state diagram:
//
//   ┌─────────────────────────────────────────────────┐
//   │                                                 │
//   ▼                                                 │
// Inactive ──enter_plan()──► Planning ──approve()──► Approved
//             ◄──reject()──    ◄──────advance_turn()──┘
//
// transition(State) is the low-level escape hatch used by tool implementations;
// it sets the state directly and fires observers without additional validation.

#include "batbox/conversation/PlanMode.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace batbox::conversation {

// ---------------------------------------------------------------------------
// Write-side tool names that are denied in Planning/Approved states.
// Stored as a fixed-size array; linear scan is fine for 5 entries.
// ---------------------------------------------------------------------------

static constexpr std::array<std::string_view, 5> kWriteTools = {
    "Bash",
    "Edit",
    "PowerShell",
    "TodoWrite",
    "Write",
};

// ---------------------------------------------------------------------------
// Transitions
// ---------------------------------------------------------------------------

void PlanMode::enter_plan() {
    if (state_ == PlanState::Approved) {
        throw PlanModeError(
            "PlanMode::enter_plan() called while state is Approved; "
            "call advance_turn() first to return to Inactive.");
    }
    if (state_ == PlanState::Planning) {
        // Idempotent noop — already planning.
        return;
    }
    // Inactive → Planning
    transition_to(PlanState::Planning);
}

std::uint32_t PlanMode::approve(std::string plan_text) {
    if (state_ != PlanState::Planning) {
        throw PlanModeError(
            std::string("PlanMode::approve() called from state ") +
            std::string(plan_state_name(state_)) +
            "; must be in Planning state.");
    }
    plan_text_ = std::move(plan_text);
    ++plan_id_;
    transition_to(PlanState::Approved);
    return plan_id_;
}

void PlanMode::reject() {
    if (state_ != PlanState::Planning) {
        throw PlanModeError(
            std::string("PlanMode::reject() called from state ") +
            std::string(plan_state_name(state_)) +
            "; must be in Planning state.");
    }
    // Planning → Inactive.  plan_text_ / plan_id_ retained as history.
    transition_to(PlanState::Inactive);
}

void PlanMode::advance_turn() noexcept {
    if (state_ != PlanState::Approved) {
        return; // noop
    }
    // Approved → Inactive (one-shot).
    // plan_text_ and plan_id_ retained for reference until the next approve().
    // noexcept: swallow any observer exceptions.
    try {
        transition_to(PlanState::Inactive);
    } catch (...) {
        // An observer threw — eat it here; state was already updated.
    }
}

void PlanMode::transition(PlanState new_state) {
    // Low-level escape hatch for tool implementations.  No guard checks.
    transition_to(new_state);
}

// ---------------------------------------------------------------------------
// Tool gating
// ---------------------------------------------------------------------------

bool PlanMode::is_tool_allowed(std::string_view tool_name) const noexcept {
    if (state_ == PlanState::Inactive) {
        return true;
    }
    // Planning or Approved: deny write-side tools.
    for (const auto& wt : kWriteTools) {
        if (wt == tool_name) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// UI helpers
// ---------------------------------------------------------------------------

std::string_view PlanMode::banner() const noexcept {
    switch (state_) {
        case PlanState::Planning:  return "PLAN MODE";
        case PlanState::Approved:  return "PLAN MODE \xe2\x80\x94 APPROVED"; // UTF-8 em dash
        case PlanState::Inactive:  return "";
    }
    return "";
}

// ---------------------------------------------------------------------------
// Observers
// ---------------------------------------------------------------------------

PlanObserverHandle PlanMode::add_observer(Observer fn) {
    const std::uint32_t id = next_observer_id_++;
    observers_.push_back({id, std::move(fn)});
    return PlanObserverHandle(this, id);
}

void PlanMode::remove_observer(std::uint32_t id) noexcept {
    observers_.erase(
        std::remove_if(observers_.begin(), observers_.end(),
                       [id](const ObserverEntry& e) { return e.id == id; }),
        observers_.end());
}

void PlanMode::transition_to(PlanState new_state) {
    state_ = new_state;
    // Notify observers. Iterate by index to be stable if an observer modifies
    // the container indirectly.
    for (std::size_t i = 0; i < observers_.size(); ++i) {
        observers_[i].fn(new_state);
    }
}

} // namespace batbox::conversation
