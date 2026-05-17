// tests/unit/test_keybindings.cpp
// ---------------------------------------------------------------------------
// doctest suite for batbox::repl::Keybindings (CPP 2.5).
//
// Coverage:
//   1. default_keybindings() — all expected actions present with correct descriptors
//   2. Constructor — produces event_to_action mappings for defaults
//   3. event_to_action — default bindings fire correct actions
//   4. apply_override — override replaces specific bindings
//   5. apply_override — unknown action names are silently ignored
//   6. apply_override — unrecognised descriptor keeps previous binding
//   7. Conflict detection — two actions sharing a key triggers a warning (smoke)
//   8. key_for() — returns descriptor for an action
//   9. descriptor_map() — snapshot of current bindings
//  10. Ctrl+Enter multi-terminal aliases (kitty + traditional \r\n)
//  11. parse_descriptor smoke — Shift+Tab, Ctrl+L, Up, Escape
//  12. event_to_action returns None for unbound event
//  13. cycle_mode default is Shift+Tab
//  14. history_search default is Ctrl+R
//  15. CycleMode event_to_action via TabReverse
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/repl/Keybindings.hpp>
#include <batbox/config/KeybindingsConfig.hpp>

#include <string>

using namespace batbox::repl;
using namespace batbox::config;

// ============================================================================
// SUITE 1 — default_keybindings() static
// ============================================================================
TEST_SUITE("Keybindings::default_keybindings") {

    TEST_CASE("all curated actions are present") {
        const auto defs = Keybindings::default_keybindings();
        CHECK(defs.count(ReplAction::Send)          == 1);
        CHECK(defs.count(ReplAction::Cancel)        == 1);
        CHECK(defs.count(ReplAction::CycleMode)     == 1);
        CHECK(defs.count(ReplAction::Newline)       == 1);
        CHECK(defs.count(ReplAction::HistoryUp)     == 1);
        CHECK(defs.count(ReplAction::HistoryDown)   == 1);
        CHECK(defs.count(ReplAction::Clear)         == 1);
        CHECK(defs.count(ReplAction::VimToggle)     == 1);
        CHECK(defs.count(ReplAction::HistorySearch) == 1);
    }

    TEST_CASE("cycle_mode default descriptor is Shift+Tab") {
        const auto defs = Keybindings::default_keybindings();
        REQUIRE(defs.count(ReplAction::CycleMode) == 1);
        CHECK(defs.at(ReplAction::CycleMode) == "Shift+Tab");
    }

    TEST_CASE("send default descriptor is Ctrl+Enter") {
        const auto defs = Keybindings::default_keybindings();
        REQUIRE(defs.count(ReplAction::Send) == 1);
        CHECK(defs.at(ReplAction::Send) == "Ctrl+Enter");
    }

    TEST_CASE("cancel default descriptor is Escape") {
        const auto defs = Keybindings::default_keybindings();
        REQUIRE(defs.count(ReplAction::Cancel) == 1);
        CHECK(defs.at(ReplAction::Cancel) == "Escape");
    }

    TEST_CASE("clear default descriptor is Ctrl+L") {
        const auto defs = Keybindings::default_keybindings();
        REQUIRE(defs.count(ReplAction::Clear) == 1);
        CHECK(defs.at(ReplAction::Clear) == "Ctrl+L");
    }

    TEST_CASE("history_search default descriptor is Ctrl+R") {
        const auto defs = Keybindings::default_keybindings();
        REQUIRE(defs.count(ReplAction::HistorySearch) == 1);
        CHECK(defs.at(ReplAction::HistorySearch) == "Ctrl+R");
    }

    TEST_CASE("vim_toggle default descriptor is Ctrl+G — not Escape") {
        // UI-D8 fix: VimToggle must not share Escape with Cancel.
        const auto defs = Keybindings::default_keybindings();
        REQUIRE(defs.count(ReplAction::VimToggle) == 1);
        CHECK(defs.at(ReplAction::VimToggle) == "Ctrl+G");
        // Cancel must still be Escape.
        REQUIRE(defs.count(ReplAction::Cancel) == 1);
        CHECK(defs.at(ReplAction::Cancel) == "Escape");
        // They must differ.
        CHECK(defs.at(ReplAction::VimToggle) != defs.at(ReplAction::Cancel));
    }
}

// ============================================================================
// SUITE 2 — Constructor produces working event map
// ============================================================================
TEST_SUITE("Keybindings — constructor defaults") {

    TEST_CASE("constructed Keybindings maps Shift+Tab to CycleMode") {
        Keybindings kb;
        auto action = kb.event_to_action(ftxui::Event::TabReverse);
        CHECK(action == ReplAction::CycleMode);
    }

    TEST_CASE("constructed Keybindings maps Escape to Cancel") {
        Keybindings kb;
        // Escape is bound exclusively to Cancel (VimToggle moved to Ctrl+G).
        // UI-D8 fix: no ambiguous double-binding on Escape.
        auto action = kb.event_to_action(ftxui::Event::Escape);
        CHECK(action == ReplAction::Cancel);
    }

    TEST_CASE("constructed Keybindings maps Ctrl+G to VimToggle") {
        Keybindings kb;
        // VimToggle is now bound to Ctrl+G () — resolves UI-D8 conflict.
        auto ev = ftxui::Event::Special("");
        CHECK(kb.event_to_action(ev) == ReplAction::VimToggle);
    }

    TEST_CASE("constructed Keybindings maps Ctrl+L to Clear") {
        Keybindings kb;
        auto action = kb.event_to_action(ftxui::Event::CtrlL);
        CHECK(action == ReplAction::Clear);
    }

    TEST_CASE("constructed Keybindings maps Ctrl+R to HistorySearch") {
        Keybindings kb;
        auto action = kb.event_to_action(ftxui::Event::CtrlR);
        CHECK(action == ReplAction::HistorySearch);
    }

    TEST_CASE("constructed Keybindings maps ArrowUp to HistoryUp") {
        Keybindings kb;
        auto action = kb.event_to_action(ftxui::Event::ArrowUp);
        CHECK(action == ReplAction::HistoryUp);
    }

    TEST_CASE("constructed Keybindings maps ArrowDown to HistoryDown") {
        Keybindings kb;
        auto action = kb.event_to_action(ftxui::Event::ArrowDown);
        CHECK(action == ReplAction::HistoryDown);
    }
}

// ============================================================================
// SUITE 3 — Ctrl+Enter multi-terminal aliases
// ============================================================================
TEST_SUITE("Keybindings — Ctrl+Enter multi-terminal") {

    TEST_CASE("Ctrl+Enter kitty escape-bracket 13;5u maps to Send") {
        Keybindings kb;
        auto ev = ftxui::Event::Special("\x1b[13;5u");
        CHECK(kb.event_to_action(ev) == ReplAction::Send);
    }

    TEST_CASE("Ctrl+Enter traditional \\r\\n maps to Send") {
        Keybindings kb;
        auto ev = ftxui::Event::Special("\x0d\x0a");
        CHECK(kb.event_to_action(ev) == ReplAction::Send);
    }

    TEST_CASE("Shift+Enter kitty escape-bracket 13;2u maps to Newline") {
        Keybindings kb;
        auto ev = ftxui::Event::Special("\x1b[13;2u");
        CHECK(kb.event_to_action(ev) == ReplAction::Newline);
    }
}

// ============================================================================
// SUITE 4 — apply_override replaces specific bindings
// ============================================================================
TEST_SUITE("Keybindings — apply_override") {

    TEST_CASE("override send changes it to Ctrl+S") {
        Keybindings kb;
        KeybindingMap overrides = { { "send", "Ctrl+S" } };
        kb.apply_override(overrides);

        // Ctrl+S = \x13 (ASCII 19 = 's' - 'a' + 1 = 0x13)
        auto ev = ftxui::Event::Special("\x13");
        CHECK(kb.event_to_action(ev) == ReplAction::Send);
        // Original Ctrl+Enter kitty should no longer be Send:
        auto old_ev = ftxui::Event::Special("\x1b[13;5u");
        CHECK(kb.event_to_action(old_ev) != ReplAction::Send);
    }

    TEST_CASE("override cycle_mode changes descriptor") {
        Keybindings kb;
        KeybindingMap overrides = { { "cycle_mode", "Alt+Tab" } };
        kb.apply_override(overrides);

        // Alt+Tab = \x1b\x09
        auto ev = ftxui::Event::Special("\x1b\x09");
        CHECK(kb.event_to_action(ev) == ReplAction::CycleMode);

        // Original Shift+Tab should no longer be CycleMode.
        CHECK(kb.event_to_action(ftxui::Event::TabReverse) != ReplAction::CycleMode);
    }

    TEST_CASE("key_for reflects override") {
        Keybindings kb;
        KeybindingMap overrides = { { "clear", "Ctrl+K" } };
        kb.apply_override(overrides);

        auto desc = kb.key_for(ReplAction::Clear);
        REQUIRE(desc.has_value());
        CHECK(*desc == "Ctrl+K");
    }

    TEST_CASE("unaffected actions survive override") {
        Keybindings kb;
        KeybindingMap overrides = { { "send", "Ctrl+S" } };
        kb.apply_override(overrides);

        // CycleMode still Shift+Tab.
        CHECK(kb.event_to_action(ftxui::Event::TabReverse) == ReplAction::CycleMode);
        // Clear still Ctrl+L.
        CHECK(kb.event_to_action(ftxui::Event::CtrlL) == ReplAction::Clear);
    }
}

// ============================================================================
// SUITE 5 — apply_override ignores unknown action names
// ============================================================================
TEST_SUITE("Keybindings — apply_override unknown actions") {

    TEST_CASE("unknown action name is silently ignored") {
        Keybindings kb;
        KeybindingMap overrides = { { "totally_unknown_action", "Ctrl+X" } };
        // Must not throw or crash; defaults remain intact.
        kb.apply_override(overrides);
        CHECK(kb.event_to_action(ftxui::Event::TabReverse) == ReplAction::CycleMode);
    }
}

// ============================================================================
// SUITE 6 — apply_override keeps previous binding on bad descriptor
// ============================================================================
TEST_SUITE("Keybindings — apply_override bad descriptor") {

    TEST_CASE("unrecognised descriptor keeps previous binding") {
        Keybindings kb;
        // "XYZ+Enter" has an unrecognised modifier token.
        KeybindingMap overrides = { { "send", "XYZ+Enter" } };
        kb.apply_override(overrides);

        // Send binding should still be Ctrl+Enter (kitty).
        auto ev = ftxui::Event::Special("\x1b[13;5u");
        CHECK(kb.event_to_action(ev) == ReplAction::Send);
    }
}

// ============================================================================
// SUITE 7 — key_for and descriptor_map
// ============================================================================
TEST_SUITE("Keybindings — key_for + descriptor_map") {

    TEST_CASE("key_for returns descriptor for bound action") {
        Keybindings kb;
        auto desc = kb.key_for(ReplAction::CycleMode);
        REQUIRE(desc.has_value());
        CHECK(*desc == "Shift+Tab");
    }

    TEST_CASE("key_for returns nullopt for None action") {
        Keybindings kb;
        auto desc = kb.key_for(ReplAction::None);
        CHECK_FALSE(desc.has_value());
    }

    TEST_CASE("descriptor_map contains all default actions") {
        Keybindings kb;
        auto dm = kb.descriptor_map();
        CHECK(dm.count(ReplAction::Send)          == 1);
        CHECK(dm.count(ReplAction::Cancel)        == 1);
        CHECK(dm.count(ReplAction::CycleMode)     == 1);
        CHECK(dm.count(ReplAction::HistorySearch) == 1);
    }

    TEST_CASE("descriptor_map snapshot is independent of Keybindings") {
        Keybindings kb;
        auto dm1 = kb.descriptor_map();

        KeybindingMap overrides = { { "clear", "Ctrl+K" } };
        kb.apply_override(overrides);

        // Original snapshot unchanged.
        CHECK(dm1.at(ReplAction::Clear) == "Ctrl+L");

        // New snapshot reflects override.
        auto dm2 = kb.descriptor_map();
        CHECK(dm2.at(ReplAction::Clear) == "Ctrl+K");
    }
}

// ============================================================================
// SUITE 8 — event_to_action returns None for unbound events
// ============================================================================
TEST_SUITE("Keybindings — None for unbound events") {

    TEST_CASE("unbound character event returns None") {
        Keybindings kb;
        auto ev = ftxui::Event::Character('z');
        CHECK(kb.event_to_action(ev) == ReplAction::None);
    }

    TEST_CASE("mouse event returns None") {
        Keybindings kb;
        // Construct a minimal mouse event.
        ftxui::Mouse m{};
        m.button = ftxui::Mouse::Left;
        m.motion = ftxui::Mouse::Pressed;
        m.x = 0; m.y = 0;
        auto ev = ftxui::Event::Mouse("", m);
        CHECK(kb.event_to_action(ev) == ReplAction::None);
    }

    TEST_CASE("arbitrary special event returns None") {
        Keybindings kb;
        auto ev = ftxui::Event::Special("\x1b[999~");
        CHECK(kb.event_to_action(ev) == ReplAction::None);
    }
}

// ============================================================================
// SUITE 9 — Integration with config::load_keybindings
// ============================================================================
TEST_SUITE("Keybindings — config loader integration") {

    TEST_CASE("apply_override with default_keybindings map leaves bindings unchanged") {
        Keybindings kb;
        // Start from scratch.
        Keybindings kb2;
        // Apply the string-map defaults from KeybindingsConfig to kb2.
        kb2.apply_override(batbox::config::default_keybindings());

        // Both should map Shift+Tab to CycleMode.
        CHECK(kb.event_to_action(ftxui::Event::TabReverse)
              == kb2.event_to_action(ftxui::Event::TabReverse));
        CHECK(kb.event_to_action(ftxui::Event::CtrlL)
              == kb2.event_to_action(ftxui::Event::CtrlL));
    }
}
