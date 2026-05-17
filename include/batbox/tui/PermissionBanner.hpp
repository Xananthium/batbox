// include/batbox/tui/PermissionBanner.hpp
// =============================================================================
// batbox::tui::PermissionBanner — top-of-screen one-line permission mode banner.
//
// Design
// ------
// PermissionBanner is an FTXUI ComponentBase subclass that renders a single
// horizontal bar above the chat view showing the current permission mode.
//
// Visual appearance:
//   Default     :  [no banner rendered — emptyElement()]
//   Plan        :  "  plan mode  " in muted colour, normal bg
//   AcceptEdits :  "  accept-edits mode  " in muted colour, normal bg
//   Nuclear     :  "  ☢️ NUCLEAR MODE — ALL PERMISSIONS BYPASSED  " on magenta bg,
//                  white fg, bold — full-width bar
//
// Keybinding: Shift+Tab (ReplAction::CycleMode) cycles modes in order:
//   Default → Plan → AcceptEdits → Nuclear → Default → ...
//
// Nuclear requires confirmation before activating (Decision of Record #6):
//   A one-line inline prompt "[Entering NUCLEAR MODE. Confirm? (y/N)]" is shown.
//   The user must press 'y' to confirm or any other key to cancel.
//   CLI --nuclear flag bypasses the confirmation modal (set_mode_direct()).
//
// Integration with InputBar:
//   After a mode change the caller must call input_bar->set_mode(label)
//   to update the status line.  PermissionBanner owns the mode state and
//   surfaces it via current_mode() / on_mode_changed callback.
//
// Thread safety: UI-thread only.
//
// Blueprint contract: batbox::tui::PermissionBanner (CPP 1.16)
// =============================================================================

#pragma once

#include <batbox/permissions/PermissionMode.hpp>
#include <batbox/theme/Theme.hpp>
#include <batbox/tui/ThemeApply.hpp>

#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <functional>
#include <string>

namespace batbox::tui {

// =============================================================================
// PermissionBanner
// =============================================================================

/// FTXUI ComponentBase: renders a one-line permission mode indicator bar
/// at the top of the chat area and handles Shift+Tab mode cycling.
///
/// Blueprint contract: class batbox::tui::PermissionBanner (CPP 1.16)
class PermissionBanner : public ftxui::ComponentBase {
public:
    // =========================================================================
    // Types
    // =========================================================================

    /// Called on the UI thread whenever the active mode changes.
    /// The callback receives the new mode and its canonical label string.
    using ModeChangedCallback = std::function<void(batbox::permissions::PermissionMode,
                                                    std::string_view label)>;

    // =========================================================================
    // Construction
    // =========================================================================

    /// Construct a PermissionBanner.
    ///
    /// @param theme            Active colour palette (ThemeRef = const Theme&).
    ///                         Must outlive this component.
    /// @param on_mode_changed  Optional callback invoked after every successful
    ///                         mode change (including CLI direct-set).  Called on
    ///                         the UI thread.  May be nullptr.
    explicit PermissionBanner(const batbox::theme::Theme& theme,
                               ModeChangedCallback         on_mode_changed = nullptr);

    ~PermissionBanner() override = default;

    PermissionBanner(const PermissionBanner&)            = delete;
    PermissionBanner& operator=(const PermissionBanner&) = delete;
    PermissionBanner(PermissionBanner&&)                 = delete;
    PermissionBanner& operator=(PermissionBanner&&)      = delete;

    // =========================================================================
    // Mode access
    // =========================================================================

    /// Return the currently active permission mode.
    [[nodiscard]] batbox::permissions::PermissionMode current_mode() const noexcept;

    /// Bypass confirmation and set mode directly (used by CLI --nuclear flag).
    ///
    /// Does NOT invoke the confirmation modal regardless of target mode.
    /// Fires on_mode_changed callback if the mode actually changes.
    ///
    /// Thread: UI thread only.
    ///
    /// Blueprint contract name: set_mode_direct
    void set_mode_direct(batbox::permissions::PermissionMode mode);

    // =========================================================================
    // FTXUI ComponentBase overrides
    // =========================================================================

    /// Render the one-line banner element.
    ///
    /// Returns:
    ///   - Nuclear mode: full-width magenta background bar with white bold text.
    ///   - Plan / AcceptEdits: muted one-line label bar.
    ///   - Default: emptyElement() — zero height, no visual presence.
    ///   - Confirmation pending: inline prompt bar "(y/N) Confirm NUCLEAR MODE?"
    ///
    /// Blueprint contract name: OnRender
    ftxui::Element OnRender() override;

    /// Handle keyboard events.
    ///
    /// Consumed events:
    ///   - Shift+Tab (\x1b[Z): cycle to next mode (may enter confirm state).
    ///   - 'y' when confirm_pending_: activate Nuclear mode.
    ///   - Any other key when confirm_pending_: cancel, revert to previous mode.
    ///
    /// Returns true when the event is consumed; false otherwise.
    ///
    /// Blueprint contract name: OnEvent
    bool OnEvent(ftxui::Event event) override;

private:
    // =========================================================================
    // Internal helpers
    // =========================================================================

    /// Render the Nuclear mode banner (magenta bg, white bold text).
    [[nodiscard]] ftxui::Element render_nuclear_banner() const;

    /// Render the non-nuclear mode label bar (muted, normal bg).
    [[nodiscard]] ftxui::Element render_mode_label() const;

    /// Render the Nuclear confirmation prompt bar.
    [[nodiscard]] ftxui::Element render_confirm_prompt() const;

    /// Apply a mode change:
    ///   - For Nuclear: enter confirm_pending_ state instead.
    ///   - For other modes: apply immediately and fire callback.
    void request_mode_change(batbox::permissions::PermissionMode target);

    /// Commit the pending mode change (called when user confirms with 'y').
    void confirm_nuclear();

    /// Cancel the pending Nuclear confirmation (revert to pre_confirm_mode_).
    void cancel_confirm();

    /// Fire on_mode_changed_ with the current mode + label if callback is set.
    void notify_changed();

    // =========================================================================
    // State
    // =========================================================================

    const batbox::theme::Theme& theme_;         ///< Active colour palette
    ModeChangedCallback         on_mode_changed_; ///< Optional observer

    batbox::permissions::PermissionMode mode_{
        batbox::permissions::PermissionMode::Default};  ///< Active mode

    /// True while waiting for 'y' confirmation before activating Nuclear.
    bool confirm_pending_{false};

    /// Mode held before entering confirm state (reverted on cancel).
    batbox::permissions::PermissionMode pre_confirm_mode_{
        batbox::permissions::PermissionMode::Default};
};

// =============================================================================
// Factory
// =============================================================================

/// Create a heap-allocated PermissionBanner wrapped in ftxui::Component.
///
/// Blueprint contract name: make_permission_banner
[[nodiscard]]
ftxui::Component make_permission_banner(
    const batbox::theme::Theme&                     theme,
    PermissionBanner::ModeChangedCallback           on_mode_changed = nullptr);

} // namespace batbox::tui
