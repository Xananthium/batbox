// include/batbox/theme/Theme.hpp
// ---------------------------------------------------------------------------
// batbox::theme::Theme — colour-role struct carrying 13 ftxui::Color values.
//
// Design
// ------
// Theme is a plain-data struct.  Each field maps to one semantic colour role
// used throughout the TUI (backgrounds, foregrounds, accents, diffs, etc.).
// Components receive a `ThemeRef` (= const Theme&) at construction time and
// call batbox::tui::color_for(theme, role) to obtain the concrete colour.
//
// Five named palettes are defined in src/theme/themes.cpp:
//   miss-kittin     (default) — electroclash: magenta/cyan/near-black
//   stock-exchange            — finance terminal: cyan/yellow on black
//   frank-sinatra             — smoky 50s: sepia/cream/black
//   monochrome                — strict white-on-black, no accents
//   classic                   — original claude-code colour set
//
// Loading
// -------
// Use theme_from_name(name) to look up by string, falling back to miss-kittin
// for unknown names.  Use load_theme(settings) to incorporate the BATBOX_THEME
// env-var override on top of settings.theme.
//
// Both functions are declared in this header and defined in Theme.cpp and
// themes.cpp respectively.
// ---------------------------------------------------------------------------
#pragma once

#include <ftxui/screen/color.hpp>

#include <string>
#include <string_view>

// Forward-declare Settings so themes.cpp can include it without circular deps.
namespace batbox::config { struct Settings; }

namespace batbox::theme {

// ============================================================================
// Theme struct
// ============================================================================

/// All colour roles used by the batbox TUI.
///
/// Members are plain ftxui::Color values (24-bit RGB on truecolour terminals,
/// gracefully degraded by FTXUI on 256-colour and 8-colour terminals).
///
/// The `name` field is the canonical lowercase kebab-case identifier
/// (e.g. "miss-kittin") used for display, settings persistence, and env-var
/// matching.
struct Theme {
    // ---- background / foreground ----------------------------------------
    ftxui::Color bg;              ///< Global terminal background
    ftxui::Color fg;              ///< Primary text foreground

    // ---- accents --------------------------------------------------------
    ftxui::Color accent_magenta;  ///< Hot accent (status dot, splash, prompt prefix)
    ftxui::Color accent_cyan;     ///< Cool accent (cost meter, todo dots)

    // ---- UI chrome -------------------------------------------------------
    ftxui::Color muted;           ///< Timestamps, secondary status text
    ftxui::Color success;         ///< Positive outcomes, completed indicators
    ftxui::Color error;           ///< Errors, failures, destructive warnings

    // ---- diff colours ---------------------------------------------------
    ftxui::Color diff_add_fg;     ///< Added-line foreground in diff cards
    ftxui::Color diff_add_bg;     ///< Added-line background in diff cards
    ftxui::Color diff_remove_fg;  ///< Removed-line foreground in diff cards
    ftxui::Color diff_remove_bg;  ///< Removed-line background in diff cards

    // ---- input bar -------------------------------------------------------
    ftxui::Color prompt_prefix;   ///< The `>` glyph in the input bar

    // ---- code blocks -----------------------------------------------------
    ftxui::Color code_bg;         ///< Background tint for fenced code blocks

    // ---- identity --------------------------------------------------------
    std::string name;             ///< Canonical theme identifier ("miss-kittin", etc.)
};

// ============================================================================
// ThemeRef — idiomatic alias used at component construction
// ============================================================================

/// Components take a ThemeRef so they hold a reference to the live theme
/// owned by the application without copying 13 Colors.
using ThemeRef = const Theme&;

// ============================================================================
// theme_from_name()
// ============================================================================

/// Return the named theme, or miss-kittin as the fallback if `name` is unknown
/// or empty.
///
/// Comparison is case-sensitive.  Valid names: "miss-kittin", "stock-exchange",
/// "frank-sinatra", "monochrome", "classic".
[[nodiscard]]
Theme theme_from_name(std::string_view name);

// ============================================================================
// load_theme()
// ============================================================================

/// Resolve the active theme from settings + environment.
///
/// Precedence (highest first):
///   1. BATBOX_THEME environment variable (if set and non-empty)
///   2. settings.theme field
///   3. miss-kittin default
///
/// Unknown names at any level fall back to miss-kittin.
[[nodiscard]]
Theme load_theme(const batbox::config::Settings& settings);

} // namespace batbox::theme
