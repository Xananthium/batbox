// include/batbox/tui/DemonPanel.hpp
// ---------------------------------------------------------------------------
// batbox::tui::DemonPanel — Party Monster easter-egg floating panel.
//
// Design
// ------
// DemonPanel is an FTXUI ComponentBase that renders a hot-pink 12×6 floating
// panel in the bottom-right corner of the terminal.  It is the display surface
// for the /demon agent (CPP 6.8), which provides rate-limited commentary on
// the user's conversation.
//
// The panel is hidden by default and becomes visible when the /demon agent is
// activated via the /demon slash command.  It disappears on /demon dismiss.
//
// Layout (24 cols wide × 6 rows):
//
//   ╔══════════════════════╗
//   ║ 👹 PARTY MONSTER     ║
//   ║─────────────────────║
//   ║ <demon comment>      ║
//   ║ <demon comment>      ║
//   ║─────────────────────║
//   ║ <tagline>         ◆ ║
//   ╚══════════════════════╝
//
// The hot-pink palette (#ff007f border + text, near-black bg) is HARDCODED and
// intentionally ignores the user's active theme.  That's the joke: the demon
// always renders in party-monster magenta regardless of aesthetic preference.
//
// Taglines
// --------
// kDemonTaglines[] (defined in DemonPanel.cpp) is a constexpr array of Miss
// Kittin / electroclash / party-monster flavoured strings.  The active tagline
// rotates every 5 seconds using wall-clock time.  When the /demon agent pushes
// a new comment via set_demon_comment(), the comment text overlays the tagline
// area.
//
// 5Hz ticker
// ----------
// TuiDemonTickerThread (inner class) holds a std::jthread that sleeps 200ms
// per iteration (5Hz max).  On each wake it checks whether the tagline index
// has changed (time-based) or whether a new comment is pending.  If either is
// true it posts make_demon_dirty_event("") to the FTXUI screen so OnEvent
// fires on the UI thread and triggers a redraw.
//
// Threading contract
// ------------------
//   Thread-safe (callable from any thread):
//     set_demon_comment(text), show(), hide(), toggle(), visible()
//
//   UI thread only (FTXUI rule):
//     OnRender(), OnEvent()
//
// Blueprint contract (blueprints table, task CPP 1.14)
// -----------------------------------------------------
//   class  batbox::tui::DemonPanel : public ftxui::ComponentBase
//   file   tui/DemonPanel.hpp
// ---------------------------------------------------------------------------
#pragma once

#include <batbox/theme/Theme.hpp>
#include <batbox/tui/ThemeApply.hpp>
#include <batbox/tui/Events.hpp>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace batbox::tui {

// =============================================================================
// DemonPanel
// =============================================================================

/// Party Monster floating panel — /demon easter-egg agent display surface.
///
/// Construct via the static Make() factory which returns an ftxui::Component
/// (shared_ptr<ComponentBase>) suitable for composition into the chat layout.
///
/// The panel is initially hidden.  Call show() after /demon is invoked and
/// hide() (or toggle()) after /demon dismiss.
class DemonPanel : public ftxui::ComponentBase {
public:
    // -------------------------------------------------------------------------
    // Factory
    // -------------------------------------------------------------------------

    /// Create a DemonPanel component.
    ///
    /// @param theme   Active theme reference (held by ref; must outlive panel).
    ///                Note: DemonPanel ignores most theme colors and hard-codes
    ///                its hot-pink palette.  The ref is kept for future compat.
    /// @param screen  The owning ScreenInteractive; the ticker thread calls
    ///                screen.PostEvent() from a background thread to trigger
    ///                redraws.
    ///
    /// @returns  An ftxui::Component wrapping this DemonPanel instance.
    [[nodiscard]]
    static ftxui::Component Make(
        const batbox::theme::Theme&    theme,
        ftxui::ScreenInteractive&      screen);

    // -------------------------------------------------------------------------
    // Destructor — stops the ticker thread.
    // -------------------------------------------------------------------------
    ~DemonPanel() override;

    // Non-copyable, non-movable (ComponentBase semantics + jthread ownership).
    DemonPanel(const DemonPanel&)            = delete;
    DemonPanel& operator=(const DemonPanel&) = delete;
    DemonPanel(DemonPanel&&)                 = delete;
    DemonPanel& operator=(DemonPanel&&)      = delete;

    // -------------------------------------------------------------------------
    // ComponentBase overrides (UI thread)
    // -------------------------------------------------------------------------

    /// Render the panel.
    ///
    /// Returns ftxui::emptyElement() when hidden so the layout is unaffected.
    /// When visible, returns a hot-pink 24×6 bordered box positioned at the
    /// bottom-right using hbox({filler(), panel}).
    ftxui::Element OnRender() override;

    /// Handle Events::DemonDirty posted by the ticker thread.
    ///
    /// On receipt, updates cached_tagline_idx_ from the time-based computation
    /// and copies the latest comment under the comment mutex.  FTXUI will
    /// re-render after OnEvent returns true.
    bool OnEvent(ftxui::Event event) override;

    // -------------------------------------------------------------------------
    // Visibility controls (thread-safe)
    // -------------------------------------------------------------------------

    /// Make the panel visible.  Idempotent.
    void show();

    /// Hide the panel.  Idempotent.
    void hide();

    /// Toggle visibility.
    void toggle();

    /// Query current visibility state.
    [[nodiscard]] bool visible() const;

    // -------------------------------------------------------------------------
    // Demon comment (thread-safe — called by /demon agent on its own thread)
    // -------------------------------------------------------------------------

    /// Push the latest monologue line from the /demon agent.
    ///
    /// The text is stored under a mutex and surfaced on the next DemonDirty
    /// render cycle.  Pass an empty string to clear the comment area.
    void set_demon_comment(std::string text);

    /// Retrieve the latest demon comment.
    [[nodiscard]] std::string get_demon_comment() const;

    // -------------------------------------------------------------------------
    // Tagline inspection (for tests)
    // -------------------------------------------------------------------------

    /// Return the currently displayed tagline index (0-based into kDemonTaglines).
    ///
    /// Computed from wall-clock time: changes every 5 seconds.
    [[nodiscard]] std::size_t current_tagline_idx() const;

    // -------------------------------------------------------------------------
    // Constructor (public for std::make_shared; prefer Make())
    // -------------------------------------------------------------------------
    DemonPanel(const batbox::theme::Theme& theme,
               ftxui::ScreenInteractive&   screen);

private:
    // -------------------------------------------------------------------------
    // Inner class: 5Hz ticker thread
    // -------------------------------------------------------------------------

    /// Background thread that posts DemonDirty events at up to 5Hz.
    ///
    /// Wakes every 200ms (5Hz max), checks if the tagline index changed or if
    /// a new comment has arrived since the last event.  Posts
    /// make_demon_dirty_event("") only when there is actually something new to
    /// render (change-driven, not time-driven).
    class TuiDemonTickerThread {
    public:
        /// Construct and immediately launch the ticker.
        ///
        /// @param screen      FTXUI ScreenInteractive for PostEvent().
        /// @param panel       Raw pointer to parent panel (valid for ticker lifetime).
        explicit TuiDemonTickerThread(ftxui::ScreenInteractive& screen,
                                      DemonPanel*               panel);

        /// Destructor — requests stop on the jthread (automatic via jthread dtor).
        ~TuiDemonTickerThread();

        // Non-copyable, non-movable.
        TuiDemonTickerThread(const TuiDemonTickerThread&)            = delete;
        TuiDemonTickerThread& operator=(const TuiDemonTickerThread&) = delete;
        TuiDemonTickerThread(TuiDemonTickerThread&&)                 = delete;
        TuiDemonTickerThread& operator=(TuiDemonTickerThread&&)      = delete;

    private:
        void run(std::stop_token stop_tok);

        ftxui::ScreenInteractive& screen_;
        DemonPanel*               panel_;
        std::jthread              thread_;
    };

    // -------------------------------------------------------------------------
    // Internal helpers
    // -------------------------------------------------------------------------

    /// Compute the tagline index for the current wall-clock time.
    ///
    /// Changes every 5 seconds.  Uses steady_clock so it is monotonic and
    /// cannot be confused by daylight-saving transitions.
    static std::size_t compute_tagline_idx();

    // -------------------------------------------------------------------------
    // Data members
    // -------------------------------------------------------------------------

    const batbox::theme::Theme& theme_;      ///< Live theme ref (mostly ignored)
    ftxui::ScreenInteractive&   screen_;     ///< For ticker thread PostEvent

    std::atomic<bool>           visible_{false}; ///< Visibility flag (thread-safe)

    mutable std::mutex          comment_mtx_;
    std::string                 latest_comment_;   ///< Protected by comment_mtx_
    std::string                 comment_dirty_ver_; ///< Snapshot at last dirty

    // Cached on UI thread in OnEvent to avoid lock in OnRender.
    std::string                 cached_comment_;
    std::size_t                 cached_tagline_idx_{0};

    std::unique_ptr<TuiDemonTickerThread> ticker_;
};

} // namespace batbox::tui
