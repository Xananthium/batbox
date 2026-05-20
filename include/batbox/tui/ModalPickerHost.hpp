// include/batbox/tui/ModalPickerHost.hpp
//
// batbox::tui::ModalPickerHost — blocking wrapper around ModalPicker.
//
// Design (UX-A)
// -------------
// ModalPicker is a pure FTXUI ComponentBase (render + events, UI thread only).
// ModalPickerHost adds the worker-thread blocking API so that ModelCmd (which
// runs on a worker thread during TUI slash-command dispatch) can call
// await_selection() and block until the user picks an item or cancels.
//
// Threading model (identical to PlanApprovalCard):
//   await_selection()  — called from a WORKER thread; sets pending_ = true,
//                        then blocks on cv_ until resolve() is called.
//   pending()          — called from the UI thread (WireTui renderer lambda).
//   resolve()          — called from the ModalPicker on_select / on_cancel
//                        callbacks, which fire on the UI thread (OnEvent).
//
// Usage
// -----
//   // Build the host once (App::run):
//   auto host = std::make_shared<batbox::tui::ModalPickerHost>(tui_theme);
//
//   // Worker thread (ModelCmd, via pick_from_list_fn):
//   auto idx = host->await_selection("Select model", names, current_idx);
//
//   // WireTui uses host->pending() for visibility and host->picker() for events.

#pragma once

#include <batbox/tui/ModalPicker.hpp>

#include <condition_variable>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace batbox::tui {

// =============================================================================
// ModalPickerHost
// =============================================================================

/// Blocking wrapper around ModalPicker for use from worker threads.
///
/// The host owns the ModalPicker component.  WireTui wires it as a modal layer
/// using host->pending() for visibility and host->picker_component() for events.
class ModalPickerHost {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    /// Construct a ModalPickerHost bound to the given theme.
    ///
    /// @param theme  Active colour palette; must outlive this object.
    explicit ModalPickerHost(batbox::theme::ThemeRef theme);

    ~ModalPickerHost() = default;

    // Non-copyable, non-movable.
    ModalPickerHost(const ModalPickerHost&)            = delete;
    ModalPickerHost& operator=(const ModalPickerHost&) = delete;
    ModalPickerHost(ModalPickerHost&&)                 = delete;
    ModalPickerHost& operator=(ModalPickerHost&&)      = delete;

    // -------------------------------------------------------------------------
    // Blocking entry-point (WORKER thread)
    // -------------------------------------------------------------------------

    /// Display the picker with the given items and block until the user selects
    /// or cancels.
    ///
    /// @param title        Header text displayed in the picker title bar.
    /// @param items        The list of strings to display.
    /// @param current_idx  0-based index of the currently-active item.
    /// @returns            The 0-based original index of the chosen item, or
    ///                     std::nullopt if the user pressed Escape (cancel).
    ///
    /// Thread: MUST be called from a non-UI thread.  Calling from the UI thread
    ///         would deadlock (condition_variable starves the FTXUI event loop).
    [[nodiscard]]
    std::optional<std::size_t> await_selection(
        std::string_view             title,
        std::span<const std::string> items,
        std::size_t                  current_idx);

    // -------------------------------------------------------------------------
    // UI-thread accessors (used by WireTui)
    // -------------------------------------------------------------------------

    /// True while await_selection() is blocked waiting for a response.
    /// Called from the UI thread by the WireTui renderer lambda.
    [[nodiscard]] bool pending() const;

    /// The underlying ModalPicker component.
    /// WireTui uses this to call Render() and OnEvent().
    /// Must only be used on the UI thread.
    [[nodiscard]] ModalPicker* picker_component() noexcept { return picker_.get(); }

private:
    // -------------------------------------------------------------------------
    // Internal helpers
    // -------------------------------------------------------------------------

    /// Called by the ModalPicker on_select callback (UI thread) to wake the
    /// worker thread with the chosen index.
    void resolve_select(int original_idx);

    /// Called by the ModalPicker on_cancel callback (UI thread) to wake the
    /// worker thread with nullopt.
    void resolve_cancel();

    // -------------------------------------------------------------------------
    // Data members
    // -------------------------------------------------------------------------

    /// The underlying ModalPicker component.  Reconstructed per call to
    /// await_selection() (set_items + reset is sufficient for reuse).
    std::shared_ptr<ModalPicker> picker_;

    // Synchronisation between worker thread (await_selection) and UI thread.
    mutable std::mutex      mtx_;
    std::condition_variable cv_;
    bool                    pending_{false};   ///< true while await is blocked
    bool                    resolved_{false};  ///< set by resolve_*() to wake await
    std::optional<std::size_t> result_;        ///< populated by resolve_select()

    /// Theme reference — needed to reconstruct picker_ per call if needed.
    const batbox::theme::Theme& theme_;
};

} // namespace batbox::tui
