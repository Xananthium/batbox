// include/batbox/tui/PlanApprovalCard.hpp
// ---------------------------------------------------------------------------
// batbox::tui::PlanApprovalCard — plan approval modal with [A]pprove / [R]eject / [E]dit.
//
// Design
// ------
// PlanApprovalCard is an FTXUI ComponentBase subclass that renders a floating
// plan-approval dialog when ExitPlanMode is called by the model.  It displays:
//
//   1. A title bar: "Plan Review" in AccentCyan.
//   2. The plan text (truncated to 20 lines) in code_bg.
//   3. A three-line key-hint footer:
//        [A] Approve        [R] Reject        [E] Edit / send feedback
//
// Keys (OnEvent):
//   a / A   — Approve (PlanApprovalResult::Approved)
//   Enter   — Approve (same as 'A' — unambiguous affirmative)
//   r / R   — Reject  (PlanApprovalResult::Rejected)
//   Esc     — Reject  (cancel = reject)
//   e / E   — Edit    (PlanApprovalResult::Edited, edit_feedback set to plan_text_)
//
// Threading
// ---------
// The component lives on the UI thread.  The dispatch layer worker thread calls
// await_user_decision(plan_text) which:
//   1. Stores the plan text under a mutex.
//   2. Blocks on std::condition_variable until the UI thread resolves.
//   3. Returns a PlanApprovalResult.
//
// The UI thread calls resolve() from within OnEvent when a key is pressed.
//
// Usage pattern (with ftxui::Modal):
// ------------------------------------
//   bool show_card = false;
//   auto card = std::make_shared<PlanApprovalCard>(theme);
//   auto root  = ftxui::Modal(base, card, &show_card);
//
//   // Worker thread:
//   show_card = true;
//   screen.PostEvent(Events::PlanApprovalShow);
//   auto result = card->await_user_decision(plan_text);
//   show_card = false;
//
// Blueprint contract: batbox::tui::PlanApprovalCard (task TUI-PLAN-T2)
// ---------------------------------------------------------------------------

#pragma once

#include <batbox/theme/Theme.hpp>
#include <batbox/tui/ThemeApply.hpp>
#include <batbox/core/Json.hpp>

#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace batbox::tui {

// =============================================================================
// PlanApprovalResult — result returned from await_user_decision()
// =============================================================================

/// The three possible outcomes of a plan approval modal.
struct PlanApprovalResult {
    /// Which button the user pressed.
    enum class Kind : uint8_t {
        Approved = 0,  ///< User approved the plan.
        Rejected = 1,  ///< User rejected the plan (Reject / Esc).
        Edited   = 2,  ///< User chose Edit — returns plan_text_ as feedback.
    };

    Kind        kind;
    std::string edit_feedback;  ///< Populated only for Kind::Edited (the plan text).

    static PlanApprovalResult approved() { return {Kind::Approved, {}}; }
    static PlanApprovalResult rejected() { return {Kind::Rejected, {}}; }
    static PlanApprovalResult edited(std::string feedback) {
        return {Kind::Edited, std::move(feedback)};
    }
};

// =============================================================================
// PlanApprovalCard — FTXUI modal Component for ExitPlanMode approval
// =============================================================================

/// Modal overlay for plan approval prompts.
///
/// Worker thread calls await_user_decision(plan_text) — it blocks until the user
/// presses one of the three action keys.  The UI thread dispatches keys via
/// OnEvent, calls resolve(), and wakes the worker thread.
///
/// Blueprint contract: class batbox::tui::PlanApprovalCard (TUI-PLAN-T2)
class PlanApprovalCard : public ftxui::ComponentBase {
public:
    // =========================================================================
    // Construction
    // =========================================================================

    /// Construct a PlanApprovalCard bound to the given theme.
    ///
    /// @param theme  Live theme reference; must outlive this component.
    explicit PlanApprovalCard(const batbox::theme::Theme& theme);

    ~PlanApprovalCard() override = default;

    // Non-copyable, non-movable (ComponentBase semantics).
    PlanApprovalCard(const PlanApprovalCard&)            = delete;
    PlanApprovalCard& operator=(const PlanApprovalCard&) = delete;
    PlanApprovalCard(PlanApprovalCard&&)                 = delete;
    PlanApprovalCard& operator=(PlanApprovalCard&&)      = delete;

    // =========================================================================
    // Blocking entry-point (called from a WORKER thread)
    // =========================================================================

    /// Block the calling thread until the user makes an approval decision.
    ///
    /// This method:
    ///   1. Populates plan_text_ under the mutex.
    ///   2. Blocks on condition_variable until the UI thread resolves.
    ///   3. Returns the PlanApprovalResult carrying the user choice.
    ///
    /// @param plan_text  The full plan text to display to the user.
    ///
    /// Thread: MUST be called from a non-UI thread.  Calling from the UI thread
    ///         would deadlock (condition_variable starves the FTXUI event loop).
    ///
    /// Blueprint contract name: await_user_decision
    [[nodiscard]]
    PlanApprovalResult await_user_decision(const std::string& plan_text);

    // =========================================================================
    // ComponentBase overrides (UI thread)
    // =========================================================================

    /// Render the plan approval card as a centered modal box.
    ///
    /// Blueprint contract name: OnRender
    ftxui::Element OnRender() override;

    /// Handle keyboard events.
    ///   a / A / Enter → Approve    r / R / Esc → Reject    e / E → Edit
    ///
    /// Returns true when the event is consumed (always for the action keys).
    ///
    /// Blueprint contract name: OnEvent
    bool OnEvent(ftxui::Event event) override;

    // =========================================================================
    // Accessors (exposed for tests)
    // =========================================================================

    /// True while await_user_decision() is blocked waiting for a response.
    [[nodiscard]] bool pending() const;

    /// The plan text currently displayed.  Empty before first await_user_decision().
    [[nodiscard]] std::string plan_text() const;

private:
    // =========================================================================
    // Internal helpers
    // =========================================================================

    /// Resolve the pending await() with result `r` and wake the worker thread.
    /// Called from OnEvent on the UI thread.
    void resolve(PlanApprovalResult r);

    // =========================================================================
    // Data members
    // =========================================================================

    const batbox::theme::Theme& theme_;  ///< Active colour palette (ref, not owned)

    // Display state — set by await_user_decision() before waiting, read by
    // OnRender() on the UI thread.  Protected by mtx_.
    std::string plan_text_;     ///< The plan text to display

    // Synchronisation between worker thread (await_user_decision) and UI thread.
    mutable std::mutex      mtx_;
    std::condition_variable cv_;
    bool                    pending_{false};   ///< true while await is blocked
    bool                    resolved_{false};  ///< set by resolve() to wake await
    PlanApprovalResult      result_{PlanApprovalResult::rejected()};  ///< Written by resolve()
};

} // namespace batbox::tui
