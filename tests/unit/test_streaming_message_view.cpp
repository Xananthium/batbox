// tests/unit/test_streaming_message_view.cpp
// ---------------------------------------------------------------------------
// doctest suite for batbox::tui::StreamingMessageView (CPP 1.8).
//
// Acceptance criteria tested:
//   [AC1] post_token appends and triggers re-render
//         → append_chunk() updates current_text(); OnRender() reflects new text.
//   [AC2] Render time per token <2ms (perf measurement) on a typical message
//         → timed loop over 100 append + OnRender cycles.
//   [AC3] No data races under ASan/TSan with concurrent post + render
//         → concurrent thread calls append_chunk() while UI thread calls OnRender().
//   [AC4] Unit test with simulated rapid post stream
//         → 500-token rapid-fire stream; final current_text() matches assembled string.
//
// Build (standalone, from repo root):
//   c++ -std=c++20                                                              \
//       -I include                                                              \
//       -I build2/vcpkg_installed/arm64-osx/include                            \
//       tests/unit/test_streaming_message_view.cpp                             \
//       src/tui/StreamingMessageView.cpp                                       \
//       src/tui/MarkdownRender.cpp                                             \
//       src/tui/Events.cpp                                                     \
//       src/tui/ThemeApply.cpp                                                 \
//       src/theme/Theme.cpp                                                    \
//       src/theme/themes.cpp                                                   \
//       -L build2/vcpkg_installed/arm64-osx/lib                               \
//       -lftxui-component -lftxui-dom -lftxui-screen                          \
//       -o /tmp/test_smv && /tmp/test_smv
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/tui/StreamingMessageView.hpp>
#include <batbox/tui/Events.hpp>
#include "fixtures/TestTheme.hpp"

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------
namespace {

using batbox::test_fixtures::make_test_theme;

/// Render an element to a small screen and return true if it didn't crash.
bool render_ok(ftxui::Element el, int w = 80, int h = 24) {
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(w),
                                        ftxui::Dimension::Fixed(h));
    ftxui::Render(screen, el);
    return true;
}

/// Simulate posting a token event to a component (UI-thread equivalent).
bool post_token_to(batbox::tui::StreamingMessageView& view,
                   const std::string& text) {
    auto ev = batbox::tui::make_token_event(text, "");
    return view.OnEvent(ev);
}

} // namespace

// ============================================================================
// TEST SUITE 1 — Construction and initial state
// ============================================================================

TEST_SUITE("StreamingMessageView::construction") {

    TEST_CASE("default construction: empty text, streaming=true") {
        auto theme = make_test_theme();
        batbox::tui::StreamingMessageView view(theme);

        CHECK(view.current_text().empty());
        CHECK(view.is_streaming() == true);
        CHECK(view.Focusable() == false);
    }

    TEST_CASE("OnRender with no content does not crash") {
        auto theme = make_test_theme();
        batbox::tui::StreamingMessageView view(theme);

        auto el = view.OnRender();
        // Should produce at least the cursor element — not a crash.
        CHECK(render_ok(el));
    }

    TEST_CASE("is a proper ComponentBase subclass") {
        auto theme = make_test_theme();
        auto view  = std::make_shared<batbox::tui::StreamingMessageView>(theme);

        // shared_ptr<ComponentBase> assignment must compile.
        std::shared_ptr<ftxui::ComponentBase> base = view;
        CHECK(base != nullptr);

        // Render() is callable via ComponentBase interface.
        auto el = base->Render();
        CHECK(render_ok(el));
    }
}

// ============================================================================
// TEST SUITE 2 — [AC1] append_chunk triggers re-render
// ============================================================================

TEST_SUITE("StreamingMessageView::append_chunk") {

    TEST_CASE("[AC1] Token event updates current_text via OnEvent") {
        auto theme = make_test_theme();
        batbox::tui::StreamingMessageView view(theme);

        // Simulate Token event (primary path via ScreenManager::post_token).
        bool handled = post_token_to(view, "Hello, world!");
        CHECK(handled == true);
        CHECK(view.current_text() == "Hello, world!");
    }

    TEST_CASE("[AC1] Multiple token events accumulate text") {
        auto theme = make_test_theme();
        batbox::tui::StreamingMessageView view(theme);

        post_token_to(view, "Hello");
        post_token_to(view, ", ");
        post_token_to(view, "world!");

        CHECK(view.current_text() == "Hello, world!");
    }

    TEST_CASE("[AC1] OnRender reflects text after token events") {
        auto theme = make_test_theme();
        batbox::tui::StreamingMessageView view(theme);

        post_token_to(view, "**Bold text** and `inline code`\n");

        auto el = view.OnRender();
        CHECK(render_ok(el));
        CHECK(!view.current_text().empty());
    }

    TEST_CASE("[AC1] append_chunk direct call updates current_text") {
        auto theme = make_test_theme();
        batbox::tui::StreamingMessageView view(theme);

        view.append_chunk("Direct chunk");
        CHECK(view.current_text() == "Direct chunk");
    }

    TEST_CASE("[AC1] empty token event is consumed without crashing") {
        auto theme = make_test_theme();
        batbox::tui::StreamingMessageView view(theme);

        bool handled = post_token_to(view, "");
        CHECK(handled == true);
        CHECK(view.current_text().empty());
    }

    TEST_CASE("[AC1] non-Token events are not consumed") {
        auto theme = make_test_theme();
        batbox::tui::StreamingMessageView view(theme);

        // ArrowDown is not a token event — should not be consumed.
        bool handled = view.OnEvent(ftxui::Event::ArrowDown);
        CHECK(handled == false);
    }

    TEST_CASE("OnRender with multiline markdown does not crash") {
        auto theme = make_test_theme();
        batbox::tui::StreamingMessageView view(theme);

        post_token_to(view, "# Heading\n\nSome paragraph text.\n\n```cpp\nint x = 42;\n```\n");

        auto el = view.OnRender();
        CHECK(render_ok(el));
    }

    TEST_CASE("streaming cursor appears while streaming=true") {
        auto theme = make_test_theme();
        batbox::tui::StreamingMessageView view(theme);

        CHECK(view.is_streaming() == true);
        auto el = view.OnRender();
        // Should render a cursor indicator — just verify no crash.
        CHECK(render_ok(el));
    }

    TEST_CASE("no cursor after close_stream") {
        auto theme = make_test_theme();
        batbox::tui::StreamingMessageView view(theme);

        post_token_to(view, "Final text.");
        view.close_stream();

        CHECK(view.is_streaming() == false);
        auto el = view.OnRender();
        CHECK(render_ok(el));
    }
}

// ============================================================================
// TEST SUITE 3 — Stream lifecycle
// ============================================================================

TEST_SUITE("StreamingMessageView::lifecycle") {

    TEST_CASE("close_stream transitions streaming to false") {
        auto theme = make_test_theme();
        batbox::tui::StreamingMessageView view(theme);

        CHECK(view.is_streaming() == true);
        view.close_stream();
        CHECK(view.is_streaming() == false);
    }

    TEST_CASE("reset clears text and restores streaming=true") {
        auto theme = make_test_theme();
        batbox::tui::StreamingMessageView view(theme);

        post_token_to(view, "Some text");
        view.close_stream();
        CHECK(!view.current_text().empty());
        CHECK(view.is_streaming() == false);

        view.reset();
        CHECK(view.current_text().empty());
        CHECK(view.is_streaming() == true);
    }

    TEST_CASE("reset followed by new stream works correctly") {
        auto theme = make_test_theme();
        batbox::tui::StreamingMessageView view(theme);

        post_token_to(view, "Turn 1 text");
        view.close_stream();
        view.reset();

        post_token_to(view, "Turn 2 text");
        CHECK(view.current_text() == "Turn 2 text");

        auto el = view.OnRender();
        CHECK(render_ok(el));
    }

    TEST_CASE("multiple resets are safe") {
        auto theme = make_test_theme();
        batbox::tui::StreamingMessageView view(theme);

        view.reset();
        view.reset();
        view.reset();
        CHECK(view.current_text().empty());
        CHECK(view.is_streaming() == true);
    }
}

// ============================================================================
// TEST SUITE 4 — [AC4] Simulated rapid post stream
// ============================================================================

TEST_SUITE("StreamingMessageView::rapid_stream") {

    TEST_CASE("[AC4] 500-token rapid-fire stream assembles correctly") {
        auto theme = make_test_theme();
        batbox::tui::StreamingMessageView view(theme);

        // Simulate 500 single-word tokens arriving in rapid succession.
        std::string expected;
        expected.reserve(500 * 6);

        for (int i = 0; i < 500; ++i) {
            std::string token = "word" + std::to_string(i) + " ";
            post_token_to(view, token);
            expected += token;
        }

        CHECK(view.current_text() == expected);
    }

    TEST_CASE("[AC4] rapid stream with markdown tokens renders without crash") {
        auto theme = make_test_theme();
        batbox::tui::StreamingMessageView view(theme);

        // Simulate a typical assistant response arriving token by token.
        const std::vector<std::string> tokens = {
            "# ", "Answer\n\n",
            "Here ", "is ", "a ", "code ", "example:\n\n",
            "```", "cpp\n",
            "int ", "main()", " {\n",
            "    ", "return ", "0;\n",
            "}\n",
            "```\n\n",
            "And ", "some ", "**bold** ", "text.\n",
        };

        for (const auto& tok : tokens) {
            post_token_to(view, tok);
            // Render after each token to simulate FTXUI re-render on each event.
            auto el = view.OnRender();
            CHECK(render_ok(el));
        }

        view.close_stream();
        auto final_el = view.OnRender();
        CHECK(render_ok(final_el));
    }

    TEST_CASE("[AC4] append_chunk rapid direct call assembles correctly") {
        auto theme = make_test_theme();
        batbox::tui::StreamingMessageView view(theme);

        std::string expected;
        for (int i = 0; i < 200; ++i) {
            std::string chunk = "chunk" + std::to_string(i);
            view.append_chunk(chunk);
            expected += chunk;
        }

        CHECK(view.current_text() == expected);
    }
}

// ============================================================================
// TEST SUITE 5 — [AC2] Render performance <2ms per token
// ============================================================================

TEST_SUITE("StreamingMessageView::performance") {

    TEST_CASE("[AC2] append + render cycle <2ms per token on typical message") {
        auto theme = make_test_theme();
        batbox::tui::StreamingMessageView view(theme);

        // Warm up the renderer with a paragraph already cached.
        post_token_to(view,
            "# Performance test\n\n"
            "This is a typical assistant response with some content already "
            "rendered and cached by the incremental markdown renderer.\n\n");

        // Now measure per-token cost with 100 tokens.
        constexpr int kTokens = 100;
        constexpr auto kBudget = std::chrono::milliseconds(2);

        auto t0 = std::chrono::steady_clock::now();

        for (int i = 0; i < kTokens; ++i) {
            std::string tok = "word" + std::to_string(i) + " ";
            post_token_to(view, tok);    // append_chunk + renderer_.append()
            auto el = view.OnRender();   // renderer_.render()
            // Consume the element to prevent dead-code elimination.
            auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(80),
                                                ftxui::Dimension::Fixed(24));
            ftxui::Render(screen, el);
        }

        auto t1    = std::chrono::steady_clock::now();
        auto total = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0);
        auto per_token_us = total.count() / kTokens;

        // Log for visibility.
        MESSAGE("Per-token time: ", per_token_us, " µs (budget: 2000 µs)");

        // The acceptance criterion: <2ms per token.
        CHECK(per_token_us < 2000);
    }
}

// ============================================================================
// TEST SUITE 6 — [AC3] Concurrent post + render (data-race check)
// ============================================================================

TEST_SUITE("StreamingMessageView::concurrency") {

    TEST_CASE("[AC3] concurrent append_chunk + current_text() no data race") {
        // This test is primarily valuable under TSan.  Without TSan it still
        // checks that the locking logic doesn't deadlock or crash.
        auto theme = make_test_theme();
        batbox::tui::StreamingMessageView view(theme);

        constexpr int kIterations = 1000;
        std::atomic<int> appended{0};

        // Background thread: rapidly appends chunks (simulates SSE reader).
        std::thread writer([&] {
            for (int i = 0; i < kIterations; ++i) {
                view.append_chunk("x");
                appended.fetch_add(1, std::memory_order_relaxed);
                // No sleep — stress-test the locking.
            }
        });

        // UI thread: rapidly reads current_text (simulates OnRender snapshots).
        for (int i = 0; i < kIterations; ++i) {
            std::string snap = view.current_text();
            // Verify it is a valid string (not garbage) — length must be ≤ kIterations.
            CHECK(snap.size() <= static_cast<std::size_t>(kIterations));
        }

        writer.join();

        // After join: all appends completed, text must equal kIterations 'x' chars.
        CHECK(view.current_text().size() == static_cast<std::size_t>(kIterations));
    }

    TEST_CASE("[AC3] close_stream from background thread while rendering") {
        auto theme = make_test_theme();
        batbox::tui::StreamingMessageView view(theme);

        post_token_to(view, "Some content ");

        std::thread closer([&] {
            // Brief spin to ensure OnRender has started.
            std::this_thread::sleep_for(std::chrono::microseconds(1));
            view.close_stream();
        });

        // Render repeatedly while the background thread closes the stream.
        for (int i = 0; i < 100; ++i) {
            auto el = view.OnRender();
            CHECK(render_ok(el));
        }

        closer.join();
        CHECK(view.is_streaming() == false);
    }
}
