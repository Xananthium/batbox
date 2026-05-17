// tests/unit/test_screen_manager.cpp
//
// doctest suite for batbox::tui::ScreenManager (CPP 1.6).
//
// Build (standalone, no CMake):
//   c++ -std=c++20                                                    \
//       -I/path/to/project/include                                    \
//       -I/path/to/project/build/vcpkg_installed/arm64-osx/include   \
//       tests/unit/test_screen_manager.cpp                           \
//       src/tui/Screen.cpp src/tui/Events.cpp                        \
//       -L/path/to/project/build/vcpkg_installed/arm64-osx/lib       \
//       -lftxui-component -lftxui-dom -lftxui-screen                 \
//       -o /tmp/test_screen_manager && /tmp/test_screen_manager
//
// Or via CMake (add to tests/CMakeLists.txt):
//   batbox_add_unit_test(test_screen_manager
//       unit/test_screen_manager.cpp  batbox_tui)
//
// NOTE on test strategy
// ---------------------
// ScreenInteractive::Loop() takes over the terminal (raw mode, alternate
// screen buffer, signal handlers).  Running it in a unit test would capture
// the test runner's own stdin/stdout and block indefinitely.  The tests below
// therefore exercise all code paths that do NOT enter the loop:
//
//   • Construction succeeds (ScreenInteractive is created, not started).
//   • swap_root() stores and swaps components without running the loop.
//   • post_event() and post_token() can be called before the loop starts
//     (FTXUI's task channel accepts posts before Loop() is entered; events
//     are drained when the loop later starts, or discarded on Exit).
//   • quit_closure() returns a non-null callable.
//   • stop() before run() is a safe no-op (calls Exit() on an idle screen).
//   • post_event() from multiple threads simultaneously is race-free.
//
// This validates the public interface contract and ensures the translation
// unit links cleanly under AddressSanitizer + UBSanitizer.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "batbox/tui/Screen.hpp"
#include "batbox/tui/Events.hpp"

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>

using namespace batbox::tui;

// =============================================================================
// Helpers
// =============================================================================

/// Create a minimal no-op FTXUI Component suitable for swap_root().
static ftxui::Component make_noop_component() {
    return ftxui::Renderer([] {
        return ftxui::text("noop");
    });
}

// =============================================================================
// Construction
// =============================================================================
TEST_SUITE("ScreenManager — construction") {

    TEST_CASE("default construction succeeds without entering the loop") {
        // If construction throws the test simply fails with an exception.
        ScreenManager sm;
        CHECK(true); // reached — no throw
    }

    TEST_CASE("multiple independent ScreenManager instances can be constructed") {
        // FTXUI allows constructing multiple ScreenInteractive objects; only
        // one should be active (Loop running) at a time.  We just ensure
        // construction doesn't segfault or access globals unsafely.
        ScreenManager a;
        ScreenManager b;
        CHECK(true);
    }
}

// =============================================================================
// swap_root
// =============================================================================
TEST_SUITE("ScreenManager — swap_root") {

    TEST_CASE("swap_root before run() stores the component silently") {
        ScreenManager sm;
        auto comp = make_noop_component();
        // Must not throw; no loop is running so no Post() is issued.
        sm.swap_root(comp);
        CHECK(true);
    }

    TEST_CASE("swap_root can be called multiple times before run()") {
        ScreenManager sm;
        for (int i = 0; i < 5; ++i) {
            sm.swap_root(make_noop_component());
        }
        CHECK(true);
    }

    TEST_CASE("swap_root accepts a Renderer component") {
        ScreenManager sm;
        auto renderer = ftxui::Renderer([] {
            return ftxui::hbox({
                ftxui::text("hello "),
                ftxui::text("world"),
            });
        });
        sm.swap_root(renderer);
        CHECK(true);
    }
}

// =============================================================================
// post_event (pre-loop)
// =============================================================================
TEST_SUITE("ScreenManager — post_event pre-loop") {

    TEST_CASE("post_event before run() does not throw") {
        ScreenManager sm;
        sm.post_event(ftxui::Event::Character('x'));
        CHECK(true);
    }

    TEST_CASE("post_event with a custom batbox event does not throw") {
        ScreenManager sm;
        auto ev = make_token_event("fragment", "agent-0");
        sm.post_event(ev);
        CHECK(true);
    }

    TEST_CASE("post_token before run() does not throw") {
        ScreenManager sm;
        sm.post_token("hello world");
        CHECK(true);
    }

    TEST_CASE("post_token with empty string does not throw") {
        ScreenManager sm;
        sm.post_token("");
        CHECK(true);
    }

    TEST_CASE("post_event with AgentsDirty event does not throw") {
        ScreenManager sm;
        auto ev = make_agents_dirty_event("agent-1", 5, 200, "running");
        sm.post_event(ev);
        CHECK(true);
    }

    TEST_CASE("post_event with StatusUpdate event does not throw") {
        ScreenManager sm;
        auto ev = make_status_update_event(SidecarState::Running, "PID 42");
        sm.post_event(ev);
        CHECK(true);
    }
}

// =============================================================================
// post_event thread-safety (no loop — compile + run safe)
// =============================================================================
TEST_SUITE("ScreenManager — post_event thread-safety") {

    TEST_CASE("concurrent post_event from N threads is race-free") {
        // This test verifies that PostEvent's internal task-channel lock is
        // sufficient to prevent data races when many background threads fire
        // events simultaneously.
        //
        // We do NOT start the loop — events accumulate in the internal queue.
        // The queue is allocated and protected by FTXUI, so as long as we
        // don't call Loop(), no UB occurs: the Sender side just enqueues tasks
        // via a mutex-protected channel.
        //
        // Run under ThreadSanitizer this would catch any race on the
        // ScreenInteractive internals.

        ScreenManager sm;

        constexpr int kNumThreads = 8;
        constexpr int kEventsEach = 50;

        std::atomic<int> fired{0};
        std::vector<std::thread> threads;
        threads.reserve(kNumThreads);

        for (int t = 0; t < kNumThreads; ++t) {
            threads.emplace_back([&sm, &fired, t]() {
                for (int i = 0; i < kEventsEach; ++i) {
                    sm.post_token("tok-" + std::to_string(t) + "-" +
                                  std::to_string(i));
                    ++fired;
                }
            });
        }

        for (auto& th : threads) {
            th.join();
        }

        CHECK(fired.load() == kNumThreads * kEventsEach);
    }

    TEST_CASE("swap_root and post_event concurrent before run() is race-free") {
        // Stress test: one thread spams swap_root, another spams post_event.
        // The root_mtx_ in ScreenManager should keep swap_root safe; PostEvent
        // is protected by FTXUI internals.

        ScreenManager sm;
        sm.swap_root(make_noop_component());

        std::atomic<bool> done{false};
        std::atomic<int> swaps{0};
        std::atomic<int> posts{0};

        std::thread swapper([&]() {
            for (int i = 0; i < 100; ++i) {
                sm.swap_root(make_noop_component());
                ++swaps;
            }
        });

        std::thread poster([&]() {
            for (int i = 0; i < 100; ++i) {
                sm.post_event(make_token_event("x"));
                ++posts;
            }
        });

        swapper.join();
        poster.join();

        CHECK(swaps.load() == 100);
        CHECK(posts.load() == 100);
    }
}

// =============================================================================
// stop() before run()
// =============================================================================
TEST_SUITE("ScreenManager — stop before run") {

    TEST_CASE("stop() before run() is a safe no-op") {
        // Exit() on an idle ScreenInteractive sets the quit flag but does not
        // block or throw.  If Loop() is then called it will return immediately.
        ScreenManager sm;
        sm.stop(); // must not throw or deadlock
        CHECK(true);
    }

    TEST_CASE("stop() twice before run() is safe") {
        ScreenManager sm;
        sm.stop();
        sm.stop();
        CHECK(true);
    }
}

// =============================================================================
// quit_closure
// =============================================================================
TEST_SUITE("ScreenManager — quit_closure") {

    TEST_CASE("quit_closure() returns a non-null callable") {
        ScreenManager sm;
        auto closure = sm.quit_closure();
        CHECK(static_cast<bool>(closure));
    }

    TEST_CASE("quit_closure() can be invoked before run() without crashing") {
        // Invoking ExitLoopClosure before the loop starts triggers Exit() on
        // an idle screen — safe, same as stop().
        ScreenManager sm;
        auto closure = sm.quit_closure();
        closure(); // must not throw
        CHECK(true);
    }

    TEST_CASE("quit_closure() can be stored and invoked later") {
        ScreenManager sm;
        ftxui::Closure stored;
        {
            stored = sm.quit_closure();
        }
        // sm is still alive — invoke the stored closure
        stored();
        CHECK(true);
    }
}

// =============================================================================
// Destructor safety
// =============================================================================
TEST_SUITE("ScreenManager — destructor") {

    TEST_CASE("destructor of unused ScreenManager is safe") {
        {
            ScreenManager sm;
            // Let it go out of scope without calling run().
        }
        CHECK(true);
    }

    TEST_CASE("destructor after swap_root and post_event is safe") {
        {
            ScreenManager sm;
            sm.swap_root(make_noop_component());
            sm.post_event(make_token_event("bye"));
        }
        CHECK(true);
    }
}
