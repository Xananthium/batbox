// include/batbox/conversation/PlanMode.hpp
//
// batbox::conversation::PlanMode — Plan-mode state machine.
//
// States  : Inactive  → the default; all tools permitted.
//           Planning  → entered via enter_plan() or transition(Planning);
//                       write-side tools denied.
//           Approved  → entered via approve(plan_text); one-shot flag that
//                       signals the next turn may proceed with write tools.
//                       Transitions back to Inactive automatically when
//                       advance_turn() is called at the end of an approved turn.
//
// Transitions
//   enter_plan()                Inactive  → Planning  (noop if already Planning)
//   approve(plan_text)          Planning  → Approved  (error if Inactive/Approved)
//   reject()                    Planning  → Inactive  (error if not Planning)
//   advance_turn()              Approved  → Inactive  (noop otherwise)
//   transition(new_state)       Low-level: force a specific state; validates
//                               only that the target is a valid PlanState.
//                               Prefer the typed methods above — transition()
//                               is provided to satisfy the blueprint contract
//                               for programmatic callers (EnterPlanMode tool).
//
// Tool gating
//   is_tool_allowed(name)       Returns false for write-side tools when state
//                               is Planning or Approved.
//   is_write_denied()           Returns true when write-side tools are blocked
//                               (i.e., state != Inactive). Convenience form of
//                               !is_tool_allowed("Write").
//
// Observers
//   add_observer(fn)            fn(new_state) called synchronously on each
//                               transition. Returns a handle; destroy to
//                               unsubscribe.
//
// Thread-safety: NOT thread-safe. Callers must synchronise externally if they
// share a PlanMode instance across threads.

#pragma once

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace batbox::conversation {

// ---------------------------------------------------------------------------
// State enum
// ---------------------------------------------------------------------------

enum class PlanState : std::uint8_t {
    Inactive  = 0,
    Planning  = 1,
    Approved  = 2,
};

[[nodiscard]] inline std::string_view plan_state_name(PlanState s) noexcept {
    switch (s) {
        case PlanState::Inactive:  return "Inactive";
        case PlanState::Planning:  return "Planning";
        case PlanState::Approved:  return "Approved";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

struct PlanModeError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ---------------------------------------------------------------------------
// Observer handle
// ---------------------------------------------------------------------------

/// Returned by add_observer(). The observer is deregistered when the handle
/// is destroyed.  Move-only; non-copyable.
class PlanObserverHandle {
public:
    PlanObserverHandle() = default;
    ~PlanObserverHandle() { detach(); }

    // Non-copyable, movable.
    PlanObserverHandle(const PlanObserverHandle&) = delete;
    PlanObserverHandle& operator=(const PlanObserverHandle&) = delete;
    PlanObserverHandle(PlanObserverHandle&& o) noexcept
        : owner_(o.owner_), id_(o.id_) {
        o.owner_ = nullptr;
        o.id_    = 0;
    }
    PlanObserverHandle& operator=(PlanObserverHandle&& o) noexcept {
        if (this != &o) {
            detach();
            owner_ = o.owner_;
            id_    = o.id_;
            o.owner_ = nullptr;
            o.id_    = 0;
        }
        return *this;
    }

private:
    friend class PlanMode;

    // Set by PlanMode::add_observer.
    void*         owner_ = nullptr;   // non-owning pointer back to PlanMode
    std::uint32_t id_    = 0;

    void detach() noexcept;

    explicit PlanObserverHandle(void* owner, std::uint32_t id) noexcept
        : owner_(owner), id_(id) {}
};

// ---------------------------------------------------------------------------
// PlanMode
// ---------------------------------------------------------------------------

class PlanMode {
public:
    using Observer = std::function<void(PlanState)>;

    PlanMode() = default;
    ~PlanMode() = default;

    // Non-copyable, movable.
    PlanMode(const PlanMode&) = delete;
    PlanMode& operator=(const PlanMode&) = delete;
    PlanMode(PlanMode&&) noexcept = default;
    PlanMode& operator=(PlanMode&&) noexcept = default;

    // ---- State queries -------------------------------------------------------

    /// Current state.
    [[nodiscard]] PlanState state() const noexcept { return state_; }

    /// Convenience: true when in Planning state.
    [[nodiscard]] bool is_planning() const noexcept {
        return state_ == PlanState::Planning;
    }

    /// Convenience: true when in Approved state.
    [[nodiscard]] bool is_approved() const noexcept {
        return state_ == PlanState::Approved;
    }

    /// The plan text set by the last approve() call. Empty if never approved.
    [[nodiscard]] const std::string& plan_text() const noexcept {
        return plan_text_;
    }

    /// Incrementing plan id (1-based). Increases on each approve() call.
    /// 0 = no plan approved yet in this session.
    [[nodiscard]] std::uint32_t plan_id() const noexcept { return plan_id_; }

    // ---- Transitions ---------------------------------------------------------

    /// Inactive → Planning.  Noop if already Planning.
    /// Throws PlanModeError if state == Approved.
    void enter_plan();

    /// Planning → Approved.  Stores plan_text as the active plan.
    /// Returns the plan id (1-based, incremented each call).
    /// Throws PlanModeError if state != Planning.
    [[nodiscard]] std::uint32_t approve(std::string plan_text);

    /// Planning → Inactive.
    /// Throws PlanModeError if state != Planning.
    void reject();

    /// Approved → Inactive  (one-shot: call at the end of each approved turn).
    /// Noop if state != Approved.
    void advance_turn() noexcept;

    /// Low-level programmatic transition used by EnterPlanMode / ExitPlanMode
    /// tool implementations (blueprint contract).
    /// Directly sets the state and notifies observers.  Does not enforce
    /// transition validity — prefer enter_plan() / approve() / reject() for
    /// call-sites that can provide semantic context.
    void transition(PlanState new_state);

    // ---- Tool gating ---------------------------------------------------------

    /// Returns false for write-side tools when in Planning or Approved state.
    /// Write-side tools: Write, Edit, Bash, PowerShell, TodoWrite.
    /// All other tools (including unknown names) return true.
    [[nodiscard]] bool is_tool_allowed(std::string_view tool_name) const noexcept;

    /// Returns true when write-side tools are blocked (state != Inactive).
    /// Convenience alias satisfying the blueprint contract:
    ///   `is_write_denied()` ≡ `!is_tool_allowed("Write")`
    [[nodiscard]] bool is_write_denied() const noexcept {
        return state_ != PlanState::Inactive;
    }

    // ---- UI helpers ----------------------------------------------------------

    /// Returns a banner string when a banner should be shown, empty otherwise.
    ///   Planning  → "PLAN MODE"
    ///   Approved  → "PLAN MODE — APPROVED"  (UTF-8 em dash)
    ///   Inactive  → ""
    [[nodiscard]] std::string_view banner() const noexcept;

    // ---- Observers -----------------------------------------------------------

    /// Register a callback invoked (synchronously) on every state transition.
    /// Returns a handle; destroy the handle to deregister.
    [[nodiscard]] PlanObserverHandle add_observer(Observer fn);

private:
    friend class PlanObserverHandle;

    PlanState   state_    = PlanState::Inactive;
    std::string plan_text_;
    std::uint32_t plan_id_ = 0;

    struct ObserverEntry {
        std::uint32_t id;
        Observer      fn;
    };
    std::vector<ObserverEntry> observers_;
    std::uint32_t              next_observer_id_ = 1;

    void transition_to(PlanState new_state);
    void remove_observer(std::uint32_t id) noexcept;
};

// ---------------------------------------------------------------------------
// PlanObserverHandle::detach  (inline — needs PlanMode definition above)
// ---------------------------------------------------------------------------
inline void PlanObserverHandle::detach() noexcept {
    if (owner_) {
        static_cast<PlanMode*>(owner_)->remove_observer(id_);
        owner_ = nullptr;
        id_    = 0;
    }
}

} // namespace batbox::conversation
