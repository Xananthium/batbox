// tests/unit/test_vim_mode.cpp
// ---------------------------------------------------------------------------
// doctest suite for batbox::repl::VimMode.
//
// Build + run (standalone, no CMake needed):
//
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_vim_mode.cpp \
//       src/repl/VimMode.cpp \
//       -o /tmp/test_vimmode && /tmp/test_vimmode
//
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/repl/VimMode.hpp>

#include <string>
#include <string_view>

using namespace batbox::repl;

// ---------------------------------------------------------------------------
// Helper: apply a sequence of keys to a buffer, returning the final buffer
// and cursor position.
// ---------------------------------------------------------------------------
struct BufState {
    std::string buf;
    std::size_t cursor{0};

    // Apply one key via handle_key (mutating form) — simplifies tests.
    VimAction apply(VimMode& vm, std::string_view key) {
        return vm.handle_key(key, buf, cursor);
    }
};

// ============================================================================
// SUITE 1: enabled / disabled
// ============================================================================
TEST_SUITE("VimMode — enabled/disabled") {

    TEST_CASE("disabled by default — all keys passthrough") {
        VimMode vm;
        BufState s{"hello", 0};
        auto act = s.apply(vm, "h");
        CHECK(act.kind == VimActionKind::Passthrough);
    }

    TEST_CASE("enable/disable toggle") {
        VimMode vm;
        vm.set_enabled(true);
        CHECK(vm.is_enabled());
        vm.toggle();
        CHECK_FALSE(vm.is_enabled());
        vm.toggle();
        CHECK(vm.is_enabled());
    }

    TEST_CASE("set_enabled starts in Insert mode") {
        VimMode vm;
        vm.set_enabled(true);
        CHECK(vm.mode() == VimModeState::Insert);
    }

    TEST_CASE("disabled after set_enabled(false) — passthrough again") {
        VimMode vm;
        vm.set_enabled(true);
        vm.set_enabled(false);
        BufState s{"hello", 0};
        auto act = s.apply(vm, "h");
        CHECK(act.kind == VimActionKind::Passthrough);
    }
}

// ============================================================================
// SUITE 2: Insert mode basics
// ============================================================================
TEST_SUITE("VimMode — Insert mode") {

    TEST_CASE("printable key in Insert mode → Passthrough") {
        VimMode vm;
        vm.set_enabled(true);
        BufState s{"", 0};
        auto act = s.apply(vm, "a");
        CHECK(act.kind == VimActionKind::Passthrough);
    }

    TEST_CASE("Esc in Insert mode → Normal mode") {
        VimMode vm;
        vm.set_enabled(true);
        BufState s{"hello", 3};
        auto act = s.apply(vm, "\x1b");
        CHECK(act.kind == VimActionKind::ChangeMode);
        CHECK(act.new_mode == VimModeState::Normal);
        CHECK(vm.mode() == VimModeState::Normal);
    }

    TEST_CASE("Enter in Insert mode → SendLine") {
        VimMode vm;
        vm.set_enabled(true);
        BufState s{"hello", 5};
        auto act = s.apply(vm, "\n");
        CHECK(act.kind == VimActionKind::SendLine);
    }

    TEST_CASE("mode_indicator shows INSERT in Insert mode") {
        VimMode vm;
        vm.set_enabled(true);
        CHECK(vm.mode_indicator() == "-- INSERT --");
    }
}

// ============================================================================
// SUITE 3: Normal mode — mode transitions
// ============================================================================
TEST_SUITE("VimMode — Normal mode transitions") {

    auto enter_normal = [](VimMode& vm, BufState& s) {
        vm.set_enabled(true);
        s.apply(vm, "\x1b");  // Esc: Insert → Normal
        // In case we were already in Normal (second Esc is noop):
        // explicitly set mode
    };

    TEST_CASE("i enters Insert mode") {
        VimMode vm;
        BufState s{"hello", 2};
        vm.set_enabled(true);
        s.apply(vm, "\x1b");
        auto act = s.apply(vm, "i");
        CHECK(act.kind == VimActionKind::ChangeMode);
        CHECK(act.new_mode == VimModeState::Insert);
        CHECK(vm.mode() == VimModeState::Insert);
    }

    TEST_CASE("a enters Insert mode with cursor+1") {
        VimMode vm;
        BufState s{"hello", 2};
        vm.set_enabled(true);
        s.apply(vm, "\x1b");
        auto act = s.apply(vm, "a");
        CHECK(act.kind == VimActionKind::ChangeMode);
        CHECK(act.new_mode == VimModeState::Insert);
        CHECK(act.cursor_pos == 3);
    }

    TEST_CASE("A enters Insert mode at end of line") {
        VimMode vm;
        BufState s{"hello", 2};
        vm.set_enabled(true);
        s.apply(vm, "\x1b");
        auto act = s.apply(vm, "A");
        CHECK(act.kind == VimActionKind::ChangeMode);
        CHECK(act.cursor_pos == s.buf.size());
    }

    TEST_CASE("I enters Insert mode at first non-blank") {
        VimMode vm;
        BufState s{"  hello", 5};
        vm.set_enabled(true);
        s.apply(vm, "\x1b");
        auto act = s.apply(vm, "I");
        CHECK(act.kind == VimActionKind::ChangeMode);
        CHECK(act.cursor_pos == 2);  // first non-space
    }

    TEST_CASE("v enters Visual mode") {
        VimMode vm;
        BufState s{"hello", 1};
        vm.set_enabled(true);
        s.apply(vm, "\x1b");
        auto act = s.apply(vm, "v");
        CHECK(act.kind == VimActionKind::ChangeMode);
        CHECK(act.new_mode == VimModeState::Visual);
        CHECK(vm.mode() == VimModeState::Visual);
    }

    TEST_CASE("mode_indicator shows NORMAL") {
        VimMode vm;
        BufState s{"hello", 0};
        vm.set_enabled(true);
        s.apply(vm, "\x1b");
        CHECK(vm.mode_indicator() == "-- NORMAL --");
    }
}

// ============================================================================
// SUITE 4: Normal mode — hjkl motions
// ============================================================================
TEST_SUITE("VimMode — hjkl motions") {

    auto make_normal_vm = [](BufState& s) {
        VimMode vm;
        vm.set_enabled(true);
        s.apply(vm, "\x1b");
        return vm;
    };

    TEST_CASE("h moves cursor left") {
        BufState s{"hello", 3};
        auto vm = make_normal_vm(s);
        auto act = s.apply(vm, "h");
        CHECK(act.kind == VimActionKind::MoveCursor);
        CHECK(act.cursor_pos == 2);
        CHECK(s.cursor == 2);
    }

    TEST_CASE("h at position 0 stays at 0") {
        BufState s{"hello", 0};
        auto vm = make_normal_vm(s);
        auto act = s.apply(vm, "h");
        CHECK(act.cursor_pos == 0);
        CHECK(s.cursor == 0);
    }

    TEST_CASE("l moves cursor right") {
        BufState s{"hello", 1};
        auto vm = make_normal_vm(s);
        auto act = s.apply(vm, "l");
        CHECK(act.kind == VimActionKind::MoveCursor);
        CHECK(act.cursor_pos == 2);
        CHECK(s.cursor == 2);
    }

    TEST_CASE("l at end of buffer stays at end-1") {
        BufState s{"hello", 4};  // "hello" len=5, last valid Normal pos = 4
        auto vm = make_normal_vm(s);
        auto act = s.apply(vm, "l");
        CHECK(act.cursor_pos == 4);
    }

    TEST_CASE("j and k are noops for single-line REPL") {
        BufState s{"hello", 2};
        auto vm = make_normal_vm(s);
        CHECK(s.apply(vm, "j").kind == VimActionKind::NoOp);
        CHECK(s.apply(vm, "k").kind == VimActionKind::NoOp);
    }

    TEST_CASE("count prefix 3h moves left 3") {
        BufState s{"hello world", 5};
        auto vm = make_normal_vm(s);
        s.apply(vm, "3");
        auto act = s.apply(vm, "h");
        CHECK(act.cursor_pos == 2);
    }

    TEST_CASE("count prefix 2l moves right 2") {
        BufState s{"hello world", 1};
        auto vm = make_normal_vm(s);
        s.apply(vm, "2");
        auto act = s.apply(vm, "l");
        CHECK(act.cursor_pos == 3);
    }
}

// ============================================================================
// SUITE 5: Normal mode — line-start / line-end motions
// ============================================================================
TEST_SUITE("VimMode — 0 and $ motions") {

    auto make_normal_vm = [](BufState& s) {
        VimMode vm;
        vm.set_enabled(true);
        s.apply(vm, "\x1b");
        return vm;
    };

    TEST_CASE("0 moves to start of line") {
        BufState s{"hello world", 7};
        auto vm = make_normal_vm(s);
        auto act = s.apply(vm, "0");
        CHECK(act.kind == VimActionKind::MoveCursor);
        CHECK(act.cursor_pos == 0);
        CHECK(s.cursor == 0);
    }

    TEST_CASE("$ moves to last char") {
        BufState s{"hello", 0};
        auto vm = make_normal_vm(s);
        auto act = s.apply(vm, "$");
        CHECK(act.kind == VimActionKind::MoveCursor);
        CHECK(act.cursor_pos == 4);  // 'o' at index 4
        CHECK(s.cursor == 4);
    }

    TEST_CASE("0 on empty buffer stays at 0") {
        BufState s{"", 0};
        auto vm = make_normal_vm(s);
        auto act = s.apply(vm, "0");
        CHECK(act.cursor_pos == 0);
    }

    TEST_CASE("$ on empty buffer stays at 0") {
        BufState s{"", 0};
        auto vm = make_normal_vm(s);
        auto act = s.apply(vm, "$");
        CHECK(act.cursor_pos == 0);
    }
}

// ============================================================================
// SUITE 6: Normal mode — word motions w/b/e
// ============================================================================
TEST_SUITE("VimMode — word motions w/b/e") {

    auto make_normal_vm = [](BufState& s) {
        VimMode vm;
        vm.set_enabled(true);
        s.apply(vm, "\x1b");
        return vm;
    };

    TEST_CASE("w moves to next word start") {
        BufState s{"hello world foo", 0};
        auto vm = make_normal_vm(s);
        auto act = s.apply(vm, "w");
        CHECK(act.cursor_pos == 6);  // 'w' of "world"
    }

    TEST_CASE("w from middle of word moves to next word start") {
        BufState s{"hello world foo", 3};
        auto vm = make_normal_vm(s);
        auto act = s.apply(vm, "w");
        CHECK(act.cursor_pos == 6);
    }

    TEST_CASE("b moves to start of previous word") {
        BufState s{"hello world foo", 6};
        auto vm = make_normal_vm(s);
        auto act = s.apply(vm, "b");
        CHECK(act.cursor_pos == 0);
    }

    TEST_CASE("e moves to end of current word") {
        BufState s{"hello world", 0};
        auto vm = make_normal_vm(s);
        auto act = s.apply(vm, "e");
        CHECK(act.cursor_pos == 4);  // 'o' of "hello"
    }

    TEST_CASE("2w skips two words") {
        BufState s{"one two three", 0};
        auto vm = make_normal_vm(s);
        s.apply(vm, "2");
        auto act = s.apply(vm, "w");
        CHECK(act.cursor_pos == 8);  // 't' of "three"
    }

    TEST_CASE("2b skips two words backward") {
        BufState s{"one two three", 8};
        auto vm = make_normal_vm(s);
        s.apply(vm, "2");
        auto act = s.apply(vm, "b");
        CHECK(act.cursor_pos == 0);
    }
}

// ============================================================================
// SUITE 7: Normal mode — gg and G
// ============================================================================
TEST_SUITE("VimMode — gg and G") {

    auto make_normal_vm = [](BufState& s) {
        VimMode vm;
        vm.set_enabled(true);
        s.apply(vm, "\x1b");
        return vm;
    };

    TEST_CASE("gg moves to start") {
        BufState s{"hello world", 7};
        auto vm = make_normal_vm(s);
        auto act = s.apply(vm, "gg");
        CHECK(act.kind == VimActionKind::MoveCursor);
        CHECK(act.cursor_pos == 0);
    }

    TEST_CASE("G moves to last char") {
        BufState s{"hello", 0};
        auto vm = make_normal_vm(s);
        auto act = s.apply(vm, "G");
        CHECK(act.cursor_pos == 4);
    }
}

// ============================================================================
// SUITE 8: Normal mode — x (delete char)
// ============================================================================
TEST_SUITE("VimMode — x delete char") {

    auto make_normal_vm = [](BufState& s) {
        VimMode vm;
        vm.set_enabled(true);
        s.apply(vm, "\x1b");
        return vm;
    };

    TEST_CASE("x deletes char under cursor") {
        BufState s{"hello", 1};
        auto vm = make_normal_vm(s);
        auto act = s.apply(vm, "x");
        CHECK(act.kind == VimActionKind::DeleteRange);
        CHECK(act.start == 1);
        CHECK(act.end == 2);
        CHECK(s.buf == "hllo");
    }

    TEST_CASE("x on empty buffer is noop") {
        BufState s{"", 0};
        auto vm = make_normal_vm(s);
        auto act = s.apply(vm, "x");
        CHECK(act.kind == VimActionKind::NoOp);
    }

    TEST_CASE("x yanks into yank buffer") {
        BufState s{"hello", 2};
        auto vm = make_normal_vm(s);
        s.apply(vm, "x");
        CHECK(vm.yank_buffer() == "l");
    }

    TEST_CASE("3x deletes 3 chars") {
        BufState s{"hello world", 0};
        auto vm = make_normal_vm(s);
        s.apply(vm, "3");
        auto act = s.apply(vm, "x");
        CHECK(act.kind == VimActionKind::DeleteRange);
        CHECK(act.start == 0);
        CHECK(act.end == 3);
        CHECK(s.buf == "lo world");
    }
}

// ============================================================================
// SUITE 9: Normal mode — dd / yy / p
// ============================================================================
TEST_SUITE("VimMode — dd yy p") {

    auto make_normal_vm = [](BufState& s) {
        VimMode vm;
        vm.set_enabled(true);
        s.apply(vm, "\x1b");
        return vm;
    };

    TEST_CASE("dd clears entire buffer") {
        BufState s{"hello world", 5};
        auto vm = make_normal_vm(s);
        auto act = s.apply(vm, "dd");
        CHECK(act.kind == VimActionKind::ClearLine);
        CHECK(s.buf.empty());
        CHECK(s.cursor == 0);
    }

    TEST_CASE("dd saves to yank buffer") {
        BufState s{"hello", 0};
        auto vm = make_normal_vm(s);
        s.apply(vm, "dd");
        CHECK(vm.yank_buffer() == "hello");
    }

    TEST_CASE("yy saves buffer to yank without deleting") {
        BufState s{"hello", 2};
        auto vm = make_normal_vm(s);
        auto act = s.apply(vm, "yy");
        CHECK(act.kind == VimActionKind::NoOp);
        CHECK(s.buf == "hello");
        CHECK(vm.yank_buffer() == "hello");
    }

    TEST_CASE("p pastes yank buffer after cursor") {
        // Start with "helo", cursor at 1 ('e'). x deletes 'e', buf="hlo", cursor=1.
        // p pastes 'e' after cursor (after 'l' at pos 1) → insert at pos 2 → "hloe"... 
        // Better: start with "helo" cursor at 0. x deletes 'h', buf="elo", cursor=0.
        // p pastes 'h' after cursor (after 'e' at pos 0) → insert at pos 1 → "ehlo".
        // Even better: test with known start state.
        // "abc" cursor=0. x deletes 'a', buf="bc", cursor=0, yank="a".
        // p pastes 'a' after cursor: cursor=0 ('b'), ins_pos=min(1,2)=1 → "bac".
        BufState s{"abc", 0};
        auto vm = make_normal_vm(s);
        s.apply(vm, "x");   // deletes 'a', yank="a", buf="bc", cursor=0
        CHECK(s.buf == "bc");
        CHECK(vm.yank_buffer() == "a");
        // p pastes 'a' after 'b' at cursor=0 → insert at pos 1 → "bac"
        auto act = s.apply(vm, "p");
        CHECK(act.kind == VimActionKind::ReplaceRange);
        CHECK(s.buf == "bac");
    }

    TEST_CASE("P pastes yank buffer before cursor") {
        BufState s{"heo", 2};
        auto vm = make_normal_vm(s);
        // Set up yank buffer
        s.apply(vm, "0");    // move to start
        BufState s2{"l", 0};
        VimMode vm2;
        vm2.set_enabled(true);
        s2.apply(vm2, "\x1b");
        s2.apply(vm2, "yy");  // yank "l" — actually yy yanks whole buf
        CHECK(vm2.yank_buffer() == "l");

        BufState s3{"heo", 2};
        VimMode vm3;
        vm3.set_enabled(true);
        s3.apply(vm3, "\x1b");
        s3.apply(vm3, "x");  // yank 'o' — but test P with manually set yank
        // vm3 yank is now "o" after x at pos 2
        CHECK(vm3.yank_buffer() == "o");
        auto act = s3.apply(vm3, "P");
        CHECK(act.kind == VimActionKind::ReplaceRange);
    }
}

// ============================================================================
// SUITE 10: Normal mode — d{motion} operator
// ============================================================================
TEST_SUITE("VimMode — d{motion} operator") {

    auto make_normal_vm = [](BufState& s) {
        VimMode vm;
        vm.set_enabled(true);
        s.apply(vm, "\x1b");
        return vm;
    };

    TEST_CASE("dw deletes from cursor to next word start") {
        BufState s{"hello world", 0};
        auto vm = make_normal_vm(s);
        s.apply(vm, "d");
        auto act = s.apply(vm, "w");
        CHECK(act.kind == VimActionKind::DeleteRange);
        // deletes "hello " (positions 0..6)
        CHECK(s.buf == "world");
    }

    TEST_CASE("db deletes backward to previous word start") {
        BufState s{"hello world", 6};
        auto vm = make_normal_vm(s);
        s.apply(vm, "d");
        auto act = s.apply(vm, "b");
        CHECK(act.kind == VimActionKind::DeleteRange);
        CHECK(s.buf == "world");
    }

    TEST_CASE("d$ deletes to end of line") {
        BufState s{"hello world", 5};
        auto vm = make_normal_vm(s);
        s.apply(vm, "d");
        auto act = s.apply(vm, "$");
        CHECK(act.kind == VimActionKind::DeleteRange);
        CHECK(s.buf == "hello");
    }

    TEST_CASE("d0 deletes to start of line") {
        // cursor at 6 ('w'): d0 deletes buf[0..6) = "hello ", leaving "world"
        BufState s{"hello world", 6};
        auto vm = make_normal_vm(s);
        s.apply(vm, "d");
        auto act = s.apply(vm, "0");
        CHECK(act.kind == VimActionKind::DeleteRange);
        CHECK(s.buf == "world");
    }
}

// ============================================================================
// SUITE 11: Normal mode — y{motion} and c{motion} operators
// ============================================================================
TEST_SUITE("VimMode — y{motion} and c{motion} operators") {

    auto make_normal_vm = [](BufState& s) {
        VimMode vm;
        vm.set_enabled(true);
        s.apply(vm, "\x1b");
        return vm;
    };

    TEST_CASE("yw yanks word to yank buffer without deleting") {
        BufState s{"hello world", 0};
        auto vm = make_normal_vm(s);
        s.apply(vm, "y");
        auto act = s.apply(vm, "w");
        CHECK(act.kind == VimActionKind::NoOp);
        CHECK(s.buf == "hello world");
        CHECK(vm.yank_buffer() == "hello ");
    }

    TEST_CASE("cw changes word: delete + enter Insert") {
        BufState s{"hello world", 0};
        auto vm = make_normal_vm(s);
        s.apply(vm, "c");
        auto act = s.apply(vm, "w");
        CHECK(act.kind == VimActionKind::ReplaceRange);
        CHECK(act.text.empty());
        CHECK(vm.mode() == VimModeState::Insert);
    }

    TEST_CASE("cc clears line and enters Insert") {
        BufState s{"hello world", 3};
        auto vm = make_normal_vm(s);
        s.apply(vm, "c");
        auto act = s.apply(vm, "c");
        CHECK(vm.mode() == VimModeState::Insert);
        CHECK(s.buf.empty());
    }
}

// ============================================================================
// SUITE 12: Normal mode — text objects iw / aw / i" / a"
// ============================================================================
TEST_SUITE("VimMode — text objects") {

    auto make_normal_vm = [](BufState& s) {
        VimMode vm;
        vm.set_enabled(true);
        s.apply(vm, "\x1b");
        return vm;
    };

    TEST_CASE("diw deletes inner word") {
        BufState s{"hello world", 1};
        auto vm = make_normal_vm(s);
        s.apply(vm, "d");
        s.apply(vm, "i");
        auto act = s.apply(vm, "w");
        CHECK(act.kind == VimActionKind::DeleteRange);
        CHECK(act.start == 0);
        CHECK(act.end == 5);
        CHECK(s.buf == " world");
    }

    TEST_CASE("daw deletes word + trailing space") {
        BufState s{"hello world", 1};
        auto vm = make_normal_vm(s);
        s.apply(vm, "d");
        s.apply(vm, "a");
        auto act = s.apply(vm, "w");
        CHECK(act.kind == VimActionKind::DeleteRange);
        // "hello " → positions 0..6
        CHECK(act.start == 0);
        CHECK(act.end == 6);
        CHECK(s.buf == "world");
    }

    TEST_CASE("di\" deletes content inside double quotes") {
        BufState s{"say \"hello\" there", 6};
        auto vm = make_normal_vm(s);
        s.apply(vm, "d");
        s.apply(vm, "i");
        auto act = s.apply(vm, "\"");
        CHECK(act.kind == VimActionKind::DeleteRange);
        CHECK(s.buf == "say \"\" there");
    }

    TEST_CASE("yiw yanks inner word") {
        BufState s{"hello world", 2};
        auto vm = make_normal_vm(s);
        s.apply(vm, "y");
        s.apply(vm, "i");
        s.apply(vm, "w");
        CHECK(vm.yank_buffer() == "hello");
    }

    TEST_CASE("ciw changes inner word and enters Insert") {
        BufState s{"hello world", 2};
        auto vm = make_normal_vm(s);
        s.apply(vm, "c");
        s.apply(vm, "i");
        s.apply(vm, "w");
        CHECK(vm.mode() == VimModeState::Insert);
        CHECK(s.buf == " world");
    }
}

// ============================================================================
// SUITE 13: Normal mode — ~ toggle case
// ============================================================================
TEST_SUITE("VimMode — ~ toggle case") {

    auto make_normal_vm = [](BufState& s) {
        VimMode vm;
        vm.set_enabled(true);
        s.apply(vm, "\x1b");
        return vm;
    };

    TEST_CASE("~ toggles lowercase to uppercase") {
        BufState s{"hello", 0};
        auto vm = make_normal_vm(s);
        auto act = s.apply(vm, "~");
        CHECK(act.kind == VimActionKind::ReplaceRange);
        CHECK(s.buf == "Hello");
    }

    TEST_CASE("~ toggles uppercase to lowercase") {
        BufState s{"Hello", 0};
        auto vm = make_normal_vm(s);
        auto act = s.apply(vm, "~");
        CHECK(act.kind == VimActionKind::ReplaceRange);
        CHECK(s.buf == "hello");
    }

    TEST_CASE("~ on non-alpha is noop (no crash, no change)") {
        BufState s{"h3llo", 1};
        auto vm = make_normal_vm(s);
        auto act = s.apply(vm, "~");
        // digit → no case change, but action is still ReplaceRange (same char)
        CHECK(s.buf == "h3llo");
    }
}

// ============================================================================
// SUITE 14: Visual mode
// ============================================================================
TEST_SUITE("VimMode — Visual mode") {

    auto make_visual_vm = [](BufState& s, std::size_t anchor) {
        VimMode vm;
        vm.set_enabled(true);
        s.apply(vm, "\x1b");
        s.cursor = anchor;
        s.apply(vm, "v");
        return vm;
    };

    TEST_CASE("Esc exits Visual → Normal") {
        BufState s{"hello", 2};
        auto vm = make_visual_vm(s, 2);
        auto act = s.apply(vm, "\x1b");
        CHECK(act.kind == VimActionKind::ChangeMode);
        CHECK(act.new_mode == VimModeState::Normal);
    }

    TEST_CASE("h moves cursor left in Visual") {
        BufState s{"hello", 3};
        auto vm = make_visual_vm(s, 3);
        auto act = s.apply(vm, "h");
        CHECK(act.kind == VimActionKind::MoveCursor);
        CHECK(act.cursor_pos == 2);
    }

    TEST_CASE("l moves cursor right in Visual") {
        BufState s{"hello", 1};
        auto vm = make_visual_vm(s, 1);
        auto act = s.apply(vm, "l");
        CHECK(act.cursor_pos == 2);
    }

    TEST_CASE("d deletes selected range in Visual") {
        BufState s{"hello world", 0};
        auto vm = make_visual_vm(s, 0);
        // Extend selection to pos 4 (select "hello")
        s.apply(vm, "l"); s.apply(vm, "l");
        s.apply(vm, "l"); s.apply(vm, "l");
        auto act = s.apply(vm, "d");
        CHECK(act.kind == VimActionKind::DeleteRange);
        CHECK(vm.mode() == VimModeState::Normal);
    }

    TEST_CASE("y yanks selected range and enters Normal") {
        BufState s{"hello world", 0};
        auto vm = make_visual_vm(s, 0);
        s.apply(vm, "l"); s.apply(vm, "l");
        s.apply(vm, "l"); s.apply(vm, "l");
        auto act = s.apply(vm, "y");
        CHECK(vm.mode() == VimModeState::Normal);
        CHECK(vm.yank_buffer() == "hello");
    }

    TEST_CASE("mode_indicator shows VISUAL") {
        BufState s{"hello", 2};
        auto vm = make_visual_vm(s, 2);
        CHECK(vm.mode_indicator() == "-- VISUAL --");
    }
}

// ============================================================================
// SUITE 15: reset()
// ============================================================================
TEST_SUITE("VimMode — reset") {

    TEST_CASE("reset clears pending operator and reverts to Insert") {
        VimMode vm;
        vm.set_enabled(true);
        BufState s{"hello", 2};
        s.apply(vm, "\x1b");  // Normal
        s.apply(vm, "d");     // pending operator = 'd'
        vm.reset();
        CHECK(vm.mode() == VimModeState::Insert);
    }
}

// ============================================================================
// SUITE 16: process_key (non-mutating API)
// ============================================================================
TEST_SUITE("VimMode — process_key non-mutating API") {

    TEST_CASE("process_key does not mutate buffer") {
        VimMode vm;
        vm.set_enabled(true);
        std::string buf = "hello";
        std::size_t cur = 2;
        // Enter Normal
        vm.process_key("\x1b", buf, cur);
        // Move
        std::string orig = buf;
        auto act = vm.process_key("h", buf, cur);
        CHECK(buf == orig);  // buffer unchanged
        CHECK(act.kind == VimActionKind::MoveCursor);
    }
}

// ============================================================================
// SUITE 17: Table-driven integration — full key sequences
// ============================================================================
TEST_SUITE("VimMode — table-driven integration") {

    struct Case {
        std::string  label;
        std::string  initial_buf;
        std::size_t  initial_cursor;
        std::vector<std::string> keys;
        std::string  expected_buf;
        std::size_t  expected_cursor;
    };

    auto run_case = [](const Case& tc) {
        VimMode vm;
        vm.set_enabled(true);
        BufState s{tc.initial_buf, tc.initial_cursor};
        for (auto& key : tc.keys) {
            s.apply(vm, key);
        }
        return s;
    };

    TEST_CASE("table-driven key sequences") {
        // Enter Normal with Esc, then perform operation.
        // All sequences start with \x1b to enter Normal mode.
        std::vector<Case> cases = {
            {
                "h from middle",
                "hello", 3,
                {"\x1b", "h"},
                "hello", 2
            },
            {
                "l to end and clamped",
                "hello", 4,
                {"\x1b", "l"},
                "hello", 4  // already at end in Normal
            },
            {
                "0 from middle",
                "hello world", 7,
                {"\x1b", "0"},
                "hello world", 0
            },
            {
                "$ to last char",
                "hello", 0,
                {"\x1b", "$"},
                "hello", 4
            },
            {
                "x deletes char",
                "hello", 2,
                {"\x1b", "x"},
                "helo", 2
            },
            {
                "dd clears",
                "hello world", 3,
                {"\x1b", "dd"},
                "", 0
            },
            {
                "gg goes to start",
                "hello world", 8,
                {"\x1b", "gg"},
                "hello world", 0
            },
            {
                "G goes to end",
                "hello", 0,
                {"\x1b", "G"},
                "hello", 4
            },
            {
                "i then type (passthrough)",
                "hello", 2,
                {"\x1b", "i"},
                "hello", 2   // i just changes mode; typing would be passthrough
            },
            {
                "A positions at end",
                "hello", 0,
                {"\x1b", "A"},
                "hello", 5  // A sets cursor_pos=buf.size() (Insert mode)
            },
        };

        for (auto& tc : cases) {
            CAPTURE(tc.label);
            auto s = run_case(tc);
            CHECK(s.buf    == tc.expected_buf);
            CHECK(s.cursor == tc.expected_cursor);
        }
    }
}
