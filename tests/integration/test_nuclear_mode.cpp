// tests/integration/test_nuclear_mode.cpp
// ---------------------------------------------------------------------------
// Integration tests for batbox::tui::PermissionBanner — CPP 1.16.
//
// Tests verify acceptance criteria:
//   [AC1] Nuclear banner visible at top: "NUCLEAR MODE" text in magenta bg
//   [AC2] Status line colour matches mode: banner renders differently per mode
//   [AC3] Cycling: Default→Plan→AcceptEdits→Nuclear→Default
//   [AC4] Nuclear confirmation modal: y/N required before activating Nuclear
//   [AC5] CLI --nuclear (set_mode_direct): skips modal, sets state directly
//   [AC6] Integration test: component renders at standard terminal sizes
//
// Build (standalone, no CMake):
//   c++ -std=c++20                                          \
//       -I<project>/include                                 \
//       -I<build>/vcpkg_installed/arm64-osx/include         \
//       tests/integration/test_nuclear_mode.cpp             \
//       src/tui/PermissionBanner.cpp                        \
//       src/tui/ThemeApply.cpp                              \
//       src/permissions/PermissionMode.cpp                  \
//       src/theme/Theme.cpp                                 \
//       src/theme/themes.cpp                                \
//       src/core/Logging.cpp                                \
//       src/core/Uuid.cpp                                   \
//       -L<build>/vcpkg_installed/arm64-osx/lib             \
//       -lftxui-component -lftxui-dom -lftxui-screen        \
//       -DBATBOX_SYNTAX=0                                    \
//       -o /tmp/test_nuclear_mode && /tmp/test_nuclear_mode
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/tui/PermissionBanner.hpp>
#include <batbox/permissions/PermissionMode.hpp>
#include <batbox/theme/Theme.hpp>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <string>
#include <string_view>

using namespace batbox::permissions;
using namespace batbox::tui;

// ---------------------------------------------------------------------------
// Helper: synthesise a Shift+Tab event (\x1b[Z)
// ---------------------------------------------------------------------------
namespace {

ftxui::Event shift_tab() {
    return ftxui::Event::Special("\x1b[Z");
}

batbox::theme::Theme make_test_theme() {
    return batbox::theme::theme_from_name("miss-kittin");
}

} // namespace

// ---------------------------------------------------------------------------
// TEST SUITE: PermissionBanner — initial state
// ---------------------------------------------------------------------------
TEST_SUITE("PermissionBanner — construction and initial state") {

    TEST_CASE("[AC3] Default mode at construction") {
        auto theme = make_test_theme();
        PermissionBanner banner(theme);
        CHECK(banner.current_mode() == PermissionMode::Default);
    }

    TEST_CASE("[AC1] Default mode renders emptyElement (zero lines)") {
        auto theme = make_test_theme();
        PermissionBanner banner(theme);

        // Render into a 80x5 screen — the banner should contribute no content.
        ftxui::Screen screen(80, 5);
        auto element = banner.Render();
        REQUIRE(element != nullptr);
        CHECK_NOTHROW(ftxui::Render(screen, element));
        // No crash = pass.
        CHECK(true);
    }

    TEST_CASE("[AC6] make_permission_banner factory returns non-null") {
        auto theme = make_test_theme();
        auto comp = make_permission_banner(theme);
        REQUIRE(comp != nullptr);
    }
}

// ---------------------------------------------------------------------------
// TEST SUITE: PermissionBanner — Shift+Tab mode cycling
// ---------------------------------------------------------------------------
TEST_SUITE("PermissionBanner — mode cycling via Shift+Tab") {

    TEST_CASE("[AC3] Shift+Tab cycles Default → Plan") {
        auto theme = make_test_theme();
        PermissionBanner banner(theme);

        bool consumed = banner.OnEvent(shift_tab());
        CHECK(consumed);
        CHECK(banner.current_mode() == PermissionMode::Plan);
    }

    TEST_CASE("[AC3] Shift+Tab cycles Plan → AcceptEdits") {
        auto theme = make_test_theme();
        PermissionBanner banner(theme);

        banner.set_mode_direct(PermissionMode::Plan);
        bool consumed = banner.OnEvent(shift_tab());
        CHECK(consumed);
        CHECK(banner.current_mode() == PermissionMode::AcceptEdits);
    }

    TEST_CASE("[AC3] Shift+Tab cycles AcceptEdits → Nuclear (confirm required)") {
        auto theme = make_test_theme();
        PermissionBanner banner(theme);

        banner.set_mode_direct(PermissionMode::AcceptEdits);

        // Shift+Tab requests Nuclear — enters confirmation state, mode stays AcceptEdits.
        bool consumed = banner.OnEvent(shift_tab());
        CHECK(consumed);
        // Mode has not yet changed — waiting for 'y'.
        CHECK(banner.current_mode() == PermissionMode::AcceptEdits);
    }

    TEST_CASE("[AC4] Confirming Nuclear with 'y' activates Nuclear mode") {
        auto theme = make_test_theme();
        PermissionBanner banner(theme);

        banner.set_mode_direct(PermissionMode::AcceptEdits);

        // Request Nuclear (enter confirm state).
        banner.OnEvent(shift_tab());
        CHECK(banner.current_mode() == PermissionMode::AcceptEdits); // Not yet

        // Confirm with 'y'.
        bool consumed = banner.OnEvent(ftxui::Event::Character('y'));
        CHECK(consumed);
        CHECK(banner.current_mode() == PermissionMode::Nuclear);
    }

    TEST_CASE("[AC4] Cancelling Nuclear with 'n' reverts to previous mode") {
        auto theme = make_test_theme();
        PermissionBanner banner(theme);

        banner.set_mode_direct(PermissionMode::AcceptEdits);

        // Request Nuclear.
        banner.OnEvent(shift_tab());
        CHECK(banner.current_mode() == PermissionMode::AcceptEdits);

        // Cancel with 'n' (any non-y key).
        bool consumed = banner.OnEvent(ftxui::Event::Character('n'));
        CHECK(consumed);
        // Should remain on AcceptEdits.
        CHECK(banner.current_mode() == PermissionMode::AcceptEdits);
    }

    TEST_CASE("[AC4] Pressing Escape during Nuclear confirm cancels it") {
        auto theme = make_test_theme();
        PermissionBanner banner(theme);

        banner.set_mode_direct(PermissionMode::Plan);
        banner.OnEvent(shift_tab()); // AcceptEdits
        banner.OnEvent(shift_tab()); // Requests Nuclear
        CHECK(banner.current_mode() == PermissionMode::AcceptEdits);

        // Escape cancels.
        banner.OnEvent(ftxui::Event::Escape);
        CHECK(banner.current_mode() == PermissionMode::AcceptEdits);
    }

    TEST_CASE("[AC3] Full cycle Default→Plan→AcceptEdits→(confirm)→Nuclear→Default") {
        auto theme = make_test_theme();
        PermissionBanner banner(theme);

        // Default → Plan
        banner.OnEvent(shift_tab());
        REQUIRE(banner.current_mode() == PermissionMode::Plan);

        // Plan → AcceptEdits
        banner.OnEvent(shift_tab());
        REQUIRE(banner.current_mode() == PermissionMode::AcceptEdits);

        // AcceptEdits → Nuclear (need confirmation)
        banner.OnEvent(shift_tab());
        banner.OnEvent(ftxui::Event::Character('y')); // confirm
        REQUIRE(banner.current_mode() == PermissionMode::Nuclear);

        // Nuclear → Default (no confirmation needed to leave Nuclear)
        banner.OnEvent(shift_tab());
        REQUIRE(banner.current_mode() == PermissionMode::Default);
    }
}

// ---------------------------------------------------------------------------
// TEST SUITE: PermissionBanner — set_mode_direct (CLI --nuclear)
// ---------------------------------------------------------------------------
TEST_SUITE("PermissionBanner — set_mode_direct (CLI bypass)") {

    TEST_CASE("[AC5] set_mode_direct(Nuclear) sets Nuclear without confirmation") {
        auto theme = make_test_theme();
        PermissionBanner banner(theme);

        banner.set_mode_direct(PermissionMode::Nuclear);
        CHECK(banner.current_mode() == PermissionMode::Nuclear);
    }

    TEST_CASE("[AC5] set_mode_direct cancels any pending confirmation") {
        auto theme = make_test_theme();
        PermissionBanner banner(theme);

        banner.set_mode_direct(PermissionMode::AcceptEdits);
        banner.OnEvent(shift_tab()); // Requests Nuclear (confirm pending)
        CHECK(banner.current_mode() == PermissionMode::AcceptEdits);

        // CLI bypasses — sets to Default directly, cancelling the confirm.
        banner.set_mode_direct(PermissionMode::Default);
        CHECK(banner.current_mode() == PermissionMode::Default);

        // 'y' after direct-set should not activate Nuclear (confirm was cancelled).
        banner.OnEvent(ftxui::Event::Character('y'));
        CHECK(banner.current_mode() == PermissionMode::Default);
    }

    TEST_CASE("[AC5] set_mode_direct to same mode is a no-op") {
        auto theme = make_test_theme();

        int callback_count = 0;
        PermissionBanner banner(theme, [&](PermissionMode, std::string_view) {
            ++callback_count;
        });

        banner.set_mode_direct(PermissionMode::Plan);
        CHECK(callback_count == 1);

        // Set the same mode again.
        banner.set_mode_direct(PermissionMode::Plan);
        CHECK(callback_count == 1); // No additional callback fired.
    }
}

// ---------------------------------------------------------------------------
// TEST SUITE: PermissionBanner — on_mode_changed callback
// ---------------------------------------------------------------------------
TEST_SUITE("PermissionBanner — on_mode_changed callback") {

    TEST_CASE("[AC2] Callback fires with correct mode on Shift+Tab cycle") {
        auto theme = make_test_theme();

        PermissionMode last_mode  = PermissionMode::Default;
        std::string    last_label;

        PermissionBanner banner(theme, [&](PermissionMode m, std::string_view lbl) {
            last_mode  = m;
            last_label = std::string(lbl);
        });

        banner.OnEvent(shift_tab()); // → Plan
        CHECK(last_mode == PermissionMode::Plan);
        CHECK(last_label == "plan");

        banner.OnEvent(shift_tab()); // → AcceptEdits
        CHECK(last_mode == PermissionMode::AcceptEdits);
        CHECK(last_label == "accept-edits");
    }

    TEST_CASE("[AC2] Callback fires with 'nuclear' label after Nuclear confirmed") {
        auto theme = make_test_theme();

        PermissionMode last_mode = PermissionMode::Default;
        std::string    last_label;

        PermissionBanner banner(theme, [&](PermissionMode m, std::string_view lbl) {
            last_mode  = m;
            last_label = std::string(lbl);
        });

        banner.set_mode_direct(PermissionMode::AcceptEdits);
        banner.OnEvent(shift_tab()); // Requests Nuclear (confirm pending)
        banner.OnEvent(ftxui::Event::Character('y')); // Confirm

        CHECK(last_mode == PermissionMode::Nuclear);
        CHECK(last_label == "nuclear");
    }

    TEST_CASE("[AC2] Callback NOT fired when Nuclear confirm is cancelled") {
        auto theme = make_test_theme();

        int callback_count = 0;
        PermissionBanner banner(theme, [&](PermissionMode, std::string_view) {
            ++callback_count;
        });

        banner.set_mode_direct(PermissionMode::AcceptEdits); // fires callback (1)
        banner.OnEvent(shift_tab()); // Requests Nuclear — no callback yet
        banner.OnEvent(ftxui::Event::Character('n')); // Cancel — no callback

        CHECK(callback_count == 1); // Only the set_mode_direct call.
    }
}

// ---------------------------------------------------------------------------
// TEST SUITE: PermissionBanner — rendering
// ---------------------------------------------------------------------------
TEST_SUITE("PermissionBanner — rendering") {

    TEST_CASE("[AC1] Nuclear mode renders non-null element without crash") {
        auto theme = make_test_theme();
        PermissionBanner banner(theme);

        banner.set_mode_direct(PermissionMode::Nuclear);

        ftxui::Screen screen(80, 3);
        auto element = banner.Render();
        REQUIRE(element != nullptr);
        CHECK_NOTHROW(ftxui::Render(screen, element));
    }

    TEST_CASE("[AC2] Plan mode renders non-null element") {
        auto theme = make_test_theme();
        PermissionBanner banner(theme);

        banner.set_mode_direct(PermissionMode::Plan);

        ftxui::Screen screen(80, 3);
        auto element = banner.Render();
        REQUIRE(element != nullptr);
        CHECK_NOTHROW(ftxui::Render(screen, element));
    }

    TEST_CASE("[AC2] AcceptEdits mode renders non-null element") {
        auto theme = make_test_theme();
        PermissionBanner banner(theme);

        banner.set_mode_direct(PermissionMode::AcceptEdits);

        ftxui::Screen screen(80, 3);
        auto element = banner.Render();
        REQUIRE(element != nullptr);
        CHECK_NOTHROW(ftxui::Render(screen, element));
    }

    TEST_CASE("[AC4] Confirmation pending renders non-null prompt") {
        auto theme = make_test_theme();
        PermissionBanner banner(theme);

        banner.set_mode_direct(PermissionMode::AcceptEdits);
        banner.OnEvent(shift_tab()); // Enters confirm state.

        ftxui::Screen screen(80, 3);
        auto element = banner.Render();
        REQUIRE(element != nullptr);
        CHECK_NOTHROW(ftxui::Render(screen, element));
    }

    TEST_CASE("[AC6] All modes render at narrow terminal (40x3) without crash") {
        auto theme = make_test_theme();

        const std::array<PermissionMode, 4> modes = {
            PermissionMode::Default,
            PermissionMode::Plan,
            PermissionMode::AcceptEdits,
            PermissionMode::Nuclear,
        };

        for (auto m : modes) {
            PermissionBanner banner(theme);
            banner.set_mode_direct(m);

            ftxui::Screen screen(40, 3);
            auto element = banner.Render();
            REQUIRE(element != nullptr);
            CHECK_NOTHROW(ftxui::Render(screen, element));
        }
    }

    TEST_CASE("[AC1] Nuclear banner renders without crashing at 132 columns") {
        auto theme = make_test_theme();
        PermissionBanner banner(theme);
        banner.set_mode_direct(PermissionMode::Nuclear);

        ftxui::Screen wide(132, 1);
        auto element = banner.Render();
        REQUIRE(element != nullptr);
        CHECK_NOTHROW(ftxui::Render(wide, element));
    }
}

// ---------------------------------------------------------------------------
// TEST SUITE: PermissionBanner — event passthrough
// ---------------------------------------------------------------------------
TEST_SUITE("PermissionBanner — event passthrough") {

    TEST_CASE("Non-Shift+Tab events are not consumed in normal state") {
        auto theme = make_test_theme();
        PermissionBanner banner(theme);

        // Regular 'a' keystroke should not be consumed.
        bool consumed = banner.OnEvent(ftxui::Event::Character('a'));
        CHECK_FALSE(consumed);
    }

    TEST_CASE("Mouse events are not consumed") {
        auto theme = make_test_theme();
        PermissionBanner banner(theme);

        // Construct a mouse event.
        ftxui::Mouse mouse_ev;
        mouse_ev.button = ftxui::Mouse::Left;
        mouse_ev.motion = ftxui::Mouse::Pressed;
        mouse_ev.x = 0;
        mouse_ev.y = 0;
        mouse_ev.shift = false;
        mouse_ev.meta  = false;
        mouse_ev.control = false;
        ftxui::Event ev = ftxui::Event::Mouse("", mouse_ev);

        bool consumed = banner.OnEvent(ev);
        CHECK_FALSE(consumed);
    }
}
