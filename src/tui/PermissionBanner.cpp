// src/tui/PermissionBanner.cpp
// =============================================================================
// batbox::tui::PermissionBanner — one-line permission mode banner component.
//
// See include/batbox/tui/PermissionBanner.hpp for design notes and API contract.
//
// Blueprint contract: batbox::tui::PermissionBanner (CPP 1.16)
// =============================================================================

#include <batbox/tui/PermissionBanner.hpp>
#include <batbox/permissions/PermissionMode.hpp>
#include <batbox/tui/ThemeApply.hpp>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include <string>
#include <string_view>

using namespace ftxui;

namespace batbox::tui {

// ---------------------------------------------------------------------------
// Shift+Tab escape sequence (same constant as used in Keybindings.cpp)
// ---------------------------------------------------------------------------

namespace {

constexpr std::string_view kInputShiftTab = "\x1b[Z";

/// Return a human-readable label for the given mode (shown in the status bar
/// and the banner itself).  Uses a slightly friendlier form than to_string().
std::string mode_display_label(batbox::permissions::PermissionMode mode) {
    using PM = batbox::permissions::PermissionMode;
    switch (mode) {
        case PM::Default:     return "default";
        case PM::Plan:        return "plan";
        case PM::AcceptEdits: return "accept-edits";
        case PM::Nuclear:     return "nuclear";
    }
    return "default";
}

} // anonymous namespace

// =============================================================================
// Construction
// =============================================================================

PermissionBanner::PermissionBanner(const batbox::theme::Theme& theme,
                                    ModeChangedCallback         on_mode_changed)
    : theme_(theme)
    , on_mode_changed_(std::move(on_mode_changed))
{}

// =============================================================================
// Mode access
// =============================================================================

batbox::permissions::PermissionMode PermissionBanner::current_mode() const noexcept {
    return mode_;
}

void PermissionBanner::set_mode_direct(batbox::permissions::PermissionMode mode) {
    // Cancel any in-flight confirmation.
    confirm_pending_ = false;

    if (mode_ == mode) {
        return; // No change — skip callback.
    }

    mode_ = mode;
    notify_changed();
}

// =============================================================================
// OnRender
// =============================================================================

ftxui::Element PermissionBanner::OnRender() {
    using PM = batbox::permissions::PermissionMode;

    // Confirmation prompt takes priority over the normal banner.
    if (confirm_pending_) {
        return render_confirm_prompt();
    }

    switch (mode_) {
        case PM::Nuclear:
            return render_nuclear_banner();
        case PM::Plan:
        case PM::AcceptEdits:
            return render_mode_label();
        case PM::Default:
        default:
            return emptyElement();
    }
}

// =============================================================================
// OnEvent
// =============================================================================

bool PermissionBanner::OnEvent(ftxui::Event event) {
    if (event.is_mouse()) {
        return false;
    }

    const auto& inp = event.input();

    // -------------------------------------------------------------------------
    // Confirmation modal: intercept 'y' / everything else when pending.
    // -------------------------------------------------------------------------
    if (confirm_pending_) {
        if (inp == "y" || inp == "Y") {
            confirm_nuclear();
            return true;
        }
        // Any other key cancels Nuclear confirmation.
        cancel_confirm();
        return true;
    }

    // -------------------------------------------------------------------------
    // Shift+Tab: cycle to next mode.
    // -------------------------------------------------------------------------
    if (inp == kInputShiftTab) {
        using PM = batbox::permissions::PermissionMode;
        PM next = batbox::permissions::cycle_next(mode_);
        request_mode_change(next);
        return true;
    }

    return false;
}

// =============================================================================
// Internal render helpers
// =============================================================================

ftxui::Element PermissionBanner::render_nuclear_banner() const {
    // Full-width magenta background bar with white bold text.
    // banner_text() returns the canonical string including the ☢️ emoji.
    std::string label(batbox::permissions::banner_text(
        batbox::permissions::PermissionMode::Nuclear));

    return hbox({
        text("  "),
        text(label) | bold,
        text("  ") | flex,
    })
    | color(ftxui::Color::White)
    | bgcolor(color_for(theme_, ThemeRole::AccentMagenta));
}

ftxui::Element PermissionBanner::render_mode_label() const {
    std::string label = "  " + mode_display_label(mode_) + " mode  ";

    return hbox({
        text(label) | color(color_for(theme_, ThemeRole::Muted)),
        filler(),
    });
}

ftxui::Element PermissionBanner::render_confirm_prompt() const {
    auto accent = color_for(theme_, ThemeRole::AccentMagenta);
    auto fg     = color_for(theme_, ThemeRole::Fg);
    auto muted  = color_for(theme_, ThemeRole::Muted);

    return hbox({
        text("  ") | ftxui::color(fg),
        text("Entering NUCLEAR MODE. Confirm? ") | ftxui::color(accent) | bold,
        text("(y/N)") | ftxui::color(muted),
        text("  ") | flex,
    });
}

// =============================================================================
// Internal mode-change logic
// =============================================================================

void PermissionBanner::request_mode_change(batbox::permissions::PermissionMode target) {
    using PM = batbox::permissions::PermissionMode;

    if (target == PM::Nuclear) {
        // Nuclear requires explicit confirmation.
        // Record the mode we are coming from so we can revert on cancel.
        pre_confirm_mode_ = mode_;
        confirm_pending_  = true;
        // Do NOT change mode_ yet — we wait for 'y'.
        return;
    }

    // All other modes apply immediately.
    if (mode_ != target) {
        mode_ = target;
        notify_changed();
    }
}

void PermissionBanner::confirm_nuclear() {
    confirm_pending_ = false;
    mode_ = batbox::permissions::PermissionMode::Nuclear;
    notify_changed();
}

void PermissionBanner::cancel_confirm() {
    confirm_pending_ = false;
    // mode_ has not changed — we were still on pre_confirm_mode_.
    // No callback needed (nothing actually changed).
}

void PermissionBanner::notify_changed() {
    if (on_mode_changed_) {
        on_mode_changed_(mode_, mode_display_label(mode_));
    }
}

// =============================================================================
// Factory
// =============================================================================

ftxui::Component make_permission_banner(
    const batbox::theme::Theme&           theme,
    PermissionBanner::ModeChangedCallback on_mode_changed)
{
    return std::make_shared<PermissionBanner>(theme, std::move(on_mode_changed));
}

} // namespace batbox::tui
