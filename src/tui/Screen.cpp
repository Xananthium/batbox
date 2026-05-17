// src/tui/Screen.cpp
//
// ScreenManager implementation.
//
// See include/batbox/tui/Screen.hpp for design notes, threading contract,
// and usage patterns.

#include "batbox/tui/Screen.hpp"
#include "batbox/tui/Events.hpp"

#include <cassert>
#include <stdexcept>
#include <string>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

namespace batbox::tui {

// =============================================================================
// Construction / destruction
// =============================================================================

ScreenManager::ScreenManager()
    : screen_(ftxui::ScreenInteractive::Fullscreen())
    , root_(nullptr)
{
    // The FTXUI ScreenInteractive is created but the loop is not yet running.
    // Callers must invoke swap_root() before run().
}

ScreenManager::~ScreenManager() {
    // If someone forgot to call stop() (e.g. the app crashed a worker thread
    // but the main thread is still in run()), attempt a clean shutdown.
    // This is defensive — normal teardown goes through stop().
    if (running_.load(std::memory_order_acquire)) {
        stop();
    }
}

// =============================================================================
// Lifecycle
// =============================================================================

void ScreenManager::run() {
    // Guard against double-run.
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true,
                                          std::memory_order_acq_rel)) {
        throw std::logic_error(
            "batbox::tui::ScreenManager::run() called while already running");
    }

    // Snapshot the current root under the lock.  We do NOT hold the lock
    // while calling Loop() — that would deadlock swap_root().
    ftxui::Component initial_root;
    {
        std::lock_guard<std::mutex> lk(root_mtx_);
        initial_root = root_;
    }

    if (!initial_root) {
        running_.store(false, std::memory_order_release);
        throw std::logic_error(
            "batbox::tui::ScreenManager::run() called without a root component "
            "(call swap_root() first)");
    }

    // -------------------------------------------------------------------------
    // FTXUI event loop.
    //
    // ScreenInteractive::Loop() installs terminal raw-mode, spawns internal
    // event-listener and animation threads, then drives:
    //
    //   while (!quit_) {
    //     RunOnceBlocking(component);   // waits for an event or task
    //   }
    //
    // It returns only when Exit() is called (which sets quit_).
    //
    // We pass a Renderer wrapper that always renders through the *current*
    // root_ pointer (atomically read under root_mtx_) rather than the
    // snapshot captured above.  This makes swap_root() seamless: the next
    // render cycle after swap_root() is called will use the new component.
    // -------------------------------------------------------------------------

    auto dynamic_renderer = ftxui::Renderer([this]() -> ftxui::Element {
        ftxui::Component current;
        {
            std::lock_guard<std::mutex> lk(root_mtx_);
            current = root_;
        }
        if (!current) {
            return ftxui::text("");
        }
        return current->Render();
    });

    // Wrap the dynamic renderer in a CatchEvent component so that input events
    // are forwarded to the actual active root component.
    auto dispatch_events = ftxui::CatchEvent(
        dynamic_renderer,
        [this](ftxui::Event ev) -> bool {
            ftxui::Component current;
            {
                std::lock_guard<std::mutex> lk(root_mtx_);
                current = root_;
            }
            if (!current) {
                return false;
            }
            return current->OnEvent(ev);
        });

    screen_.Loop(dispatch_events);

    running_.store(false, std::memory_order_release);
}

void ScreenManager::stop() {
    // ScreenInteractive::Exit() is thread-safe: it Posts a Quit task into the
    // internal task queue, which the loop processes on the UI thread.
    screen_.Exit();
}

// =============================================================================
// Screen transitions
// =============================================================================

void ScreenManager::swap_root(ftxui::Component component) {
    assert(component != nullptr && "swap_root: component must not be nullptr");

    {
        std::lock_guard<std::mutex> lk(root_mtx_);
        root_ = std::move(component);
    }

    // If the loop is already running, poke it so it re-renders with the new
    // root on the next iteration.  Post() is thread-safe.
    if (running_.load(std::memory_order_acquire)) {
        screen_.Post([]() { /* no-op task — just triggers a redraw cycle */ });
    }
}

// =============================================================================
// Thread-safe event posting
// =============================================================================

void ScreenManager::post_event(ftxui::Event ev) {
    // ScreenInteractive::PostEvent() is documented as thread-safe.  It enqueues
    // the event via the internal Sender<Task> channel and wakes the loop.
    screen_.PostEvent(std::move(ev));
}

void ScreenManager::post_token(std::string_view text) {
    post_event(make_token_event(std::string(text)));
}

// =============================================================================
// Helpers
// =============================================================================

ftxui::Closure ScreenManager::quit_closure() {
    // Capture by value so the closure is self-contained and safe to copy.
    // We call stop() via a local lambda; ExitLoopClosure() from FTXUI would also
    // work but does not go through our stop() (which guards against double-Exit).
    return screen_.ExitLoopClosure();
}

// =============================================================================
// screen_interactive() — expose the underlying ScreenInteractive by reference
// =============================================================================

ftxui::ScreenInteractive& ScreenManager::screen_interactive() noexcept {
    // The ScreenInteractive is owned by ScreenManager for its full lifetime.
    // Callers (e.g. DemonPanel ticker) use this for thread-safe PostEvent calls.
    return screen_;
}

} // namespace batbox::tui
