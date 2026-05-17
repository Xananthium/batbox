// include/batbox/tui/Screen.hpp
//
// ScreenManager — owns the FTXUI ScreenInteractive and drives the TUI lifecycle.
//
// Design
// ------
// ScreenManager is the single owner of a `ftxui::ScreenInteractive` (Fullscreen,
// alternate-buffer).  It exposes a clean interface for the rest of batbox:
//
//   • run()            — Blocks the calling thread (must be the main/UI thread).
//                        Enters the FTXUI event loop and returns only after
//                        stop() is called or the user presses Ctrl+D / /exit.
//
//   • stop()           — May be called from ANY thread.  Posts an Exit task to
//                        the FTXUI queue; the loop drains remaining events and
//                        returns from run().
//
//   • swap_root(comp)  — Replace the live root Component with `comp`.  Used for
//                        screen transitions: splash → chat → modal overlays.
//                        MUST be called from the main (UI) thread.
//
//   • post_event(ev)   — Thread-safe.  Forwards the event to
//                        ScreenInteractive::PostEvent, which wakes the FTXUI
//                        loop and dispatches the event on the UI thread.
//
//   • post_token(text) — Convenience wrapper.  Creates a
//                        `batbox::tui::make_token_event(text)` and forwards it
//                        via post_event.  Safe to call from any thread (SSE
//                        reader, sub-agent supervisor, etc.).
//
//   • quit_closure()   — Returns an `ftxui::Closure` that, when invoked,
//                        triggers a clean shutdown.  Useful for wiring a
//                        "Quit" button or /exit command without exposing
//                        ScreenManager to the component.
//
// Threading contract
// ------------------
// The FTXUI event loop runs entirely on the thread that called `run()`.
// This is the UI thread.  All Component::Render() and Component::OnEvent()
// callbacks execute on the UI thread.
//
//   Thread-safe (may be called from any thread):
//     post_event(ev), post_token(text), stop()
//
//   UI-thread only:
//     run(), swap_root(comp), quit_closure()
//     (swap_root post an update internally through Post(), so technically
//      it is safe from other threads too — see implementation notes.)
//
// Splash → chat transition pattern
// ---------------------------------
//   1. Construct ScreenManager.
//   2. Create Splash component; call swap_root(splash).
//   3. Call run() on the main thread (blocks).
//   4. On a background thread, after init completes: call swap_root(chat_root).
//      swap_root internally Posts a no-op tick so FTXUI re-renders with the
//      new root on the next loop iteration.
//   5. When the user quits: stop() → run() returns.
//
// Modal overlay pattern
// ---------------------
// Modal overlays are NOT managed via swap_root.  Instead, the root Component
// returned by the chat view already includes FTXUI's `Modal(base, overlay, &flag)`
// decorator; ScreenManager just drives the loop.  Components post Events::ModalShow
// / ModalHide to toggle the overlay flag.

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string_view>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>

namespace batbox::tui {

/// Central TUI lifecycle manager.  Owns the FTXUI ScreenInteractive.
///
/// Lifetime: typically one instance per process, constructed in App::run()
/// before any background threads are launched.  Must remain alive until run()
/// returns.
class ScreenManager {
public:
    // -------------------------------------------------------------------------
    // Construction / destruction
    // -------------------------------------------------------------------------

    /// Construct a ScreenManager with no root component yet.
    /// Call swap_root() before run().
    ScreenManager();

    /// Destructor.  Calls stop() if the loop is still running (defensive).
    ~ScreenManager();

    // Non-copyable, non-movable (owns a ScreenInteractive by value).
    ScreenManager(const ScreenManager&)            = delete;
    ScreenManager& operator=(const ScreenManager&) = delete;
    ScreenManager(ScreenManager&&)                 = delete;
    ScreenManager& operator=(ScreenManager&&)      = delete;

    // -------------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------------

    /// Start the FTXUI event loop.
    ///
    /// Blocks the calling thread until stop() is called, the user presses
    /// Ctrl+D, or an /exit command posts a Quit event.
    ///
    /// Precondition: swap_root() has been called with a valid Component.
    /// Precondition: run() has not already been called.
    /// Thread: MUST be called on the UI thread (typically `main`).
    void run();

    /// Signal the event loop to quit gracefully.
    ///
    /// Safe to call from any thread.  Returns immediately; the loop may process
    /// a few more pending events before run() returns.
    void stop();

    // -------------------------------------------------------------------------
    // Screen transitions
    // -------------------------------------------------------------------------

    /// Replace the currently rendered root Component.
    ///
    /// Causes FTXUI to re-render with the new root on the next loop iteration.
    /// Internally this swaps the shared_ptr under a mutex and Posts a refresh
    /// task so the change takes effect promptly.
    ///
    /// Calling this before run() sets the initial root (no Post is needed then).
    /// May be called from any thread — the actual render swap is serialised
    /// through the FTXUI task queue.
    ///
    /// @param component  The new root Component.  Must not be nullptr.
    void swap_root(ftxui::Component component);

    // -------------------------------------------------------------------------
    // Thread-safe event posting
    // -------------------------------------------------------------------------

    /// Post an FTXUI event from any thread.
    ///
    /// Wakes the event loop so Component::OnEvent() is called on the UI thread.
    /// Internally delegates to ScreenInteractive::PostEvent().
    ///
    /// @param ev  Any ftxui::Event, including custom batbox events created by
    ///            the factory functions in Events.hpp.
    void post_event(ftxui::Event ev);

    /// Convenience: post a streaming token event from any thread.
    ///
    /// Creates a `batbox::tui::make_token_event(text)` and calls post_event().
    /// Called by the SSE reader on each assistant text fragment.
    ///
    /// @param text  A UTF-8 text fragment.  May be empty (triggers a redraw
    ///              without appending text, useful for flushing the renderer).
    void post_token(std::string_view text);

    // -------------------------------------------------------------------------
    // Helpers
    // -------------------------------------------------------------------------

    /// Return an ftxui::Closure that, when invoked, calls stop().
    ///
    /// Useful for wiring an /exit command or a Quit button without giving the
    /// component a pointer to the ScreenManager.
    ///
    /// The returned closure captures a weak reference; calling it after the
    /// ScreenManager is destroyed is safe (no-op).
    ///
    /// Thread: returned closure may be invoked from the UI thread only
    /// (FTXUI closures run on the UI thread via Post).
    [[nodiscard]] ftxui::Closure quit_closure();

    /// Return a reference to the underlying FTXUI ScreenInteractive.
    ///
    /// Required by components that need to call PostEvent() directly
    /// (e.g. DemonPanel's 5Hz ticker thread).  The reference is valid
    /// for the lifetime of the ScreenManager.
    ///
    /// Thread: the returned reference may be used from any thread for
    /// PostEvent() calls (ScreenInteractive::PostEvent is thread-safe).
    [[nodiscard]] ftxui::ScreenInteractive& screen_interactive() noexcept;

private:
    // The FTXUI ScreenInteractive — fullscreen, alternate screen buffer.
    ftxui::ScreenInteractive screen_;

    // The currently active root Component.  Protected by root_mtx_ only for
    // the swap_root path (the Loop itself reads it under FTXUI's internal lock).
    ftxui::Component root_;
    std::mutex       root_mtx_;

    // True once run() has been entered (guards against double-run).
    std::atomic<bool> running_{false};
};

} // namespace batbox::tui
