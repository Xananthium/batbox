// include/batbox/tui/ModalPicker.hpp
//
// batbox::tui::ModalPicker — generic list-pick modal.
//
// Design
// ------
// ModalPicker is an FTXUI ComponentBase subclass that renders a bordered
// floating panel containing:
//
//   1. A title bar (themed with AccentCyan).
//   2. An optional fuzzy-filter Input (shown when constructed with
//      show_filter=true).  Typing narrows the visible item list to entries
//      whose labels contain the filter string (case-insensitive substring).
//   3. A scrollable item list rendered as a vertical stack of selectable
//      rows. The selected row is highlighted with AccentMagenta fg + Bg bg.
//      All other rows use Fg on Bg colours.
//   4. A one-line footer: "↑↓ navigate · Enter select · Esc cancel".
//
// Keyboard contract (OnEvent):
//   ArrowUp / k   — move selection up (wraps)
//   ArrowDown / j — move selection down (wraps)
//   Enter         — invoke on_select(selected_index) and return true
//   Escape        — invoke on_cancel() and return true
//   Any printable character (filter mode only) — forwarded to filter Input
//   Backspace (filter mode only)               — forwarded to filter Input
//
// Usage pattern (caller wires with ftxui::Modal):
// -----------------------------------------------
//   bool show_picker = false;
//   std::vector<std::string> items = { "gpt-4o", "claude-3-5-sonnet", ... };
//
//   auto picker = std::make_shared<batbox::tui::ModalPicker>(
//       theme_,
//       "Select model",
//       items,
//       /*show_filter=*/true,
//       [&](int idx) { current_model_ = items[idx]; show_picker = false; },
//       [&]()        { show_picker = false; }
//   );
//
//   auto root = ftxui::Modal(base_component, picker, &show_picker);
//
// Thread safety
// -------------
// ModalPicker must only be constructed and used on the UI thread (FTXUI
// Component rule — Render/OnEvent always run on the UI thread).
//
// The items vector and title string are copied at construction time so the
// caller is free to modify them after construction without data races.

#pragma once

#include <batbox/theme/Theme.hpp>
#include <batbox/tui/ThemeApply.hpp>

#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <functional>
#include <string>
#include <vector>

namespace batbox::tui {

// =============================================================================
// ModalPicker
// =============================================================================

/// Generic scrollable list-pick modal.
///
/// Callers use ftxui::Modal(base, picker_ptr, &show_flag) to layer this
/// component over the existing UI.  The modal captures all keyboard events
/// when visible; the base component is dimmed and does not receive input.
class ModalPicker : public ftxui::ComponentBase {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    /// Construct a ModalPicker.
    ///
    /// @param theme        Active colour palette (ThemeRef = const Theme&).
    /// @param title        Header text rendered in the title bar.
    /// @param items        Full list of string labels to display.  Copied.
    /// @param show_filter  When true, a fuzzy-filter Input is rendered above
    ///                     the list.  Keystrokes narrow visible items.
    /// @param on_select    Called with the original (pre-filter) index when
    ///                     the user confirms a selection.  Must not be null.
    /// @param on_cancel    Called when the user presses Escape.  Must not be null.
    ModalPicker(batbox::theme::ThemeRef          theme,
                std::string                       title,
                std::vector<std::string>          items,
                bool                              show_filter,
                std::function<void(int)>          on_select,
                std::function<void()>             on_cancel);

    ~ModalPicker() override = default;

    // Non-copyable, non-movable (ComponentBase semantics).
    ModalPicker(const ModalPicker&)            = delete;
    ModalPicker& operator=(const ModalPicker&) = delete;
    ModalPicker(ModalPicker&&)                 = delete;
    ModalPicker& operator=(ModalPicker&&)      = delete;

    // -------------------------------------------------------------------------
    // ComponentBase overrides
    // -------------------------------------------------------------------------

    /// Render the picker panel as a floating bordered box (OnRender override).
    ftxui::Element OnRender() override;

    /// Handle keyboard events.  Returns true when the event is consumed.
    bool OnEvent(ftxui::Event event) override;

    // -------------------------------------------------------------------------
    // Runtime control
    // -------------------------------------------------------------------------

    /// Replace the displayed items and reset filter + selection to defaults.
    ///
    /// Call this before showing the picker to update the list (e.g. after a
    /// model list refresh) without reconstructing the component.
    void set_items(std::vector<std::string> new_items);

    /// Reset the filter string and selected index to their initial state.
    /// Useful when re-showing the picker after a previous selection.
    void reset();

private:
    // -------------------------------------------------------------------------
    // Internal helpers
    // -------------------------------------------------------------------------

    /// Rebuild filtered_indices_ from items_ + filter_.
    void rebuild_filtered();

    /// Return the label for the currently highlighted entry, or "" if none.
    std::string current_label() const;

    // -------------------------------------------------------------------------
    // Construction-time data
    // -------------------------------------------------------------------------
    const batbox::theme::Theme& theme_;   ///< Active colour palette
    std::string                  title_;  ///< Header text
    std::vector<std::string>     items_;  ///< Full unfiltered item list
    bool                         show_filter_;
    std::function<void(int)>     on_select_;
    std::function<void()>        on_cancel_;

    // -------------------------------------------------------------------------
    // Mutable state (UI-thread only)
    // -------------------------------------------------------------------------
    std::string              filter_;           ///< Current filter string
    std::vector<int>         filtered_indices_; ///< Indices into items_ that pass filter
    int                      cursor_{0};        ///< Index into filtered_indices_
};

} // namespace batbox::tui
