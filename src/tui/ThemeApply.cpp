// src/tui/ThemeApply.cpp
// ---------------------------------------------------------------------------
// batbox::tui::color_for() — ThemeRole → ftxui::Color dispatch.
// ---------------------------------------------------------------------------
#include <batbox/tui/ThemeApply.hpp>
#include <batbox/theme/Theme.hpp>

#include <stdexcept>

namespace batbox::tui {

ftxui::Color color_for(const batbox::theme::Theme& theme, ThemeRole role) {
    switch (role) {
        case ThemeRole::Bg:           return theme.bg;
        case ThemeRole::Fg:           return theme.fg;
        case ThemeRole::AccentMagenta:return theme.accent_magenta;
        case ThemeRole::AccentCyan:   return theme.accent_cyan;
        case ThemeRole::Muted:        return theme.muted;
        case ThemeRole::Success:      return theme.success;
        case ThemeRole::Error:        return theme.error;
        case ThemeRole::DiffAddFg:    return theme.diff_add_fg;
        case ThemeRole::DiffAddBg:    return theme.diff_add_bg;
        case ThemeRole::DiffRemoveFg: return theme.diff_remove_fg;
        case ThemeRole::DiffRemoveBg: return theme.diff_remove_bg;
        case ThemeRole::PromptPrefix: return theme.prompt_prefix;
        case ThemeRole::CodeBg:       return theme.code_bg;
    }
    // Unreachable on well-formed code; guard against UB from a cast.
    throw std::logic_error("color_for: unknown ThemeRole");
}

} // namespace batbox::tui
