// include/batbox/tui/QuestionCard.hpp
// ---------------------------------------------------------------------------
// batbox::tui::QuestionCard — AskUserQuestion modal with numbered option list.
//
// Design
// ------
// QuestionCard is an FTXUI ComponentBase subclass that renders a floating
// question dialog when the model calls AskUserQuestion.  It displays:
//
//   1. A header chip: " <header> " in AccentMagenta on CodeBg (max 12 chars,
//      truncated with ellipsis if longer).
//   2. A bold question line in Fg.
//   3. A separator.
//   4. Numbered option rows (1–N):
//        row i:  [▸ or space] [○/●/☐/☒] i. <bold label>
//                             <dim description, indented>
//      In single-select mode: ○ for unselected, ● for cursor position.
//      In multi-select mode:  ☐ for unchecked, ☒ for checked.
//   5. Optional synthetic row "Type something…" when allow_freeform = true.
//   6. Optional synthetic row "Chat about this" when allow_escape_hatch = true.
//   7. A separator.
//   8. Footer: "Enter to select · ↑/↓ to navigate · Esc to cancel" (single-select)
//              "Space to toggle · Enter to confirm · Esc to cancel" (multi-select)
//
// Width: 52–80 chars (same constraint as PermissionCard).
//
// Threading
// ---------
// This component's OnRender path is UI-thread-only.  The worker thread calls
// set_spec() to post a new question and then await_user_answer() to block until
// the user resolves it.  OnEvent() resolves the card (UI thread) by calling
// resolve(), which notifies the condition variable.
//
// Usage pattern (with ftxui::Modal):
// ------------------------------------
//   bool show_card = false;
//   auto card = std::make_shared<QuestionCard>(theme);
//   auto root  = ftxui::Modal(base, card, &show_card);
//
//   // Worker thread (T3 pattern):
//   card->set_spec(payload);
//   show_card = true;
//   screen.PostEvent(Events::QuestionShow);
//   auto result = card->await_user_answer();
//   show_card = false;
//
// Blueprint contract: batbox::tui::QuestionCard (task TUI-ASKQ-T3)
// Resolved in TUI-ASKQ-T3
// ---------------------------------------------------------------------------

#pragma once

#include <batbox/theme/Theme.hpp>
#include <batbox/tui/ThemeApply.hpp>
#include <batbox/tui/Events.hpp>

#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

namespace batbox::tui {

// =============================================================================
// QuestionCard — FTXUI modal Component for AskUserQuestion
// =============================================================================

/// Modal overlay for AskUserQuestion prompts.
///
/// The card renders when is_visible() returns true.  Keyboard handling and
/// the blocking await_user_answer() API are implemented as of TUI-ASKQ-T3.
///
/// Blueprint contract: class batbox::tui::QuestionCard (TUI-ASKQ-T3)
class QuestionCard : public ftxui::ComponentBase {
public:
    // =========================================================================
    // Construction
    // =========================================================================

    /// Construct a QuestionCard bound to the given theme.
    ///
    /// @param theme  Live theme reference; must outlive this component.
    explicit QuestionCard(const batbox::theme::Theme& theme);

    ~QuestionCard() override = default;

    // Non-copyable, non-movable (ComponentBase semantics).
    QuestionCard(const QuestionCard&)            = delete;
    QuestionCard& operator=(const QuestionCard&) = delete;
    QuestionCard(QuestionCard&&)                 = delete;
    QuestionCard& operator=(QuestionCard&&)      = delete;

    // =========================================================================
    // State management (may be called from any thread)
    // =========================================================================

    /// Load a question for display and reset selection state.
    ///
    /// Stores a copy of @p payload, resets cursor to 0, and clears checkbox
    /// state.  Safe to call from a worker thread; visible_ is written atomically.
    ///
    /// @param payload  The question specification from QuestionShowPayload.
    void set_spec(const QuestionShowPayload& payload);

    /// Hide the card (it will stop rendering).
    void hide();

    /// True when the card has a loaded question and should be rendered.
    [[nodiscard]] bool is_visible() const noexcept;

    // =========================================================================
    // ComponentBase overrides (UI thread)
    // =========================================================================

    /// Render the question card as a centered modal box.
    ///
    /// Blueprint contract name: OnRender
    ftxui::Element OnRender() override;

    /// Keyboard handler — full navigation + selection logic (TUI-ASKQ-T3).
    ///
    /// Handles:
    ///   ↑ / k     — move cursor up (clamped at 0)
    ///   ↓ / j     — move cursor down (clamped at last row)
    ///   Space     — toggle checkbox (multi_select only; no-op otherwise)
    ///   1–9       — jump cursor to that 1-indexed row (if in range)
    ///   Enter     — confirm selection; resolves await_user_answer()
    ///   Esc       — cancel; resolves await_user_answer() with cancelled=true
    ///
    /// Blueprint contract name: OnEvent
    bool OnEvent(ftxui::Event event) override;

    // =========================================================================
    // Blocking API (worker thread — TUI-ASKQ-T3)
    // =========================================================================

    /// Block the calling thread until the user confirms or cancels the question.
    ///
    /// Must be called AFTER set_spec() has populated a valid question.  If
    /// called before set_spec() with an empty payload, returns immediately with
    /// cancelled=true to prevent deadlock.
    ///
    /// Safe to call from any worker thread.  The UI thread's OnEvent() will
    /// unblock this via the condition variable when the user presses Enter or Esc.
    ///
    /// @returns  The user's resolved answer (chosen_labels, freeform_text,
    ///           escape_hatch, cancelled).
    [[nodiscard]]
    QuestionResolvedPayload await_user_answer();

    // =========================================================================
    // Accessors (exposed for tests)
    // =========================================================================

    /// Current cursor row index (0-based over visible option rows).
    [[nodiscard]] int cursor_index() const noexcept { return cursor_index_; }

    /// Multi-select checkbox state vector (parallel to payload_.labels +
    /// synthetic rows).  Empty for single-select cards.
    [[nodiscard]] const std::vector<bool>& checked() const noexcept { return checked_; }

private:
    // =========================================================================
    // Internal helpers
    // =========================================================================

    /// Truncate a header string to at most 12 UTF-8 characters, appending
    /// a Unicode ellipsis ("…") when truncation occurs.
    static std::string truncate_header(const std::string& header);

    /// Return the total number of selectable rows (labels + synthetic rows).
    /// Caller must NOT hold mtx_ — this reads payload_ without locking
    /// (safe only when called from UI thread after set_spec returns).
    [[nodiscard]] int total_rows() const noexcept;

    /// Signal the waiting worker thread with a resolved result.
    /// Notifies the condition variable and hides the card.
    /// Called from OnEvent() on the UI thread.
    void resolve(QuestionResolvedPayload r);

    // =========================================================================
    // Data members
    // =========================================================================

    const batbox::theme::Theme& theme_;  ///< Active colour palette (ref, not owned)

    // Display state — written by set_spec() (any thread), read by OnRender()
    // (UI thread).  Protected by mtx_.
    mutable std::mutex    mtx_;
    QuestionShowPayload   payload_{};  ///< Current question spec

    // Visibility flag — written atomically by set_spec() / hide().
    std::atomic<bool> visible_{false};

    // Selection state — written by OnEvent() (UI thread), read by OnRender()
    // (same UI thread — no additional lock needed for these after T3).
    int               cursor_index_{0};   ///< Focused row (0-based)
    std::vector<bool> checked_;           ///< Multi-select per-row check state

    // Synchronisation for await_user_answer().
    std::condition_variable cv_;
    bool                    resolved_{false};
    QuestionResolvedPayload result_{};
};

} // namespace batbox::tui
