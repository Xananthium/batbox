// include/batbox/tui/Splash.hpp
// ---------------------------------------------------------------------------
// batbox::tui::Splash    — legacy full-screen boot splash (kept for compat).
// batbox::tui::SplashBanner — Claude-Code-style non-modal banner component.
//
// =========================================================================
// Splash (legacy — preserved for existing tests)
// =========================================================================
// Boot splash Component: bat ASCII art + version + daily tagline.
//
// Lifecycle:
//   1. Construct Splash with an on_done callback and a ThemeRef.
//   2. Call ScreenManager::swap_root(Splash::Make(...)).
//   3. Callback fires after 1.5 s or on any keypress.
//
// BATBOX_NO_SPLASH=true
//   Splash::should_skip() returns true.  Callers skip straight to main layout.
//
// =========================================================================
// SplashBanner (TUI-FLOW-T4 / TUI-FLOW-T10)
// =========================================================================
// Non-modal, inline banner rendered at the top of the main layout until the
// user submits the first prompt.  Replaces the full-screen Splash in WireTui.
//
// Visual layout (Claude-Code-style double-border box):
//
//   ╭──── BatBox v0.1.0 ─────────────────────────────────────────╮
//   │  welcome back!                   Tips for getting started   │
//   │  model: <model>                  > type / for slash commands │
//   │  account: <email>                > type ? for shortcuts      │
//   │  cwd: ~/path/to/project          > ask about the codebase    │
//   │                                  What's new                  │
//   │                                  * v0.1.0 - first bullet     │
//   │                                  * v0.0.9 - first bullet     │
//   │                                  * v0.0.8 - first bullet     │
//   ╰────────────────────────────────────────────────────────────╯
//
// After the user submits the first prompt, the banner collapses to a single
// muted line:
//   welcome back!
//
// Non-modal contract:
//   - InputBar is always active and focusable while the banner is shown.
//   - SplashBanner does NOT intercept keyboard events.
//   - SplashBanner::Focusable() returns false.
//
// Blueprint contract (TUI-FLOW-T4 / TUI-FLOW-T10)
//   class   batbox::tui::SplashBanner    (this file)
//   method  batbox::tui::SplashBanner::OnRender
//   method  batbox::tui::SplashBanner::collapse
//   method  batbox::tui::SplashBanner::set_model
//   method  batbox::tui::SplashBanner::set_email
//   method  batbox::tui::SplashBanner::set_cwd
//   method  batbox::tui::SplashBanner::set_changelog
// ---------------------------------------------------------------------------
#pragma once

#include <batbox/theme/Theme.hpp>
#include <batbox/tui/Changelog.hpp>
#include <batbox/tui/ThemeApply.hpp>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <functional>
#include <string>
#include <vector>

namespace batbox::tui {

// ---------------------------------------------------------------------------
// Splash — legacy full-screen boot splash (bat ASCII art).
// Preserved for existing tests and the BATBOX_NO_SPLASH path.
// ---------------------------------------------------------------------------

/// Boot splash Component: bat ASCII art + version + daily tagline.
/// Construct via Make().  Prefer SplashBanner for production use.
class Splash : public ftxui::ComponentBase {
public:
    [[nodiscard]]
    static ftxui::Component Make(
        std::function<void()> on_done,
        const batbox::theme::Theme& theme,
        std::string version = "v0.1.0");

    [[nodiscard]]
    static bool should_skip();

    ftxui::Element OnRender() override;
    bool OnEvent(ftxui::Event event) override;

    ~Splash() override;

    Splash(std::function<void()> on_done,
           const batbox::theme::Theme& theme,
           std::string version);

    void start_timer();
    void dismiss();

    std::function<void()> on_done_;
    const batbox::theme::Theme& theme_;
    std::string version_;
    bool done_{false};

    struct TimerImpl;
    std::unique_ptr<TimerImpl> timer_;

    static constexpr const char* kTimerEventName = "batbox.splash-timer";
};

// ---------------------------------------------------------------------------
// SplashBanner — Claude-Code-style non-modal banner (TUI-FLOW-T4).
// ---------------------------------------------------------------------------

/// Non-modal splash banner: titled bordered box with welcome panel (left)
/// and tips + what's-new panel (right).
///
/// Collapses to a single "welcome back!" line after collapse() is called.
/// Never steals focus from InputBar.
///
/// Factory: use SplashBanner::Make() to get an ftxui::Component.
class SplashBanner : public ftxui::ComponentBase {
public:
    /// Create a SplashBanner component.
    ///
    /// @param theme    Active colour palette; must outlive this component.
    /// @param version  Version string shown in the title bar, e.g. "v0.1.0".
    /// @returns  An ftxui::Component wrapping this SplashBanner instance.
    [[nodiscard]]
    static ftxui::Component Make(
        const batbox::theme::Theme& theme,
        std::string version = "v0.1.0");

    /// Constructor — use Make() instead.
    SplashBanner(const batbox::theme::Theme& theme, std::string version);

    ~SplashBanner() override = default;

    // ---- Configuration (call before first render) ---------------------------

    /// Set the model name shown in the welcome panel.  e.g. "magistral-small".
    void set_model(std::string model);

    /// Set the account email shown in the welcome panel.
    void set_email(std::string email);

    /// Set the working directory path; $HOME is replaced with '~'.
    void set_cwd(std::string cwd);

    /// Set the changelog entries to show in the "What's new" right panel.
    ///
    /// Replaces the hardcoded kChangelog array.  Pass the result of
    /// load_changelog(); if the vector is empty the fallback to kChangelog
    /// is applied in render_full().
    ///
    /// Only the first 3 entries (index 0, 1, 2 — newest-first) are shown.
    void set_changelog(std::vector<ChangelogEntry> entries);

    // ---- State transitions --------------------------------------------------

    /// Collapse the banner to a single welcome line.
    /// Called by WireTui after the first user message is submitted.
    void collapse();

    /// True after collapse() has been called.
    [[nodiscard]] bool is_collapsed() const noexcept { return collapsed_; }

    // ---- FTXUI interface ----------------------------------------------------

    ftxui::Element OnRender() override;

    /// SplashBanner never takes focus — InputBar must always be reachable.
    bool Focusable() const override { return false; }

private:
    ftxui::Element render_full() const;
    ftxui::Element render_collapsed() const;

    const batbox::theme::Theme& theme_;
    std::string version_;
    std::string model_;
    std::string email_;
    std::string cwd_;
    std::vector<ChangelogEntry> changelog_entries_; ///< Set via set_changelog(); empty = use kChangelog fallback.
    bool collapsed_{false};
};

} // namespace batbox::tui
