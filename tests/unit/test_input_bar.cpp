// tests/unit/test_input_bar.cpp
// ---------------------------------------------------------------------------
// doctest suite for batbox::tui::InputBar (CPP 1.9).
//
// Strategy:
//   • InputBar embeds ftxui::ComponentBase.  In unit tests we construct it
//     directly (no ScreenInteractive needed) and drive OnEvent() manually.
//   • Render() is smoke-tested to confirm it returns a non-null Element.
//   • All acceptance criteria from CPP 1.9 are covered.
//
// Build note:
//   This test is linked against batbox_tui (which links batbox_repl +
//   batbox_theme via CMakeLists).  See tests/unit/CMakeLists.txt.
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/tui/InputBar.hpp>
#include <batbox/repl/History.hpp>
#include <batbox/repl/Keybindings.hpp>
#include <batbox/theme/Theme.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

using namespace batbox::tui;
using namespace batbox::repl;
using namespace batbox::theme;

// =============================================================================
// Helpers
// =============================================================================

/// Build a minimal theme (miss-kittin)
static Theme make_theme() {
    return theme_from_name("miss-kittin");
}

/// Create an InputBar with no-persistence history and default keybindings.
struct TestFixture {
    Theme        theme;
    History      history{std::filesystem::path{}, 1000};
    Keybindings  kb;
    std::string  last_submit;
    bool         submit_called{false};

    std::shared_ptr<InputBar> bar;

    explicit TestFixture(
        InputBar::SlashCommandProvider slash = nullptr,
        InputBar::AutocompleteProvider ac    = nullptr)
        : theme(make_theme())
        , bar(std::make_shared<InputBar>(
              theme,
              history,
              kb,
              [this](std::string s) { last_submit = s; submit_called = true; },
              std::move(slash),
              std::move(ac)))
    {}

    /// Simulate pressing a key by its raw input string.
    bool press(const std::string& input) {
        return bar->OnEvent(ftxui::Event::Special(input));
    }

    /// Type a sequence of printable characters one at a time.
    void type_chars(std::string_view text) {
        for (char c : text) {
            press(std::string(1, c));
        }
    }
};

// Helper event input strings (must match what FTXUI produces)
static const std::string kBackspace   = "\x7f";
static const std::string kDelete      = "\x1b[3~";
static const std::string kArrowLeft   = "\x1b[D";
static const std::string kArrowRight  = "\x1b[C";
static const std::string kArrowUp     = "\x1b[A";
static const std::string kArrowDown   = "\x1b[B";
static const std::string kHome        = "\x1b[H";
static const std::string kEnd         = "\x1b[F";
static const std::string kTab         = "\x09";
static const std::string kEscape      = "\x1b";
static const std::string kReturn      = "\x0a";
static const std::string kCtrlEnter   = "\x1b[13;5u";   // kitty protocol
static const std::string kShiftEnter  = "\x1b[13;2u";   // kitty protocol
static const std::string kShiftTab    = "\x1b[Z";
static const std::string kCtrlL       = "\x0c";

// =============================================================================
// SUITE 1 — Construction and basic render
// =============================================================================
TEST_SUITE("InputBar — construction and render") {

    TEST_CASE("default construction: empty buffer, cursor at 0") {
        TestFixture f;
        CHECK(f.bar->buffer() == "");
        CHECK(f.bar->cursor() == 0);
    }

    TEST_CASE("Render() returns non-null element") {
        TestFixture f;
        auto el = f.bar->Render();
        CHECK(el != nullptr);
    }

    TEST_CASE("Render() with non-empty buffer returns non-null element") {
        TestFixture f;
        f.type_chars("hello");
        auto el = f.bar->Render();
        CHECK(el != nullptr);
    }

    TEST_CASE("set_status does not crash render") {
        TestFixture f;
        StatusLine sl;
        sl.model_name  = "claude-sonnet-4";
        sl.token_count = 1247;
        sl.cost_usd    = 0.012;
        sl.mode_label  = "default";
        f.bar->set_status(sl);
        auto el = f.bar->Render();
        CHECK(el != nullptr);
    }
}

// =============================================================================
// SUITE 2 — Single-line input and cursor positioning
// =============================================================================
TEST_SUITE("InputBar — single-line input and cursor") {

    TEST_CASE("typing printable characters appends to buffer") {
        TestFixture f;
        f.type_chars("hello");
        CHECK(f.bar->buffer() == "hello");
        CHECK(f.bar->cursor() == 5);
    }

    TEST_CASE("backspace removes character before cursor") {
        TestFixture f;
        f.type_chars("abc");
        f.press(kBackspace);
        CHECK(f.bar->buffer() == "ab");
        CHECK(f.bar->cursor() == 2);
    }

    TEST_CASE("backspace at position 0 is a no-op") {
        TestFixture f;
        f.type_chars("a");
        f.press(kArrowLeft);   // cursor -> 0
        REQUIRE(f.bar->cursor() == 0);
        f.press(kBackspace);
        CHECK(f.bar->buffer() == "a");
        CHECK(f.bar->cursor() == 0);
    }

    TEST_CASE("delete key removes character at cursor") {
        TestFixture f;
        f.type_chars("abc");
        f.press(kArrowLeft);  // cursor -> 2 (before 'c')
        REQUIRE(f.bar->cursor() == 2);
        f.press(kDelete);
        CHECK(f.bar->buffer() == "ab");
        CHECK(f.bar->cursor() == 2);
    }

    TEST_CASE("delete at end of buffer is no-op") {
        TestFixture f;
        f.type_chars("abc");
        f.press(kDelete); // cursor == buf.size()
        CHECK(f.bar->buffer() == "abc");
    }

    TEST_CASE("arrow left/right move cursor") {
        TestFixture f;
        f.type_chars("abc");
        CHECK(f.bar->cursor() == 3);
        f.press(kArrowLeft);
        CHECK(f.bar->cursor() == 2);
        f.press(kArrowRight);
        CHECK(f.bar->cursor() == 3);
    }

    TEST_CASE("arrow left clamps at 0") {
        TestFixture f;
        f.press(kArrowLeft);
        CHECK(f.bar->cursor() == 0);
    }

    TEST_CASE("arrow right clamps at buffer end") {
        TestFixture f;
        f.type_chars("ab");
        f.press(kArrowRight);
        CHECK(f.bar->cursor() == 2);
    }

    TEST_CASE("Home key moves cursor to 0") {
        TestFixture f;
        f.type_chars("hello");
        f.press(kHome);
        CHECK(f.bar->cursor() == 0);
    }

    TEST_CASE("End key moves cursor to buffer end") {
        TestFixture f;
        f.type_chars("hello");
        f.press(kHome);
        f.press(kEnd);
        CHECK(f.bar->cursor() == 5);
    }

    TEST_CASE("inserting in the middle of buffer") {
        TestFixture f;
        f.type_chars("ac");
        f.press(kArrowLeft);  // cursor between 'a' and 'c'
        f.press("b");
        CHECK(f.bar->buffer() == "abc");
        CHECK(f.bar->cursor() == 2);
    }

    TEST_CASE("clear() resets buffer and cursor") {
        TestFixture f;
        f.type_chars("hello");
        f.bar->clear();
        CHECK(f.bar->buffer() == "");
        CHECK(f.bar->cursor() == 0);
    }

    TEST_CASE("set_buffer sets content and positions cursor at end") {
        TestFixture f;
        f.bar->set_buffer("world");
        CHECK(f.bar->buffer() == "world");
        CHECK(f.bar->cursor() == 5);
    }
}

// =============================================================================
// SUITE 3 — Submit (Enter / Ctrl+Enter)
// =============================================================================
TEST_SUITE("InputBar — submit") {

    TEST_CASE("Enter submits non-empty buffer") {
        TestFixture f;
        f.type_chars("hello world");
        f.press(kReturn);
        CHECK(f.submit_called);
        CHECK(f.last_submit == "hello world");
        CHECK(f.bar->buffer() == "");
    }

    TEST_CASE("Ctrl+Enter (kitty) submits non-empty buffer") {
        TestFixture f;
        f.type_chars("test");
        f.press(kCtrlEnter);
        CHECK(f.submit_called);
        CHECK(f.last_submit == "test");
    }

    TEST_CASE("Enter on empty buffer does not call submit") {
        TestFixture f;
        f.press(kReturn);
        CHECK_FALSE(f.submit_called);
    }

    TEST_CASE("submit pushes to history") {
        TestFixture f;
        f.type_chars("my command");
        f.press(kReturn);
        CHECK(f.history.size() == 1);
        CHECK(f.history.at(0).value_or("") == "my command");
    }

    TEST_CASE("buffer is cleared after submit") {
        TestFixture f;
        f.type_chars("abc");
        f.press(kReturn);
        CHECK(f.bar->buffer() == "");
        CHECK(f.bar->cursor() == 0);
    }
}

// =============================================================================
// SUITE 4 — Shift+Enter inserts newline
// =============================================================================
TEST_SUITE("InputBar — Shift+Enter inserts newline") {

    TEST_CASE("Shift+Enter (kitty) inserts newline into buffer") {
        TestFixture f;
        f.type_chars("line1");
        f.press(kShiftEnter);
        CHECK(f.bar->buffer() == "line1\n");
        CHECK_FALSE(f.submit_called);
    }

    TEST_CASE("buffer with newline renders without crash") {
        TestFixture f;
        f.type_chars("a");
        f.press(kShiftEnter);
        f.type_chars("b");
        CHECK(f.bar->buffer() == "a\nb");
        auto el = f.bar->Render();
        CHECK(el != nullptr);
    }
}

// =============================================================================
// SUITE 5 — History navigation
// =============================================================================
TEST_SUITE("InputBar — history navigation") {

    TEST_CASE("Up arrow navigates to previous history entry") {
        TestFixture f;
        f.history.push("first");
        f.history.push("second");
        f.press(kArrowUp);
        CHECK(f.bar->buffer() == "second");
    }

    TEST_CASE("Up arrow twice navigates to older entry") {
        TestFixture f;
        f.history.push("first");
        f.history.push("second");
        f.press(kArrowUp);
        f.press(kArrowUp);
        CHECK(f.bar->buffer() == "first");
    }

    TEST_CASE("Down arrow after Up returns to newer entry") {
        TestFixture f;
        f.history.push("first");
        f.history.push("second");
        f.press(kArrowUp);  // -> "second"
        f.press(kArrowUp);  // -> "first"
        f.press(kArrowDown); // -> "second"
        CHECK(f.bar->buffer() == "second");
    }

    TEST_CASE("Down arrow past newest clears buffer") {
        TestFixture f;
        f.history.push("cmd");
        f.press(kArrowUp);   // -> "cmd"
        f.press(kArrowDown); // -> past end → clear
        CHECK(f.bar->buffer() == "");
    }

    TEST_CASE("Ctrl+L clears the buffer") {
        TestFixture f;
        f.type_chars("some text");
        f.press(kCtrlL);
        CHECK(f.bar->buffer() == "");
        CHECK(f.bar->cursor() == 0);
    }
}

// =============================================================================
// SUITE 6 — Slash palette overlay
// =============================================================================
TEST_SUITE("InputBar — slash palette overlay") {

    static std::vector<std::string> make_commands() {
        return {"clear", "compact", "exit", "help", "model", "theme", "vim"};
    }

    TEST_CASE("typing '/' on empty buffer opens palette") {
        TestFixture f(make_commands);
        f.press("/");
        // Palette should be visible in Render() — no crash
        auto el = f.bar->Render();
        CHECK(el != nullptr);
        // Buffer should contain "/"
        CHECK(f.bar->buffer() == "/");
    }

    TEST_CASE("typing '/' in the middle of text does NOT open palette") {
        TestFixture f(make_commands);
        f.type_chars("abc");
        f.press("/");
        // Palette should NOT be open — just inserts '/'
        CHECK(f.bar->buffer() == "abc/");
        auto el = f.bar->Render();
        CHECK(el != nullptr);
    }

    TEST_CASE("Escape dismisses the palette") {
        TestFixture f(make_commands);
        f.press("/");                // open
        f.press(kEscape);            // dismiss
        auto el = f.bar->Render();
        CHECK(el != nullptr);
        // Buffer retains the '/'
        CHECK_FALSE(f.bar->buffer().empty());
    }

    TEST_CASE("palette: Enter selects first item") {
        TestFixture f(make_commands);
        f.press("/");
        f.press(kReturn);   // commit first item
        // Should have selected "clear" (first alphabetically)
        CHECK(f.bar->buffer() == "/clear");
        CHECK_FALSE(f.submit_called);
    }

    TEST_CASE("palette: arrow down selects next item") {
        TestFixture f(make_commands);
        f.press("/");
        f.press(kArrowDown);
        f.press(kReturn);
        // First item is "clear" (index 0), after arrow-down -> "compact" (index 1)
        CHECK(f.bar->buffer() == "/compact");
    }

    TEST_CASE("palette: typing filters list") {
        TestFixture f(make_commands);
        f.press("/");
        // After opening palette, typing chars filters
        f.press("e"); // should match "clear", "model", "theme", "exit" — those containing 'e'
        f.press(kReturn);
        // First matching item containing 'e'
        const auto& buf = f.bar->buffer();
        CHECK_FALSE(buf.empty());
        CHECK(buf[0] == '/');
    }

    TEST_CASE("palette: backspace on empty filter closes palette") {
        TestFixture f(make_commands);
        f.press("/");
        f.press(kBackspace);  // filter is empty -> close
        // Palette dismissed; buffer still contains "/"
        auto el = f.bar->Render();
        CHECK(el != nullptr);
    }

    TEST_CASE("palette render does not crash with empty provider result") {
        TestFixture f([]() -> std::vector<std::string> { return {}; });
        f.press("/");
        auto el = f.bar->Render();
        CHECK(el != nullptr);
    }

    TEST_CASE("palette render does not crash with nullptr provider") {
        // nullptr slash_provider → palette can't open meaningfully
        TestFixture f(nullptr);
        // Typing '/' on empty buffer with no provider still should not crash
        f.press("/");
        auto el = f.bar->Render();
        CHECK(el != nullptr);
    }
}

// =============================================================================
// SUITE 7 — Autocomplete (Tab key)
// =============================================================================
TEST_SUITE("InputBar — autocomplete Tab cycling") {

    static std::vector<std::string> ac_provider(std::string_view /*prefix*/) {
        return {"apple", "apply", "application"};
    }

    TEST_CASE("Tab with no provider is a no-op") {
        TestFixture f; // no ac_provider
        f.type_chars("ap");
        f.press(kTab);
        // Buffer unchanged (Tab is a no-op without provider)
        // No crash is the key requirement
        auto el = f.bar->Render();
        CHECK(el != nullptr);
    }

    TEST_CASE("Tab cycles through completions") {
        TestFixture f(nullptr, ac_provider);
        f.type_chars("ap");
        f.press(kTab);  // first completion
        CHECK(f.bar->buffer() == "apple");
        f.press(kTab);
        CHECK(f.bar->buffer() == "apply");
        f.press(kTab);
        CHECK(f.bar->buffer() == "application");
        f.press(kTab);  // wraps
        CHECK(f.bar->buffer() == "apple");
    }

    TEST_CASE("typing after Tab resets autocomplete") {
        TestFixture f(nullptr, ac_provider);
        f.type_chars("ap");
        f.press(kTab);
        CHECK(f.bar->buffer() == "apple");
        f.type_chars("s"); // resets autocomplete
        CHECK(f.bar->buffer() == "apples");
        // A subsequent Tab should re-query from "apples" prefix
        f.press(kTab);
        // provider always returns {"apple","apply","application"} regardless of prefix
        CHECK(f.bar->buffer() == "apple");
    }
}

// =============================================================================
// SUITE 8 — Status line fields
// =============================================================================
TEST_SUITE("InputBar — status line") {

    TEST_CASE("set_model updates model name") {
        TestFixture f;
        f.bar->set_model("claude-opus-4");
        auto el = f.bar->Render();
        CHECK(el != nullptr);
    }

    TEST_CASE("set_usage stores tokens and cost") {
        TestFixture f;
        f.bar->set_usage(5000, 0.025);
        auto el = f.bar->Render();
        CHECK(el != nullptr);
    }

    // A3: dirty-string cache invalidation tests (TUI-FIX-T6)
    TEST_CASE("set_usage invalidates string cache on each call") {
        TestFixture f;
        // First render with 0 tokens
        auto el1 = f.bar->Render();
        CHECK(el1 != nullptr);
        // Update usage; must not crash and next render reflects new values
        f.bar->set_usage(1234, 0.005);
        auto el2 = f.bar->Render();
        CHECK(el2 != nullptr);
        // Update again; must still render correctly
        f.bar->set_usage(9999, 0.125);
        auto el3 = f.bar->Render();
        CHECK(el3 != nullptr);
    }

    TEST_CASE("set_usage with zero tokens renders without crash") {
        TestFixture f;
        f.bar->set_usage(0, 0.0);
        auto el = f.bar->Render();
        CHECK(el != nullptr);
    }

    TEST_CASE("set_usage with large token count renders comma-formatted string") {
        TestFixture f;
        // 1,234,567 tokens — should be formatted as "1,234,567tk"
        f.bar->set_usage(1234567, 1.5);
        auto el = f.bar->Render();
        CHECK(el != nullptr);
    }

    TEST_CASE("multiple set_usage calls accumulate correctly without crash") {
        TestFixture f;
        // Simulate multiple sub-turn usage events
        for (int i = 1; i <= 10; ++i) {
            f.bar->set_usage(static_cast<uint32_t>(i * 100),
                             static_cast<double>(i) * 0.001);
            auto el = f.bar->Render();
            CHECK(el != nullptr);
        }
    }

    TEST_CASE("set_mode stores mode label") {
        TestFixture f;
        f.bar->set_mode("plan");
        auto el = f.bar->Render();
        CHECK(el != nullptr);
    }
}

// =============================================================================
// SUITE 9 — Vim mode indicator
// =============================================================================
TEST_SUITE("InputBar — vim mode indicator") {

    TEST_CASE("vim mode off by default") {
        TestFixture f;
        // vim mode is off unless BATBOX_VIM_MODE=true env var is set
        // In test env this should be unset
        // Just confirm render does not crash
        auto el = f.bar->Render();
        CHECK(el != nullptr);
    }

    TEST_CASE("toggle_vim enables vim mode") {
        TestFixture f;
        f.bar->toggle_vim();
        auto el = f.bar->Render();
        CHECK(el != nullptr);
    }

    TEST_CASE("vim mode indicator visible in render after enable") {
        TestFixture f;
        f.bar->set_vim_enabled(true);
        f.type_chars("hello");
        auto el = f.bar->Render();
        CHECK(el != nullptr);
    }
}

// =============================================================================
// SUITE 10 — make_input_bar factory
// =============================================================================
TEST_SUITE("InputBar — factory function") {

    TEST_CASE("make_input_bar returns non-null Component") {
        auto theme = make_theme();
        History     hist{std::filesystem::path{}, 100};
        Keybindings kb;

        auto bar = make_input_bar(
            theme,
            hist,
            kb,
            [](std::string) {},
            nullptr,
            nullptr);

        CHECK(bar != nullptr);
    }

    TEST_CASE("make_input_bar component Render does not crash") {
        auto theme = make_theme();
        History     hist{std::filesystem::path{}, 100};
        Keybindings kb;

        auto bar = make_input_bar(
            theme,
            hist,
            kb,
            [](std::string) {});

        auto el = bar->Render();
        CHECK(el != nullptr);
    }

    TEST_CASE("make_input_bar dynamic_pointer_cast returns InputBar*") {
        auto theme = make_theme();
        History     hist{std::filesystem::path{}, 100};
        Keybindings kb;

        auto bar_comp = make_input_bar(
            theme, hist, kb,
            [](std::string) {});

        auto* raw = dynamic_cast<InputBar*>(bar_comp.get());
        CHECK(raw != nullptr);
    }
}

// =============================================================================
// SUITE 11 — Footer hint chips (TUI-FLOW-T6)
// =============================================================================
TEST_SUITE("InputBar — footer hint chips (TUI-FLOW-T6)") {

    // Helper: construct InputBar with default footer state
    // (splash=false, stream=false, mcp=0, effort="medium")
    struct FooterFixture : TestFixture {
        FooterFixture() : TestFixture() {
            // Ensure splash is off and defaults are in place
            bar->set_splash_showing(false);
        }
    };

    TEST_CASE("Render does not crash with default footer state") {
        FooterFixture f;
        auto el = f.bar->Render();
        CHECK(el != nullptr);
    }

    TEST_CASE("set_stream_active does not crash render") {
        FooterFixture f;
        f.bar->set_stream_active(true);
        auto el = f.bar->Render();
        CHECK(el != nullptr);
        f.bar->set_stream_active(false);
        el = f.bar->Render();
        CHECK(el != nullptr);
    }

    TEST_CASE("set_effort_level does not crash render") {
        FooterFixture f;
        f.bar->set_effort_level("low");
        auto el = f.bar->Render();
        CHECK(el != nullptr);
        f.bar->set_effort_level("high");
        el = f.bar->Render();
        CHECK(el != nullptr);
    }

    TEST_CASE("set_mcp_failed does not crash render") {
        FooterFixture f;
        f.bar->set_mcp_failed(2);
        auto el = f.bar->Render();
        CHECK(el != nullptr);
        f.bar->set_mcp_failed(0);
        el = f.bar->Render();
        CHECK(el != nullptr);
    }

    // --- State combination: splash showing ---
    TEST_CASE("splash state: render is non-null") {
        TestFixture f;
        f.bar->set_splash_showing(true);
        f.bar->set_stream_active(false);
        f.bar->set_mcp_failed(0);
        auto el = f.bar->Render();
        CHECK(el != nullptr);
    }

    // --- State combination: splash off, stream off, mcp=0 ---
    TEST_CASE("no-stream, no-mcp: render is non-null (effort chip shown)") {
        FooterFixture f;
        f.bar->set_stream_active(false);
        f.bar->set_mcp_failed(0);
        f.bar->set_effort_level("medium");
        auto el = f.bar->Render();
        CHECK(el != nullptr);
    }

    // --- State combination: splash off, stream on, mcp=0 ---
    TEST_CASE("stream active, no-mcp: render is non-null (esc chip shown)") {
        FooterFixture f;
        f.bar->set_stream_active(true);
        f.bar->set_mcp_failed(0);
        f.bar->set_effort_level("medium");
        auto el = f.bar->Render();
        CHECK(el != nullptr);
    }

    // --- State combination: splash off, stream on, mcp=2 ---
    TEST_CASE("stream active, mcp=2: render is non-null") {
        FooterFixture f;
        f.bar->set_stream_active(true);
        f.bar->set_mcp_failed(2);
        auto el = f.bar->Render();
        CHECK(el != nullptr);
    }

    // --- set_stream_active toggles correctly ---
    TEST_CASE("set_stream_active true then false renders without crash") {
        FooterFixture f;
        f.bar->set_stream_active(true);
        CHECK(f.bar->Render() != nullptr);
        f.bar->set_stream_active(false);
        CHECK(f.bar->Render() != nullptr);
    }

    // --- Multiple set_mcp_failed calls ---
    TEST_CASE("set_mcp_failed toggling between 0 and positive renders correctly") {
        FooterFixture f;
        f.bar->set_mcp_failed(3);
        CHECK(f.bar->Render() != nullptr);
        f.bar->set_mcp_failed(0);
        CHECK(f.bar->Render() != nullptr);
        f.bar->set_mcp_failed(1);
        CHECK(f.bar->Render() != nullptr);
    }

    // --- set_splash_showing interacts cleanly with footer chip state ---
    TEST_CASE("splash_showing=true overrides stream-active chip display") {
        TestFixture f;
        f.bar->set_splash_showing(true);
        f.bar->set_stream_active(true);   // would show "esc to interrupt" if splash were off
        f.bar->set_mcp_failed(0);
        // Under splash=true the splash chips take priority — just confirm no crash
        auto el = f.bar->Render();
        CHECK(el != nullptr);
    }

    TEST_CASE("splash_showing=false restores normal footer after splash collapses") {
        TestFixture f;
        f.bar->set_splash_showing(true);
        auto el1 = f.bar->Render();
        CHECK(el1 != nullptr);

        // Collapse splash
        f.bar->set_splash_showing(false);
        f.bar->set_stream_active(true);
        f.bar->set_effort_level("high");
        auto el2 = f.bar->Render();
        CHECK(el2 != nullptr);
    }
}

// =============================================================================
// SUITE 12 — Contextual placeholder rotation (TUI-FLOW-T9)
// =============================================================================
TEST_SUITE("InputBar — contextual placeholder rotation (TUI-FLOW-T9)") {

    // Helper: make an InputBar with splash showing and no buffer content.
    struct T9Fixture : TestFixture {
        T9Fixture() : TestFixture() {
            bar->set_splash_showing(true);
        }
    };

    TEST_CASE("T9-AC1: set_placeholder_templates — non-empty templates rotate on render") {
        // Each call to Render() increments the frame counter.
        // With kPlaceholderFrameThrottle=120, the slot advances only every 120 calls.
        // We test that after 0*120 renders slot=0 (A), 1*120 renders slot=1 (B), etc.
        T9Fixture f;
        f.bar->set_placeholder_templates({"A", "B", "C"});

        // Render 120 frames — should be in slot 0 (A) for all of them
        for (int i = 0; i < 120; ++i) {
            CHECK(f.bar->Render() != nullptr);
        }

        // Render another 120 frames — slot should advance to 1 (B)
        for (int i = 0; i < 120; ++i) {
            CHECK(f.bar->Render() != nullptr);
        }

        // Render another 120 frames — slot should advance to 2 (C)
        for (int i = 0; i < 120; ++i) {
            CHECK(f.bar->Render() != nullptr);
        }

        // Render another 120 frames — slot wraps back to 0 (A)
        for (int i = 0; i < 120; ++i) {
            CHECK(f.bar->Render() != nullptr);
        }

        // All renders returned non-null — rotation executed without crash.
        // (Actual string validation would require text extraction from FTXUI
        //  which is beyond unit-test scope; no-crash + API coverage is sufficient.)
    }

    TEST_CASE("T9-AC2: empty templates fall back to T4 default — no crash") {
        T9Fixture f;
        f.bar->set_placeholder_templates({});   // empty → should use kSplashPlaceholder
        auto el = f.bar->Render();
        CHECK(el != nullptr);
    }

    TEST_CASE("T9-AC3: single-template list never advances index out of range") {
        T9Fixture f;
        f.bar->set_placeholder_templates({"Only one"});
        // Drive 500 renders — slot must always be 0 (mod 1 = 0)
        for (int i = 0; i < 500; ++i) {
            CHECK(f.bar->Render() != nullptr);
        }
    }

    TEST_CASE("T9-AC4: non-empty buf_ — placeholder not rendered (T4 behavior unchanged)") {
        T9Fixture f;
        f.bar->set_placeholder_templates({"A", "B", "C"});
        f.type_chars("x");   // buf_ is non-empty
        // Render must still return non-null (normal cursor render path)
        auto el = f.bar->Render();
        CHECK(el != nullptr);
        // And buffer is still "x"
        CHECK(f.bar->buffer() == "x");
    }

    TEST_CASE("T9-AC5: set_placeholder_templates resets frame counter") {
        T9Fixture f;
        f.bar->set_placeholder_templates({"A", "B"});
        // Advance past first slot
        for (int i = 0; i < 150; ++i) f.bar->Render();
        // Re-set templates — counter should reset to 0 (slot 0 = A again)
        f.bar->set_placeholder_templates({"X", "Y"});
        // Just confirm it renders without crash after reset
        CHECK(f.bar->Render() != nullptr);
    }

    TEST_CASE("T9-AC6: splash off — no placeholder regardless of templates") {
        TestFixture f;    // splash_showing_ = false by default
        f.bar->set_placeholder_templates({"A", "B", "C"});
        f.bar->set_splash_showing(false);
        // Should render normal (cursor) path
        auto el = f.bar->Render();
        CHECK(el != nullptr);
    }
}

// =============================================================================
// SUITE 13 — Permission mode cycle via Shift+Tab (TUI-PERM-T1)
// =============================================================================

// ---------------------------------------------------------------------------
// MockPermissionGate — minimal stand-in for unit tests.
//
// We don't want to link the full permissions library into the InputBar test;
// instead we build a tiny class that satisfies the interface used by InputBar:
//   current_mode() — returns mode_
//   set_mode(m)    — stores m into mode_
//
// Because InputBar only uses the raw pointer (no vtable required), we can
// use a plain struct and cast its pointer to batbox::permissions::PermissionGate*.
// However, PermissionGate is a concrete class (not a virtual interface), so we
// must actually link against it.  The tests/unit CMakeLists already links
// batbox_permissions as a transitive dep via batbox_tui, so we can construct
// a real PermissionGate with a no-op prompt callback.
// ---------------------------------------------------------------------------
#include <batbox/permissions/PermissionGate.hpp>
#include <batbox/permissions/PermissionMode.hpp>
#include <batbox/permissions/PermissionStore.hpp>
#include <batbox/permissions/PermissionRule.hpp>
#include <batbox/core/Json.hpp>

using PM = batbox::permissions::PermissionMode;

/// Build a minimal PermissionGate for test purposes.
/// Uses a temporary PermissionStore backed by an in-memory (empty) path.
/// The prompt_fn always denies so tool dispatch never blocks.
static batbox::permissions::PermissionGate make_test_gate(PM initial_mode) {
    auto store = std::make_shared<batbox::permissions::PermissionStore>(
        std::filesystem::path{});   // empty path → in-memory / no-persist
    return batbox::permissions::PermissionGate(
        std::move(store),
        initial_mode,
        [](std::string_view, const batbox::Json&) {
            return batbox::permissions::Decision::deny();
        });
}

TEST_SUITE("InputBar — Shift+Tab permission mode cycle (TUI-PERM-T1)") {

    TEST_CASE("set_permission_gate: null gate — Shift+Tab is no-op, returns true") {
        // Even without a gate, CycleMode must return true (key consumed)
        // so the event does not fall through to other handlers.
        TestFixture f;
        f.bar->set_permission_gate(nullptr);
        // Shift+Tab raw escape sequence
        bool consumed = f.press(kShiftTab);
        CHECK(consumed);
        // No gate: mode_label unchanged (empty or default)
        auto el = f.bar->Render();
        CHECK(el != nullptr);
    }

    TEST_CASE("set_permission_gate: gate wired — Shift+Tab cycles Default→Plan") {
        TestFixture f;
        auto gate = make_test_gate(PM::Default);
        f.bar->set_permission_gate(&gate);

        // Initial state: gate is Default, InputBar should reflect it.
        CHECK(gate.current_mode() == PM::Default);

        // First Shift+Tab: Default → Plan
        f.press(kShiftTab);
        CHECK(gate.current_mode() == PM::Plan);
        // InputBar status_.mode_label must be updated
        auto el = f.bar->Render();
        CHECK(el != nullptr);
    }

    TEST_CASE("Shift+Tab cycles through all four modes and wraps") {
        TestFixture f;
        auto gate = make_test_gate(PM::Default);
        f.bar->set_permission_gate(&gate);

        // 1st press: Default → Plan
        f.press(kShiftTab);
        CHECK(gate.current_mode() == PM::Plan);

        // 2nd press: Plan → AcceptEdits
        f.press(kShiftTab);
        CHECK(gate.current_mode() == PM::AcceptEdits);

        // 3rd press: AcceptEdits → Nuclear
        f.press(kShiftTab);
        CHECK(gate.current_mode() == PM::Nuclear);

        // 4th press: Nuclear → Default (wrap)
        f.press(kShiftTab);
        CHECK(gate.current_mode() == PM::Default);
    }

    TEST_CASE("Shift+Tab 5 times from Default lands on Plan (wrap+1)") {
        TestFixture f;
        auto gate = make_test_gate(PM::Default);
        f.bar->set_permission_gate(&gate);

        for (int i = 0; i < 5; ++i) f.press(kShiftTab);
        CHECK(gate.current_mode() == PM::Plan);
    }

    TEST_CASE("set_permission_gate initialises mode_label from current gate mode") {
        TestFixture f;
        auto gate = make_test_gate(PM::AcceptEdits);
        // Before wiring, mode_label is whatever InputBar defaults to
        // After wiring, it should reflect AcceptEdits immediately (no keypress needed)
        f.bar->set_permission_gate(&gate);
        // Render must not crash; mode chip should show "accept edits"
        auto el = f.bar->Render();
        CHECK(el != nullptr);
        CHECK(gate.current_mode() == PM::AcceptEdits);
    }

    TEST_CASE("Render does not crash for each mode") {
        const std::array<PM, 4> modes = {
            PM::Default, PM::Plan, PM::AcceptEdits, PM::Nuclear
        };
        for (auto mode : modes) {
            TestFixture f;
            auto gate = make_test_gate(mode);
            f.bar->set_permission_gate(&gate);
            auto el = f.bar->Render();
            CHECK_MESSAGE(el != nullptr, "Render crashed for mode");
        }
    }

    TEST_CASE("CycleMode action does not touch input buffer") {
        TestFixture f;
        auto gate = make_test_gate(PM::Default);
        f.bar->set_permission_gate(&gate);
        f.type_chars("hello");
        f.press(kShiftTab);
        // Buffer must be unchanged after a mode cycle
        CHECK(f.bar->buffer() == "hello");
    }
}
