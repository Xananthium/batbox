// include/batbox/tui/DiffCard.hpp
// ---------------------------------------------------------------------------
// batbox::tui::DiffCard
//
// A blocking FTXUI modal that displays a unified diff (from a WriteTool or
// EditTool structured payload) and waits for the user to accept or reject it.
//
// Design
// ------
// DiffCard is an FTXUI ComponentBase subclass.  It is composed into the
// application's root component tree via ftxui::Modal(base, diff_card, &show).
// When the user presses Enter/y the card resolves Accept; when they press
// Esc/n it resolves Reject.
//
// Thread-safety
// -------------
// The component lives on the UI (main) thread.  The worker thread that
// receives a ToolResult with a diff_card payload calls:
//
//   DiffCard::Decision decision = diff_card->await(payload);
//
// which posts an Events::ModalShow event to the UI thread, then blocks on a
// std::condition_variable until the UI thread calls resolve() from within
// OnEvent().  The UI thread is unblocked, the await() returns the decision.
//
// Acceptance criteria (CPP 1.11)
// --------------------------------
// [AC1] Unified diff string is parsed into +/- rows.
// [AC2] Added lines rendered with diff_add_fg on diff_add_bg.
// [AC3] Removed lines rendered with diff_remove_fg on diff_remove_bg.
// [AC4] Scrollable when diff content exceeds screen height.
// [AC5] Enter (or 'y') → Accept.
// [AC6] Esc (or 'n')  → Reject.
// [AC7] The component is a proper FTXUI ComponentBase subclass.
// [AC8] Unit test covers all acceptance criteria.
//
// Blueprint contract: batbox::tui::DiffCard (blueprints table, task CPP 1.11)
// ---------------------------------------------------------------------------
#pragma once

#include <batbox/theme/Theme.hpp>
#include <batbox/core/Json.hpp>

#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

namespace batbox::tui {

// =============================================================================
// DiffCard — FTXUI modal Component for Edit/Write confirmation
// =============================================================================

/// Side-by-side coloured unified-diff modal.
///
/// Usage (UI thread):
///   auto card = std::make_shared<DiffCard>(theme);
///   // Wire into the root component via Modal(base, card->component(), &show)
///
/// Usage (worker thread, after UI is running):
///   DiffCard::Decision d = card->await(payload);  // blocks until user answers
///
/// Decision values: Accept (Enter/y) or Reject (Esc/n).
class DiffCard : public ftxui::ComponentBase {
public:
    // =========================================================================
    // Types
    // =========================================================================

    /// The two outcomes a user can return from the modal.
    enum class Decision : uint8_t {
        Accept = 0,  ///< User pressed Enter or 'y'
        Reject = 1,  ///< User pressed Esc or 'n'
    };

    // =========================================================================
    // Construction
    // =========================================================================

    /// Construct a DiffCard bound to the given theme.
    ///
    /// @param theme  Live theme reference; must outlive this component.
    explicit DiffCard(const batbox::theme::Theme& theme);

    // =========================================================================
    // Blocking entry-point (called from a WORKER thread)
    // =========================================================================

    /// Block the calling thread until the user accepts or rejects the diff.
    ///
    /// This method:
    ///   1. Parses the diff string from `payload` into coloured rows.
    ///   2. Posts an Events::ModalShow event to wake the UI thread.
    ///   3. Blocks on a condition_variable until the UI thread resolves.
    ///   4. Returns the user's decision.
    ///
    /// @param payload  JSON object with keys:
    ///                   "type"      : "diff_card"
    ///                   "path"      : string  — destination file path
    ///                   "operation" : string  — "create" | "overwrite" | "edit"
    ///                   "diff"      : string  — unified diff text
    ///
    /// Thread: MUST be called from a non-UI thread.  Calling from the UI thread
    ///         would deadlock (the condition_variable wait would starve the
    ///         FTXUI event loop).
    [[nodiscard]] Decision await(const batbox::Json& payload);

    // =========================================================================
    // ComponentBase overrides (UI thread)
    // =========================================================================

    /// Render the diff card as a centered modal box.
    ftxui::Element OnRender() override;

    /// Handle keyboard events: Enter/'y' → Accept, Esc/'n' → Reject,
    /// Arrow-up/k → scroll up, Arrow-down/j → scroll down.
    bool OnEvent(ftxui::Event event) override;

    // =========================================================================
    // Scroll helpers (exposed for tests)
    // =========================================================================

    /// Current scroll offset (lines from the top).
    [[nodiscard]] int scroll_offset() const { return scroll_offset_; }

    /// Total number of diff rows loaded.
    [[nodiscard]] int row_count() const { return static_cast<int>(rows_.size()); }

private:
    // =========================================================================
    // Internal types
    // =========================================================================

    enum class RowKind : uint8_t {
        Context = 0,  ///< ' ' prefix  — unchanged line
        Add     = 1,  ///< '+' prefix  — added line (diff_add colours)
        Remove  = 2,  ///< '-' prefix  — removed line (diff_remove colours)
        Header  = 3,  ///< '---', '+++', '@@ … @@' lines — rendered muted
    };

    struct DiffRow {
        RowKind     kind;
        std::string text;  ///< Raw line text (without the +/-/space prefix)
    };

    // =========================================================================
    // UI-thread resolve (called from OnEvent)
    // =========================================================================

    /// Resolve the pending await() with `d` and wake the worker thread.
    void resolve(Decision d);

protected:
    // =========================================================================
    // Parsing (protected for testability)
    // =========================================================================

    /// Parse unified diff text into rows_ (clearing any previous content).
    /// Exposed as protected so test subclasses can drive parsing without
    /// invoking await() which requires a running FTXUI loop.
    void parse_diff(const std::string& diff_text);

private:

    // =========================================================================
    // Rendering helpers
    // =========================================================================

    /// Render a single DiffRow as a coloured FTXUI Element.
    [[nodiscard]] ftxui::Element render_row(const DiffRow& row) const;

    // =========================================================================
    // Data members
    // =========================================================================

    const batbox::theme::Theme& theme_;  ///< Active colour palette (ref, not owned)

    // Diff content populated by await() before the UI thread renders.
    std::vector<DiffRow> rows_;
    std::string          path_;       ///< File path from payload["path"]
    std::string          operation_;  ///< "create" | "overwrite" | "edit"

    // Scroll state (UI thread only).
    int scroll_offset_{0};
    int visible_height_{20};  ///< Updated on each Render(); default safe value

    // Synchronisation between worker thread (await) and UI thread (OnEvent).
    std::mutex              mtx_;
    std::condition_variable cv_;
    bool                    pending_{false};   ///< true while await() is blocked
    bool                    resolved_{false};  ///< set by resolve() to wake await()
    Decision                result_{Decision::Reject};
};

} // namespace batbox::tui
