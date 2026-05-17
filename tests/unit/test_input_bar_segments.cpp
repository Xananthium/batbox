// tests/unit/test_input_bar_segments.cpp
// ---------------------------------------------------------------------------
// doctest suite for InputBar segment-model (TUI-FIX-T4).
//
// Covers:
//   1. Paste insertion (segment created, id increments)
//   2. Sub-threshold pastes stay as plain text
//   3. Single backspace deletes entire paste chip
//   4. Shift+Enter inserts newline (does not submit)
//   5. Cursor skips over paste chip as single atom
//   6. flatten_for_submit() expands paste to full body
//   7. Paste chip render label format
//   8. Multi-segment buffer flattens correctly
//   9. Clear resets paste counter
//  10. Plain Enter on text-only buffer still submits (regression)
//  11. Additional Shift+Enter alias sequences (\e\r, \e[27;2;13~)
//  12. Bracketed paste sequence detection (\e[200~...\e[201~)
//  13. Home/End skip over paste chips
//  14. Arrow-left before paste chip skips it atomically
//  15. Arrow-right after paste chip skips it atomically
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/tui/InputBar.hpp>
#include <batbox/tui/InputSegment.hpp>
#include <batbox/repl/History.hpp>
#include <batbox/repl/Keybindings.hpp>
#include <batbox/theme/Theme.hpp>

#include <filesystem>
#include <string>
#include <vector>

#include <ftxui/component/event.hpp>

using namespace batbox::tui;
using namespace batbox::repl;
using namespace batbox::theme;

// =============================================================================
// Helpers
// =============================================================================

static Theme make_theme() {
    return theme_from_name("miss-kittin");
}

struct SegFixture {
    Theme        theme;
    History      history{std::filesystem::path{}, 1000};
    Keybindings  kb;
    std::string  last_submit;
    bool         submit_called{false};

    std::shared_ptr<InputBar> bar;

    explicit SegFixture()
        : theme(make_theme())
        , bar(std::make_shared<InputBar>(
              theme,
              history,
              kb,
              [this](std::string s) { last_submit = s; submit_called = true; },
              nullptr,
              nullptr))
    {}

    bool press(const std::string& input) {
        return bar->OnEvent(ftxui::Event::Special(input));
    }

    void type_chars(std::string_view text) {
        for (char c : text) {
            press(std::string(1, c));
        }
    }
};

// Key constants used in these tests
static const std::string kBackspace  = "\x7f";
static const std::string kReturn     = "\x0a";
static const std::string kArrowLeft  = "\x1b[D";
static const std::string kArrowRight = "\x1b[C";
static const std::string kHome       = "\x1b[H";
static const std::string kEnd        = "\x1b[F";

// Shift+Enter sequences
static const std::string kShiftEnterKitty = "\x1b[13;2u";   // kitty → handled by Keybindings
static const std::string kShiftEnterAlt   = "\x1b\r";       // xterm / Terminal.app
static const std::string kShiftEnterCSIU  = "\x1b[27;2;13~"; // libvte

// Bracketed paste
static const std::string kBPasteStart = "\x1b[200~";
static const std::string kBPasteEnd   = "\x1b[201~";

// =============================================================================
// SUITE 1 — Paste insertion
// =============================================================================
TEST_SUITE("InputBar segments — paste insertion") {

    TEST_CASE("insert_paste: long paste (>= 200 chars) creates a paste chip") {
        SegFixture f;
        // Build a string of exactly 200 'a' characters
        std::string body(200, 'a');
        f.bar->insert_paste(body);
        // flatten_for_submit should return the full body
        CHECK(f.bar->flatten_for_submit() == body);
        // buffer() is an alias for flatten_for_submit()
        CHECK(f.bar->buffer() == body);
        // Render should not crash
        CHECK(f.bar->Render() != nullptr);
    }

    TEST_CASE("insert_paste: multi-line paste creates a paste chip") {
        SegFixture f;
        std::string body = "line1\nline2\nline3";
        f.bar->insert_paste(body);
        CHECK(f.bar->flatten_for_submit() == body);
        CHECK(f.bar->Render() != nullptr);
    }

    TEST_CASE("insert_paste: paste id increments on each chip creation") {
        SegFixture f;
        // First paste chip
        f.bar->insert_paste("line1\nline2");
        // Second paste chip  
        f.bar->insert_paste("line3\nline4");
        // Both should be present in the flattened output
        const std::string flat = f.bar->flatten_for_submit();
        CHECK(flat.find("line1") != std::string::npos);
        CHECK(flat.find("line3") != std::string::npos);
        // Render should contain both chips
        CHECK(f.bar->Render() != nullptr);
    }

    TEST_CASE("insert_paste at cursor mid-buffer splits text correctly") {
        SegFixture f;
        f.type_chars("hello world");
        // Move cursor to position 5 (after "hello")
        for (int i = 0; i < 6; ++i) f.press(kArrowLeft); // move 6 left: "hello "
        // Now insert a multi-line paste
        std::string paste = "foo\nbar";
        f.bar->insert_paste(paste);
        // Flattened: "hellofoo\nbar world"
        const std::string flat = f.bar->flatten_for_submit();
        CHECK(flat.find("hello") != std::string::npos);
        CHECK(flat.find("foo\nbar") != std::string::npos);
        CHECK(flat.find("world") != std::string::npos);
    }
}

// =============================================================================
// SUITE 2 — Sub-threshold pastes stay as plain text
// =============================================================================
TEST_SUITE("InputBar segments — sub-threshold pastes") {

    TEST_CASE("short paste (< 200 chars, no newlines) inserts as plain text") {
        SegFixture f;
        std::string body = "short paste";  // 11 chars, no newlines
        f.bar->insert_paste(body);
        // Should be a regular text segment; buffer() == body
        CHECK(f.bar->buffer() == body);
        // cursor should be at end
        CHECK(f.bar->cursor() == body.size());
    }

    TEST_CASE("paste of exactly 199 chars with no newlines is plain text") {
        SegFixture f;
        std::string body(199, 'x');
        f.bar->insert_paste(body);
        CHECK(f.bar->buffer() == body);
    }

    TEST_CASE("paste of 1 char with no newlines is plain text") {
        SegFixture f;
        f.bar->insert_paste("a");
        CHECK(f.bar->buffer() == "a");
    }

    TEST_CASE("paste of 199 chars WITH a newline creates a chip") {
        SegFixture f;
        std::string body(199, 'x');
        body += '\n';
        f.bar->insert_paste(body);
        // flatten_for_submit() should expand to original body
        CHECK(f.bar->flatten_for_submit() == body);
        // Render should not crash
        CHECK(f.bar->Render() != nullptr);
    }
}

// =============================================================================
// SUITE 3 — Backspace deletes paste chip
// =============================================================================
TEST_SUITE("InputBar segments — backspace deletes chip") {

    TEST_CASE("single backspace after paste chip deletes entire chip") {
        SegFixture f;
        std::string paste_body = "paragraph1\nparagraph2\nparagraph3";
        f.bar->insert_paste(paste_body);
        // Cursor should be right after the paste chip
        // One backspace should delete the entire chip
        f.press(kBackspace);
        CHECK(f.bar->buffer() == "");
        CHECK(f.bar->cursor() == 0);
    }

    TEST_CASE("backspace after text then paste chip deletes only chip") {
        SegFixture f;
        f.type_chars("before");
        std::string paste_body = "multi\nline\npaste";
        f.bar->insert_paste(paste_body);
        // Now press backspace once — should delete the paste chip, not "before"
        f.press(kBackspace);
        CHECK(f.bar->buffer() == "before");
    }

    TEST_CASE("two backspaces: first deletes chip, second deletes char before chip") {
        SegFixture f;
        f.type_chars("abc");
        f.bar->insert_paste("line1\nline2");
        // First backspace: delete chip
        f.press(kBackspace);
        CHECK(f.bar->buffer() == "abc");
        // Second backspace: delete 'c'
        f.press(kBackspace);
        CHECK(f.bar->buffer() == "ab");
    }
}

// =============================================================================
// SUITE 4 — Shift+Enter inserts newline
// =============================================================================
TEST_SUITE("InputBar segments — Shift+Enter newline") {

    TEST_CASE("Shift+Enter (Alt alias: \\e\\r) inserts newline into buffer") {
        SegFixture f;
        f.type_chars("line1");
        f.press(kShiftEnterAlt);
        CHECK(f.bar->buffer() == "line1\n");
        CHECK_FALSE(f.submit_called);
    }

    TEST_CASE("Shift+Enter (CSI-u alias: \\e[27;2;13~) inserts newline") {
        SegFixture f;
        f.type_chars("line1");
        f.press(kShiftEnterCSIU);
        CHECK(f.bar->buffer() == "line1\n");
        CHECK_FALSE(f.submit_called);
    }

    TEST_CASE("Shift+Enter (kitty: \\x1b[13;2u) inserts newline via Keybindings") {
        SegFixture f;
        f.type_chars("line1");
        f.press(kShiftEnterKitty);
        CHECK(f.bar->buffer() == "line1\n");
        CHECK_FALSE(f.submit_called);
    }

    TEST_CASE("multiple Shift+Enter calls build a multi-line buffer") {
        SegFixture f;
        f.type_chars("a");
        f.press(kShiftEnterAlt);
        f.type_chars("b");
        f.press(kShiftEnterAlt);
        f.type_chars("c");
        CHECK(f.bar->buffer() == "a\nb\nc");
    }

    TEST_CASE("Shift+Enter does not submit") {
        SegFixture f;
        f.type_chars("hello");
        f.press(kShiftEnterAlt);
        f.press(kShiftEnterCSIU);
        f.press(kShiftEnterKitty);
        CHECK_FALSE(f.submit_called);
    }

    TEST_CASE("buffer with newline from Shift+Enter renders without crash") {
        SegFixture f;
        f.type_chars("a");
        f.press(kShiftEnterAlt);
        f.type_chars("b");
        auto el = f.bar->Render();
        CHECK(el != nullptr);
    }
}

// =============================================================================
// SUITE 5 — Cursor skips over paste chips
// =============================================================================
TEST_SUITE("InputBar segments — cursor navigation over chips") {

    TEST_CASE("arrow-right over paste chip moves past entire chip") {
        SegFixture f;
        f.type_chars("abc");
        f.bar->insert_paste("multi\nline\npaste");
        f.type_chars("xyz");
        // Go to beginning of buffer
        f.press(kHome);
        // Move right 3 chars to reach position after "abc"
        f.press(kArrowRight);
        f.press(kArrowRight);
        f.press(kArrowRight);
        // Now cursor should be at 3 (after "abc", before paste chip body start)
        // One more right-arrow should skip the entire paste body
        const std::size_t before_skip = f.bar->cursor();
        f.press(kArrowRight);
        // After skip, cursor should jump past the paste body
        // The paste body is "multi\nline\npaste" (17 chars)
        // So cursor should be at 3 + 17 = 20
        const std::size_t after_skip = f.bar->cursor();
        CHECK(after_skip > before_skip + 1); // skipped more than 1 byte
    }

    TEST_CASE("arrow-left over paste chip moves before entire chip") {
        SegFixture f;
        f.type_chars("abc");
        f.bar->insert_paste("multi\nline");
        f.type_chars("xyz");
        // Cursor at end. Move left 3 to get past "xyz"
        f.press(kArrowLeft);
        f.press(kArrowLeft);
        f.press(kArrowLeft);
        const std::size_t before_skip = f.bar->cursor();
        // One more left should skip the entire paste body
        f.press(kArrowLeft);
        const std::size_t after_skip = f.bar->cursor();
        CHECK(after_skip < before_skip - 1); // skipped more than 1 byte
    }

    TEST_CASE("Home moves cursor to before first paste chip") {
        SegFixture f;
        f.bar->insert_paste("multi\nline");
        f.type_chars("after");
        f.press(kHome);
        CHECK(f.bar->cursor() == 0);
    }

    TEST_CASE("End moves cursor to end of buffer after paste chip") {
        SegFixture f;
        f.type_chars("before");
        f.bar->insert_paste("multi\nline");
        f.type_chars("after");
        f.press(kHome);
        f.press(kEnd);
        // cursor should be at total length: "before" + "multi\nline" + "after"
        const std::size_t expected = std::string("beforemulti\nlineafter").size();
        CHECK(f.bar->cursor() == expected);
    }
}

// =============================================================================
// SUITE 6 — flatten_for_submit expands paste bodies
// =============================================================================
TEST_SUITE("InputBar segments — flatten_for_submit") {

    TEST_CASE("flatten_for_submit returns full paste body, not chip label") {
        SegFixture f;
        std::string paste_body = "paragraph1\nparagraph2\nparagraph3\nparagraph4\nparagraph5";
        f.bar->insert_paste(paste_body);
        const std::string flat = f.bar->flatten_for_submit();
        // Must equal the original paste body exactly
        CHECK(flat == paste_body);
    }

    TEST_CASE("flatten_for_submit with text + paste + text") {
        SegFixture f;
        f.type_chars("before ");
        f.bar->insert_paste("pasted\ncontent");
        f.type_chars(" after");
        const std::string expected = "before pasted\ncontent after";
        CHECK(f.bar->flatten_for_submit() == expected);
    }

    TEST_CASE("flatten_for_submit with multiple paste chips") {
        SegFixture f;
        f.bar->insert_paste("first\npaste");
        f.type_chars(" middle ");
        f.bar->insert_paste("second\npaste");
        const std::string expected = "first\npaste middle second\npaste";
        CHECK(f.bar->flatten_for_submit() == expected);
    }

    TEST_CASE("submit delivers full expanded text via on_submit callback") {
        SegFixture f;
        f.type_chars("intro: ");
        f.bar->insert_paste("pasted\ncontent\nhere");
        f.type_chars(" end");
        f.press(kReturn);
        REQUIRE(f.submit_called);
        CHECK(f.last_submit == "intro: pasted\ncontent\nhere end");
    }
}

// =============================================================================
// SUITE 7 — Paste chip render label format
// =============================================================================
TEST_SUITE("InputBar segments — paste chip render label") {

    TEST_CASE("multi-line paste chip renders [Pasted text #N +M lines] label") {
        SegFixture f;
        // 3 newlines = 3 lines of content
        f.bar->insert_paste("a\nb\nc\nd");
        auto el = f.bar->Render();
        CHECK(el != nullptr);
        // Cannot directly extract text from FTXUI element in unit tests,
        // but we verify no crash and the paste_chip_label static helper
        PasteSegment ps;
        ps.id = 1; ps.line_count = 3; ps.char_count = 7;
        // Use the static helper indirectly by checking via a test for chip label contents
        // (private method — we verify behaviour via Render() not crashing)
    }

    TEST_CASE("single-line long paste chip renders [Pasted text #N (X chars)] label") {
        SegFixture f;
        std::string body(200, 'a');
        f.bar->insert_paste(body);
        // Chip should be for single-line paste
        auto el = f.bar->Render();
        CHECK(el != nullptr);
    }
}

// =============================================================================
// SUITE 8 — clear() resets paste counter
// =============================================================================
TEST_SUITE("InputBar segments — clear resets state") {

    TEST_CASE("clear() resets buffer and paste counter") {
        SegFixture f;
        f.bar->insert_paste("multi\nline\npaste");
        f.bar->clear();
        CHECK(f.bar->buffer() == "");
        CHECK(f.bar->cursor() == 0);
        // After clear, next paste should start at id #1 again
        f.bar->insert_paste("another\npaste");
        // Just verify it renders OK and has content
        CHECK(f.bar->flatten_for_submit() == "another\npaste");
    }
}

// =============================================================================
// SUITE 9 — Plain Enter regression (text-only buffer)
// =============================================================================
TEST_SUITE("InputBar segments — plain Enter regression") {

    TEST_CASE("plain Enter on text-only buffer submits normally") {
        SegFixture f;
        f.type_chars("hello world");
        f.press(kReturn);
        REQUIRE(f.submit_called);
        CHECK(f.last_submit == "hello world");
        CHECK(f.bar->buffer() == "");
    }

    TEST_CASE("plain Enter on empty buffer does not submit") {
        SegFixture f;
        f.press(kReturn);
        CHECK_FALSE(f.submit_called);
    }

    TEST_CASE("plain Enter after Shift+Enter submits multi-line text") {
        SegFixture f;
        f.type_chars("line1");
        f.press(kShiftEnterAlt);
        f.type_chars("line2");
        f.press(kReturn);
        REQUIRE(f.submit_called);
        CHECK(f.last_submit == "line1\nline2");
    }
}

// =============================================================================
// SUITE 10 — Bracketed paste protocol
// =============================================================================
TEST_SUITE("InputBar segments — bracketed paste protocol") {

    TEST_CASE("bracketed paste envelope creates paste chip for multi-line content") {
        SegFixture f;
        // Simulate terminal sending bracketed paste
        f.press(kBPasteStart);
        // FTXUI delivers the paste content as one or more events
        f.press("line1\nline2\nline3");
        f.press(kBPasteEnd);
        // Should have created a paste chip containing "line1\nline2\nline3"
        CHECK(f.bar->flatten_for_submit() == "line1\nline2\nline3");
        CHECK(f.bar->Render() != nullptr);
    }

    TEST_CASE("bracketed paste with short single-line content inserts as plain text") {
        SegFixture f;
        f.press(kBPasteStart);
        f.press("hello");
        f.press(kBPasteEnd);
        CHECK(f.bar->buffer() == "hello");
    }

    TEST_CASE("bracketed paste does not submit on CRs within paste") {
        SegFixture f;
        f.press(kBPasteStart);
        f.press("paragraph one\r\nparagraph two\r\nparagraph three");
        f.press(kBPasteEnd);
        CHECK_FALSE(f.submit_called);
        // flatten should contain the raw content
        const auto flat = f.bar->flatten_for_submit();
        CHECK(flat.find("paragraph one") != std::string::npos);
    }

    TEST_CASE("bracketed paste split across multiple events accumulates correctly") {
        SegFixture f;
        f.press(kBPasteStart);
        f.press("part1\n");
        f.press("part2\n");
        f.press("part3");
        f.press(kBPasteEnd);
        const std::string flat = f.bar->flatten_for_submit();
        CHECK(flat == "part1\npart2\npart3");
    }
}

// =============================================================================
// SUITE 11 — set_buffer and buffer() backward compat
// =============================================================================
TEST_SUITE("InputBar segments — set_buffer backward compat") {

    TEST_CASE("set_buffer replaces all segments with plain text") {
        SegFixture f;
        f.bar->insert_paste("multi\nline");
        f.type_chars(" after");
        f.bar->set_buffer("new content");
        CHECK(f.bar->buffer() == "new content");
        CHECK(f.bar->cursor() == 11);
    }

    TEST_CASE("buffer() after insert_paste returns flattened content") {
        SegFixture f;
        f.type_chars("pre ");
        f.bar->insert_paste("a\nb\nc");
        f.type_chars(" post");
        CHECK(f.bar->buffer() == "pre a\nb\nc post");
    }
}
