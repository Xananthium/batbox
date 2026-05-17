// include/batbox/tui/ThemeApply.hpp
// ---------------------------------------------------------------------------
// batbox::tui::ThemeRole + color_for()
//
// Design
// ------
// ThemeRole is a scoped enum that names every semantic colour slot understood
// by the TUI layer.  It mirrors the field set of batbox::theme::Theme but
// provides a type-safe tag for runtime dispatch.
//
// color_for(theme, role) performs the role → ftxui::Color lookup.  It is a
// free function (not a class) so components can call it inline without holding
// a reference to a helper object:
//
//   ftxui::Color c = batbox::tui::color_for(theme_, ThemeRole::AccentMagenta);
//   element = element | ftxui::color(c);
//
// ThemeRef (defined in Theme.hpp) is const Theme& — pass it to components at
// construction rather than copying the whole struct.
// ---------------------------------------------------------------------------
#pragma once

#include <batbox/theme/Theme.hpp>

#include <ftxui/screen/color.hpp>

namespace batbox::tui {

// ============================================================================
// ThemeRole — semantic colour-role tag
// ============================================================================

/// Every colour role exposed by batbox::theme::Theme, as a scoped enum.
///
/// The enumerator names are PascalCase versions of the Theme struct fields:
///   Theme::bg              → ThemeRole::Bg
///   Theme::fg              → ThemeRole::Fg
///   Theme::accent_magenta  → ThemeRole::AccentMagenta
///   … etc.
enum class ThemeRole {
    Bg,
    Fg,
    AccentMagenta,
    AccentCyan,
    Muted,
    Success,
    Error,
    DiffAddFg,
    DiffAddBg,
    DiffRemoveFg,
    DiffRemoveBg,
    PromptPrefix,
    CodeBg,
};

// ============================================================================
// color_for()
// ============================================================================

/// Return the ftxui::Color for the given role from the supplied theme.
///
/// This function is the single indirection point used by all TUI components.
/// It is intentionally simple: a switch over the 13 known roles.  The compiler
/// will warn (or error with -Wswitch) if a new ThemeRole value is added without
/// updating this function.
///
/// Parameters:
///   theme — the active theme (a ThemeRef / const Theme&)
///   role  — which colour slot to look up
///
/// Returns the corresponding ftxui::Color from `theme`.
[[nodiscard]]
ftxui::Color color_for(const batbox::theme::Theme& theme, ThemeRole role);

} // namespace batbox::tui
