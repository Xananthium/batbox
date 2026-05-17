// tests/unit/test_modal_picker.cpp
//
// doctest suite for batbox::tui::ModalPicker (CPP 1.12).
//
// Build (standalone, no CMake):
//   c++ -std=c++20                                                        \
//       -I/path/to/project/include                                        \
//       -I/path/to/project/build/vcpkg_installed/arm64-osx/include        \
//       tests/unit/test_modal_picker.cpp                                  \
//       src/tui/ModalPicker.cpp                                           \
//       src/theme/Theme.cpp src/theme/themes.cpp src/tui/ThemeApply.cpp  \
//       -L/path/to/project/build/vcpkg_installed/arm64-osx/lib           \
//       -lftxui-component -lftxui-dom -lftxui-screen                     \
//       -o /tmp/test_modal_picker && /tmp/test_modal_picker
//
// Or via CMake:
//   batbox_add_unit_test(test_modal_picker
//       unit/test_modal_picker.cpp  batbox_tui)

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "batbox/tui/ModalPicker.hpp"
#include "batbox/theme/Theme.hpp"

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <string>
#include <vector>

using namespace batbox::tui;
using namespace batbox::theme;

// =============================================================================
// Shared test fixtures
// =============================================================================

/// Minimal theme for tests — use miss-kittin by name so colours are real.
static Theme test_theme() {
    return theme_from_name("miss-kittin");
}

/// Helper: navigate the picker `n` steps downward via ArrowDown events.
static void nav_down(ModalPicker& p, int n) {
    for (int i = 0; i < n; ++i)
        p.OnEvent(ftxui::Event::ArrowDown);
}

/// Helper: navigate the picker `n` steps upward via ArrowUp events.
static void nav_up(ModalPicker& p, int n) {
    for (int i = 0; i < n; ++i)
        p.OnEvent(ftxui::Event::ArrowUp);
}

/// Helper: type a string into the filter (character by character).
static void type_filter(ModalPicker& p, const std::string& text) {
    for (char c : text)
        p.OnEvent(ftxui::Event::Character(std::string(1, c)));
}

// =============================================================================
// Construction
// =============================================================================
TEST_SUITE("ModalPicker — Construction") {

    TEST_CASE("Picker constructs without throw for non-empty item list") {
        Theme t = test_theme();
        bool called = false;
        CHECK_NOTHROW(ModalPicker(t, "Test", {"a", "b", "c"}, false,
            [&](int) { called = true; }, [&]() {}));
    }

    TEST_CASE("Picker constructs with empty item list") {
        Theme t = test_theme();
        CHECK_NOTHROW(ModalPicker(t, "Empty", {}, false,
            [](int) {}, []() {}));
    }

    TEST_CASE("Picker constructs with filter enabled") {
        Theme t = test_theme();
        CHECK_NOTHROW(ModalPicker(t, "Filter", {"one", "two", "three"}, true,
            [](int) {}, []() {}));
    }
}

// =============================================================================
// Render
// =============================================================================
TEST_SUITE("ModalPicker — Render") {

    TEST_CASE("Render returns a non-null element") {
        Theme t = test_theme();
        ModalPicker p(t, "Title", {"alpha", "beta"}, false,
                      [](int) {}, []() {});
        auto elem = p.Render();
        CHECK(elem != nullptr);
    }

    TEST_CASE("Render does not throw for empty item list") {
        Theme t = test_theme();
        ModalPicker p(t, "Empty", {}, false, [](int) {}, []() {});
        CHECK_NOTHROW(p.Render());
    }

    TEST_CASE("Render does not throw with filter enabled") {
        Theme t = test_theme();
        ModalPicker p(t, "Title", {"alpha", "beta"}, true,
                      [](int) {}, []() {});
        CHECK_NOTHROW(p.Render());
    }
}

// =============================================================================
// Navigation: ArrowDown / ArrowUp
// =============================================================================
TEST_SUITE("ModalPicker — Navigation") {

    TEST_CASE("ArrowDown moves to next item and is consumed") {
        Theme t = test_theme();
        int selected = -1;
        ModalPicker p(t, "Nav", {"A", "B", "C"}, false,
            [&](int i) { selected = i; }, []() {});

        bool consumed = p.OnEvent(ftxui::Event::ArrowDown);
        CHECK(consumed);
        // Press Enter: should select index 1 (B)
        p.OnEvent(ftxui::Event::Return);
        CHECK(selected == 1);
    }

    TEST_CASE("ArrowUp from top wraps to last item") {
        Theme t = test_theme();
        int selected = -1;
        ModalPicker p(t, "Nav", {"A", "B", "C"}, false,
            [&](int i) { selected = i; }, []() {});

        p.OnEvent(ftxui::Event::ArrowUp);  // wrap: 0 -> 2
        p.OnEvent(ftxui::Event::Return);
        CHECK(selected == 2);
    }

    TEST_CASE("ArrowDown from last item wraps to first") {
        Theme t = test_theme();
        int selected = -1;
        ModalPicker p(t, "Nav", {"A", "B", "C"}, false,
            [&](int i) { selected = i; }, []() {});

        nav_down(p, 3); // 0->1->2->0 (wrap)
        p.OnEvent(ftxui::Event::Return);
        CHECK(selected == 0);
    }

    TEST_CASE("j key moves down (vim navigation)") {
        Theme t = test_theme();
        int selected = -1;
        ModalPicker p(t, "Nav", {"X", "Y"}, false,
            [&](int i) { selected = i; }, []() {});

        p.OnEvent(ftxui::Event::Character("j"));
        p.OnEvent(ftxui::Event::Return);
        CHECK(selected == 1);
    }

    TEST_CASE("k key moves up (vim navigation)") {
        Theme t = test_theme();
        int selected = -1;
        ModalPicker p(t, "Nav", {"X", "Y", "Z"}, false,
            [&](int i) { selected = i; }, []() {});

        p.OnEvent(ftxui::Event::Character("k")); // 0 -> 2 (wrap)
        p.OnEvent(ftxui::Event::Return);
        CHECK(selected == 2);
    }

    TEST_CASE("Navigation on empty list does not crash") {
        Theme t = test_theme();
        ModalPicker p(t, "Empty", {}, false, [](int) {}, []() {});
        CHECK_NOTHROW(p.OnEvent(ftxui::Event::ArrowDown));
        CHECK_NOTHROW(p.OnEvent(ftxui::Event::ArrowUp));
    }
}

// =============================================================================
// Selection: Enter key
// =============================================================================
TEST_SUITE("ModalPicker — Selection") {

    TEST_CASE("Enter on first item calls on_select with original index 0") {
        Theme t = test_theme();
        int received = -1;
        ModalPicker p(t, "Select", {"apple", "banana", "cherry"}, false,
            [&](int i) { received = i; }, []() {});

        bool consumed = p.OnEvent(ftxui::Event::Return);
        CHECK(consumed);
        CHECK(received == 0);
    }

    TEST_CASE("Enter on second item calls on_select with original index 1") {
        Theme t = test_theme();
        int received = -1;
        ModalPicker p(t, "Select", {"alpha", "beta", "gamma"}, false,
            [&](int i) { received = i; }, []() {});

        nav_down(p, 1);
        p.OnEvent(ftxui::Event::Return);
        CHECK(received == 1);
    }

    TEST_CASE("Enter on empty list does not invoke callback") {
        Theme t = test_theme();
        bool called = false;
        ModalPicker p(t, "Empty", {}, false,
            [&](int) { called = true; }, []() {});
        p.OnEvent(ftxui::Event::Return);
        CHECK_FALSE(called);
    }

    TEST_CASE("on_select callback receives ORIGINAL index (not filtered index)") {
        Theme t = test_theme();
        // Items: 0=apple 1=avocado 2=banana 3=blueberry
        // Filter "b" -> filtered: [2, 3]; cursor=0 -> original=2
        int received = -1;
        ModalPicker p(t, "Filter-Select",
                      {"apple", "avocado", "banana", "blueberry"},
                      true,
                      [&](int i) { received = i; },
                      []() {});

        type_filter(p, "b");
        p.OnEvent(ftxui::Event::Return);
        CHECK(received == 2);   // "banana" is at original index 2
    }
}

// =============================================================================
// Cancel: Escape key
// =============================================================================
TEST_SUITE("ModalPicker — Cancel") {

    TEST_CASE("Escape calls on_cancel and is consumed") {
        Theme t = test_theme();
        bool cancelled = false;
        ModalPicker p(t, "Cancel", {"a", "b"}, false,
            [](int) {}, [&]() { cancelled = true; });

        bool consumed = p.OnEvent(ftxui::Event::Escape);
        CHECK(consumed);
        CHECK(cancelled);
    }

    TEST_CASE("Escape does not call on_select") {
        Theme t = test_theme();
        bool selected = false;
        ModalPicker p(t, "Cancel", {"a"}, false,
            [&](int) { selected = true; }, []() {});

        p.OnEvent(ftxui::Event::Escape);
        CHECK_FALSE(selected);
    }
}

// =============================================================================
// Filter: typing narrows the list
// =============================================================================
TEST_SUITE("ModalPicker — Filter") {

    TEST_CASE("Typing a character is consumed when show_filter=true") {
        Theme t = test_theme();
        ModalPicker p(t, "Filter", {"alpha", "beta"}, true,
                      [](int) {}, []() {});
        bool consumed = p.OnEvent(ftxui::Event::Character("a"));
        CHECK(consumed);
    }

    TEST_CASE("Typing a character is NOT consumed when show_filter=false") {
        // When filter is off, the OnEvent handler should not eat printable chars
        // (unless they are j/k navigation characters).
        Theme t = test_theme();
        ModalPicker p(t, "NoFilter", {"alpha", "beta"}, false,
                      [](int) {}, []() {});
        // 'x' is not j/k so should return false.
        bool consumed = p.OnEvent(ftxui::Event::Character("x"));
        CHECK_FALSE(consumed);
    }

    TEST_CASE("Filter narrows visible items and Enter returns original index") {
        Theme t = test_theme();
        // Items: gpt-4o(0) claude-3-5-sonnet(1) claude-opus(2) gpt-3.5(3)
        int received = -1;
        ModalPicker p(t, "Model",
                      {"gpt-4o", "claude-3-5-sonnet", "claude-opus", "gpt-3.5"},
                      true,
                      [&](int i) { received = i; },
                      []() {});

        // Type "claude" — should show only indices 1 and 2.
        type_filter(p, "claude");
        // cursor is at 0 -> filtered[0] = original index 1 ("claude-3-5-sonnet")
        p.OnEvent(ftxui::Event::Return);
        CHECK(received == 1);
    }

    TEST_CASE("Filter is case-insensitive") {
        Theme t = test_theme();
        int received = -1;
        ModalPicker p(t, "CI",
                      {"Apple", "Banana", "Apricot"},
                      true,
                      [&](int i) { received = i; },
                      []() {});

        type_filter(p, "AP");  // should match "Apple"(0) and "Apricot"(2)
        nav_down(p, 1);        // cursor -> 1 -> original index 2 "Apricot"
        p.OnEvent(ftxui::Event::Return);
        CHECK(received == 2);
    }

    TEST_CASE("Backspace removes last filter character") {
        Theme t = test_theme();
        int received = -1;
        // Items: foo(0) bar(1) baz(2)
        ModalPicker p(t, "BS",
                      {"foo", "bar", "baz"},
                      true,
                      [&](int i) { received = i; },
                      []() {});

        type_filter(p, "ba");   // matches bar(1) baz(2)
        p.OnEvent(ftxui::Event::Backspace); // filter back to "b" -> still bar(1) baz(2)
        p.OnEvent(ftxui::Event::Backspace); // filter back to "" -> all 3 visible
        // cursor was clamped; should be at 0 -> original 0 "foo"
        p.OnEvent(ftxui::Event::Return);
        CHECK(received == 0);
    }

    TEST_CASE("Filter that matches nothing shows no items; Enter is no-op") {
        Theme t = test_theme();
        bool called = false;
        ModalPicker p(t, "NoMatch",
                      {"alpha", "beta"},
                      true,
                      [&](int) { called = true; },
                      []() {});

        type_filter(p, "zzz");  // no matches
        p.OnEvent(ftxui::Event::Return);
        CHECK_FALSE(called);
    }

    TEST_CASE("Filter renders without throw when no matches") {
        Theme t = test_theme();
        ModalPicker p(t, "NoMatch", {"alpha", "beta"}, true,
                      [](int) {}, []() {});
        type_filter(p, "zzz");
        CHECK_NOTHROW(p.Render());
    }
}

// =============================================================================
// set_items() and reset()
// =============================================================================
TEST_SUITE("ModalPicker — set_items / reset") {

    TEST_CASE("set_items replaces item list and resets filter and cursor") {
        Theme t = test_theme();
        int received = -1;
        ModalPicker p(t, "Dynamic",
                      {"old-a", "old-b"},
                      true,
                      [&](int i) { received = i; },
                      []() {});

        type_filter(p, "old");  // narrow list
        nav_down(p, 1);         // move cursor

        // Replace items with a fresh set.
        p.set_items({"new-x", "new-y", "new-z"});

        // Cursor should be at 0; filter should be cleared.
        p.OnEvent(ftxui::Event::Return);
        CHECK(received == 0);  // first item of new list
    }

    TEST_CASE("reset() clears filter and returns cursor to 0") {
        Theme t = test_theme();
        int received = -1;
        ModalPicker p(t, "Reset",
                      {"alpha", "beta", "gamma"},
                      true,
                      [&](int i) { received = i; },
                      []() {});

        type_filter(p, "beta");  // narrow to [1]
        p.reset();

        // After reset all 3 items visible, cursor at 0.
        p.OnEvent(ftxui::Event::Return);
        CHECK(received == 0);
    }
}

// =============================================================================
// 5+ caller contract: at least 5 realistic usage scenarios
// (verifies the component is wired correctly for /model, /theme, /resume, mode)
// =============================================================================
TEST_SUITE("ModalPicker — Caller contract") {

    TEST_CASE("Caller 1: /model picker selects third model via navigation") {
        Theme t = test_theme();
        std::string chosen;
        std::vector<std::string> models = {
            "gpt-4o", "gpt-4o-mini", "claude-3-5-sonnet", "claude-opus-4", "o3"
        };
        ModalPicker p(t, "Select model", models, true,
            [&](int i) { chosen = models[static_cast<size_t>(i)]; }, []() {});

        nav_down(p, 2);
        p.OnEvent(ftxui::Event::Return);
        CHECK(chosen == "claude-3-5-sonnet");
    }

    TEST_CASE("Caller 2: /theme picker cancels without selection") {
        Theme t = test_theme();
        bool selected = false;
        bool cancelled = false;
        std::vector<std::string> themes = {
            "miss-kittin", "stock-exchange", "frank-sinatra", "monochrome", "classic"
        };
        ModalPicker p(t, "Select theme", themes, false,
            [&](int) { selected = true; }, [&]() { cancelled = true; });

        p.OnEvent(ftxui::Event::Escape);
        CHECK_FALSE(selected);
        CHECK(cancelled);
    }

    TEST_CASE("Caller 3: /resume session picker fuzzy-searches by name") {
        Theme t = test_theme();
        int chosen_idx = -1;
        std::vector<std::string> sessions = {
            "2025-05-14-refactor-auth",
            "2025-05-15-build-modal",
            "2025-05-13-fix-crash",
        };
        ModalPicker p(t, "Resume session", sessions, true,
            [&](int i) { chosen_idx = i; }, []() {});

        type_filter(p, "modal");
        p.OnEvent(ftxui::Event::Return);
        CHECK(chosen_idx == 1);  // "2025-05-15-build-modal"
    }

    TEST_CASE("Caller 4: mode picker (no filter) selects plan mode") {
        Theme t = test_theme();
        int chosen_idx = -1;
        std::vector<std::string> modes = {
            "default", "plan", "acceptEdits", "nuclear"
        };
        ModalPicker p(t, "Select mode", modes, false,
            [&](int i) { chosen_idx = i; }, []() {});

        nav_down(p, 1);
        p.OnEvent(ftxui::Event::Return);
        CHECK(chosen_idx == 1);  // "plan"
    }

    TEST_CASE("Caller 5: agent picker with large list navigates by filter") {
        Theme t = test_theme();
        int chosen_idx = -1;
        std::vector<std::string> agents;
        for (int i = 0; i < 20; ++i)
            agents.push_back("agent-" + std::to_string(i));
        // agent-10 is at original index 10.
        ModalPicker p(t, "Select agent", agents, true,
            [&](int i) { chosen_idx = i; }, []() {});

        type_filter(p, "agent-10");
        p.OnEvent(ftxui::Event::Return);
        CHECK(chosen_idx == 10);
    }
}
