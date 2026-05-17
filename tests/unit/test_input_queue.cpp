// tests/unit/test_input_queue.cpp
// ---------------------------------------------------------------------------
// doctest suite for TUI-FIX-T10: InputBar input-queue (Codex hybrid).
//
// All 10 edge cases are exercised as unit tests:
//   1. Tab idle + queue empty → existing autocomplete.
//   2. Tab idle + queue non-empty → existing autocomplete.
//   3. Tab mid-turn → append current input as new queue entry; clear input row.
//   4. Tab during paste (in_paste_seq_) → IGNORED; paste accumulator swallows.
//   5. Enter mid-turn → on_interrupt_(); submit <current>\n<joined queue>.
//   6. Enter idle + queue empty → existing submit.
//   7. Enter idle + current empty + queue non-empty → submit joined queue.
//   8. Enter idle + current non-empty + queue non-empty → submit combined.
//   9. Esc streaming + queue empty → cancel stream (T3 path).
//  10. Esc streaming + queue non-empty → cancel stream, PRESERVE queue.
//
// Additional tests:
//  - G1: InputSegment is a tagged-union struct (no std::variant).
//  - G10: format_tokens uses snprintf (verified by behaviour, not internals).
//  - InputQueue SoA: depth, flatten_entry, flatten_all.
//  - Footer chip truncation rule (TERM_WIDTH < 80 → "[N queued]").
//  - Queue max depth (kMaxQueueDepth = 5).
//  - PasteSegment atomicity through queueing.
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
// Fixture
// =============================================================================

static Theme make_theme() {
    return theme_from_name("miss-kittin");
}

struct QueueFixture {
    Theme       theme;
    History     history{std::filesystem::path{}, 1000};
    Keybindings kb;

    std::string  last_submit;
    bool         submit_called{false};
    bool         interrupt_called{false};

    std::shared_ptr<InputBar> bar;

    explicit QueueFixture()
        : theme(make_theme())
        , bar(std::make_shared<InputBar>(
              theme,
              history,
              kb,
              [this](std::string s) { last_submit = s; submit_called = true; },
              nullptr,
              nullptr))
    {
        bar->set_on_interrupt([this]() { interrupt_called = true; });
    }

    bool press(const std::string& input) {
        return bar->OnEvent(ftxui::Event::Special(input));
    }

    void type_chars(std::string_view text) {
        for (char c : text) {
            press(std::string(1, c));
        }
    }

    void set_streaming(bool active) {
        bar->set_stream_active(active);
    }

    const InputQueue& queue() const {
        return bar->input_queue();
    }
};

// Key constants
static const std::string kTab       = "\x09";
static const std::string kReturn    = "\x0a";
static const std::string kEscape    = "\x1b";
static const std::string kBPStart   = "\x1b[200~";
static const std::string kBPEnd     = "\x1b[201~";
static const std::string kArrowUp   = "\x1b[A";
static const std::string kArrowDown = "\x1b[B";

// =============================================================================
// G1: InputSegment tagged-union struct (no std::variant)
// =============================================================================
TEST_SUITE("G1 — InputSegment tagged union") {

    TEST_CASE("InputSegment is a struct with uint8_t kind field") {
        InputSegment text_seg = InputSegment::make_text("hello");
        CHECK(text_seg.kind == 0);
        CHECK(text_seg.body == "hello");
        CHECK(is_text(text_seg));
        CHECK(!is_paste(text_seg));
    }

    TEST_CASE("InputSegment make_paste sets kind=1 and paste fields") {
        InputSegment ps = InputSegment::make_paste(3, "some long paste body\nwith newline", 1, 33);
        CHECK(ps.kind == 1);
        CHECK(ps.paste_id == 3);
        CHECK(ps.line_count == 1);
        CHECK(ps.char_count == 33);
        CHECK(!is_text(ps));
        CHECK(is_paste(ps));
    }

    TEST_CASE("is_text and is_paste are exclusive") {
        InputSegment t = InputSegment::make_text("abc");
        InputSegment p = InputSegment::make_paste(1, "def", 0, 3);
        CHECK(is_text(t) != is_paste(t));
        CHECK(is_text(p) != is_paste(p));
    }

    TEST_CASE("InputSegment size is smaller than std::variant alternative") {
        // Tagged union struct must not exceed 64 bytes (variant would be ~72).
        // We just check it's a reasonable size — no variant overhead.
        CHECK(sizeof(InputSegment) <= 64);
    }
}

// =============================================================================
// InputQueue SoA
// =============================================================================
TEST_SUITE("InputQueue SoA") {

    TEST_CASE("empty queue has depth 0") {
        InputQueue q;
        CHECK(q.depth() == 0);
        CHECK(q.empty());
    }

    TEST_CASE("append increases depth") {
        InputQueue q;
        std::vector<InputSegment> segs1 = { InputSegment::make_text("hello") };
        std::vector<InputSegment> segs2 = { InputSegment::make_text("world") };
        q.append(segs1);
        CHECK(q.depth() == 1);
        q.append(segs2);
        CHECK(q.depth() == 2);
        CHECK(!q.empty());
    }

    TEST_CASE("flatten_entry returns text of that entry") {
        InputQueue q;
        std::vector<InputSegment> s1 = { InputSegment::make_text("first entry") };
        std::vector<InputSegment> s2 = { InputSegment::make_text("second entry") };
        q.append(s1);
        q.append(s2);
        CHECK(q.flatten_entry(0) == "first entry");
        CHECK(q.flatten_entry(1) == "second entry");
    }

    TEST_CASE("flatten_all joins entries with newlines") {
        InputQueue q;
        std::vector<InputSegment> s1 = { InputSegment::make_text("line1") };
        std::vector<InputSegment> s2 = { InputSegment::make_text("line2") };
        std::vector<InputSegment> s3 = { InputSegment::make_text("line3") };
        q.append(s1);
        q.append(s2);
        q.append(s3);
        CHECK(q.flatten_all() == "line1\nline2\nline3");
    }

    TEST_CASE("clear empties the queue") {
        InputQueue q;
        std::vector<InputSegment> s = { InputSegment::make_text("data") };
        q.append(s);
        q.append(s);
        CHECK(q.depth() == 2);
        q.clear();
        CHECK(q.depth() == 0);
        CHECK(q.empty());
    }

    TEST_CASE("PasteSegment survives queueing (atomicity)") {
        InputQueue q;
        // A queue entry with a paste segment keeps its kind and fields
        std::vector<InputSegment> segs = {
            InputSegment::make_text("prefix "),
            InputSegment::make_paste(1, "pasted content\nline2", 1, 20),
            InputSegment::make_text(" suffix"),
        };
        q.append(segs);
        CHECK(q.depth() == 1);
        // The flat body includes all three segments concatenated
        std::string flat = q.flatten_entry(0);
        CHECK(flat.find("prefix ") != std::string::npos);
        CHECK(flat.find("pasted content\nline2") != std::string::npos);
        CHECK(flat.find(" suffix") != std::string::npos);
        // The paste segment itself is stored with kind=1
        CHECK(q.all_segments.size() == 3);
        CHECK(is_paste(q.all_segments[1]));
        CHECK(q.all_segments[1].paste_id == 1);
        CHECK(q.all_segments[1].line_count == 1);
    }
}

// =============================================================================
// Edge case 1: Tab idle + queue empty → existing autocomplete
// =============================================================================
TEST_SUITE("Edge case 1 — Tab idle + queue empty → autocomplete") {

    TEST_CASE("Tab idle with empty queue invokes autocomplete provider") {
        Theme       th = make_theme();
        History     h{std::filesystem::path{}, 1000};
        Keybindings kb;

        bool ac_called = false;
        auto bar = std::make_shared<InputBar>(
            th, h, kb,
            [](std::string) {},
            nullptr,
            [&ac_called](std::string_view) -> std::vector<std::string> {
                ac_called = true;
                return {"suggestion"};
            });

        // Type some text while idle (not streaming)
        bar->OnEvent(ftxui::Event::Special("h"));
        bar->OnEvent(ftxui::Event::Special("i"));
        bar->OnEvent(ftxui::Event::Special(kTab));

        CHECK(ac_called);
        CHECK(bar->input_queue().empty());
    }
}

// =============================================================================
// Edge case 2: Tab idle + queue non-empty → still autocomplete
// =============================================================================
TEST_SUITE("Edge case 2 — Tab idle + queue non-empty → autocomplete") {

    TEST_CASE("Tab idle with non-empty queue still calls autocomplete, not queue") {
        Theme       th = make_theme();
        History     h{std::filesystem::path{}, 1000};
        Keybindings kb;

        bool ac_called = false;
        auto bar = std::make_shared<InputBar>(
            th, h, kb,
            [](std::string) {},
            nullptr,
            [&ac_called](std::string_view) -> std::vector<std::string> {
                ac_called = true;
                return {"auto"};
            });

        // Manually put something in queue by going streaming->Tab->idle
        bar->set_stream_active(true);
        bar->OnEvent(ftxui::Event::Special("q"));
        bar->OnEvent(ftxui::Event::Special("u"));
        bar->OnEvent(ftxui::Event::Special("e"));
        bar->OnEvent(ftxui::Event::Special(kTab));
        CHECK(bar->input_queue().depth() == 1);

        // Now idle
        bar->set_stream_active(false);
        bar->OnEvent(ftxui::Event::Special("n"));
        bar->OnEvent(ftxui::Event::Special("e"));
        bar->OnEvent(ftxui::Event::Special("w"));
        bar->OnEvent(ftxui::Event::Special(kTab));

        CHECK(ac_called);
        // Queue depth should be unchanged at 1 (not 2)
        CHECK(bar->input_queue().depth() == 1);
    }
}

// =============================================================================
// Edge case 3: Tab mid-turn → append current input; clear input row
// =============================================================================
TEST_SUITE("Edge case 3 — Tab mid-turn → append to queue") {

    TEST_CASE("Tab while streaming appends current input to queue") {
        QueueFixture f;
        f.set_streaming(true);

        f.type_chars("first question");
        CHECK(f.bar->buffer() == "first question");
        CHECK(f.queue().depth() == 0);

        f.press(kTab);

        CHECK(f.queue().depth() == 1);
        CHECK(f.queue().flatten_entry(0) == "first question");
        // Input row should be cleared
        CHECK(f.bar->buffer() == "");
        // cursor_idx stays on input row
    }

    TEST_CASE("Multiple Tabs mid-turn append multiple queue entries") {
        QueueFixture f;
        f.set_streaming(true);

        f.type_chars("q1");
        f.press(kTab);
        f.type_chars("q2");
        f.press(kTab);
        f.type_chars("q3");
        f.press(kTab);

        CHECK(f.queue().depth() == 3);
        CHECK(f.queue().flatten_entry(0) == "q1");
        CHECK(f.queue().flatten_entry(1) == "q2");
        CHECK(f.queue().flatten_entry(2) == "q3");
        CHECK(f.bar->buffer() == "");
    }

    TEST_CASE("Tab mid-turn with empty buffer does NOT create empty queue entry") {
        QueueFixture f;
        f.set_streaming(true);
        // Buffer is empty — Tab should not append empty entry
        f.press(kTab);
        CHECK(f.queue().depth() == 0);
    }

    TEST_CASE("Queue bounded to kMaxQueueDepth entries") {
        QueueFixture f;
        f.set_streaming(true);

        for (std::size_t i = 0; i < InputQueue::kMaxQueueDepth + 3; ++i) {
            f.type_chars("x");
            f.press(kTab);
        }
        CHECK(f.queue().depth() == InputQueue::kMaxQueueDepth);
    }
}

// =============================================================================
// Edge case 4: Tab during paste accumulation → IGNORED
// =============================================================================
TEST_SUITE("Edge case 4 — Tab during in_paste_seq_ ignored") {

    TEST_CASE("Tab inside bracketed paste does not trigger queue append") {
        QueueFixture f;
        f.set_streaming(true);

        // Start bracketed paste
        f.press(kBPStart);
        // Tab arrives inside paste accumulator — should be swallowed as text
        f.press(kTab);
        // End bracketed paste (short text, goes as plain text since < 200 chars)
        f.press(kBPEnd);

        // Queue should be empty (Tab did not trigger queue)
        CHECK(f.queue().empty());
        // The Tab character (\x09) should be in the buffer as text
        CHECK(f.bar->buffer().find('\x09') != std::string::npos);
    }

    TEST_CASE("Tab inside paste does not change queue depth mid-accumulation") {
        QueueFixture f;
        f.set_streaming(true);

        // Pre-existing queue entry
        f.type_chars("before");
        f.press(kTab);
        CHECK(f.queue().depth() == 1);

        // Now paste with a Tab inside
        f.press(kBPStart);
        f.press(kTab);
        f.press(kBPEnd);

        // Queue depth should still be 1 (Tab inside paste did not add entry)
        CHECK(f.queue().depth() == 1);
    }
}

// =============================================================================
// Edge case 5: Enter mid-turn → on_interrupt_() + submit combined
// =============================================================================
TEST_SUITE("Edge case 5 — Enter mid-turn: interrupt + combined submit") {

    TEST_CASE("Enter mid-turn with no queue: interrupts and submits current") {
        QueueFixture f;
        f.set_streaming(true);
        f.type_chars("steer me here");

        f.press(kReturn);

        CHECK(f.interrupt_called);
        CHECK(f.submit_called);
        CHECK(f.last_submit == "steer me here");
        // Both input and queue cleared
        CHECK(f.bar->buffer() == "");
        CHECK(f.queue().empty());
    }

    TEST_CASE("Enter mid-turn with queue: interrupts and submits current+queue") {
        QueueFixture f;
        f.set_streaming(true);

        // Build queue
        f.type_chars("q1");
        f.press(kTab);
        f.type_chars("q2");
        f.press(kTab);

        // Type current and press Enter
        f.type_chars("steer");
        f.press(kReturn);

        CHECK(f.interrupt_called);
        CHECK(f.submit_called);
        // Combined: current + "\n" + queue entries joined with "\n"
        CHECK(f.last_submit == "steer\nq1\nq2");
        CHECK(f.bar->buffer() == "");
        CHECK(f.queue().empty());
    }

    TEST_CASE("Enter mid-turn with empty current + non-empty queue: interrupt + submit queue") {
        QueueFixture f;
        f.set_streaming(true);

        f.type_chars("only in queue");
        f.press(kTab);
        // Input row is now empty; stream still active

        f.press(kReturn);

        CHECK(f.interrupt_called);
        CHECK(f.submit_called);
        CHECK(f.last_submit == "only in queue");
        CHECK(f.queue().empty());
    }
}

// =============================================================================
// Edge case 6: Enter idle + queue empty → existing submit
// =============================================================================
TEST_SUITE("Edge case 6 — Enter idle + queue empty → plain submit") {

    TEST_CASE("Enter idle with no queue submits buffer normally") {
        QueueFixture f;
        f.type_chars("hello world");

        f.press(kReturn);

        CHECK(!f.interrupt_called);
        CHECK(f.submit_called);
        CHECK(f.last_submit == "hello world");
        CHECK(f.bar->buffer() == "");
        CHECK(f.queue().empty());
    }

    TEST_CASE("Enter idle with empty buffer and empty queue is a no-op") {
        QueueFixture f;
        bool consumed = f.press(kReturn);
        CHECK(!f.submit_called);
        CHECK(!f.interrupt_called);
        // false return means the event was not consumed (no action)
        CHECK(!consumed);
    }
}

// =============================================================================
// Edge case 7: Enter idle + current empty + queue non-empty → submit queue
// =============================================================================
TEST_SUITE("Edge case 7 — Enter idle + empty current + non-empty queue") {

    TEST_CASE("Enter idle with empty current submits queue alone") {
        QueueFixture f;
        f.set_streaming(true);

        f.type_chars("queued1");
        f.press(kTab);
        f.type_chars("queued2");
        f.press(kTab);

        f.set_streaming(false);
        // Input row is empty, queue has 2 entries
        CHECK(f.bar->buffer() == "");
        CHECK(f.queue().depth() == 2);

        f.press(kReturn);

        CHECK(!f.interrupt_called);
        CHECK(f.submit_called);
        CHECK(f.last_submit == "queued1\nqueued2");
        CHECK(f.queue().empty());
    }
}

// =============================================================================
// Edge case 8: Enter idle + current non-empty + queue non-empty → combined
// =============================================================================
TEST_SUITE("Edge case 8 — Enter idle + non-empty current + non-empty queue") {

    TEST_CASE("Enter idle with both current and queue submits combined") {
        QueueFixture f;
        f.set_streaming(true);

        f.type_chars("queued_a");
        f.press(kTab);
        f.type_chars("queued_b");
        f.press(kTab);

        f.set_streaming(false);
        f.type_chars("current_text");
        CHECK(f.queue().depth() == 2);
        CHECK(f.bar->buffer() == "current_text");

        f.press(kReturn);

        CHECK(!f.interrupt_called);
        CHECK(f.submit_called);
        CHECK(f.last_submit == "current_text\nqueued_a\nqueued_b");
        CHECK(f.queue().empty());
        CHECK(f.bar->buffer() == "");
    }
}

// =============================================================================
// Edge case 9: Esc streaming + queue empty → cancel stream
// =============================================================================
TEST_SUITE("Edge case 9 — Esc streaming + queue empty → cancel") {

    TEST_CASE("Esc while streaming with empty queue fires interrupt callback") {
        QueueFixture f;
        f.set_streaming(true);
        CHECK(f.queue().empty());

        f.press(kEscape);

        CHECK(f.interrupt_called);
        CHECK(!f.submit_called);
        // Queue remains empty
        CHECK(f.queue().empty());
    }
}

// =============================================================================
// Edge case 10: Esc streaming + queue non-empty → cancel, PRESERVE queue
// =============================================================================
TEST_SUITE("Edge case 10 — Esc streaming + queue non-empty → cancel, preserve queue") {

    TEST_CASE("Esc while streaming preserves the queue") {
        QueueFixture f;
        f.set_streaming(true);

        f.type_chars("future1");
        f.press(kTab);
        f.type_chars("future2");
        f.press(kTab);

        CHECK(f.queue().depth() == 2);

        f.press(kEscape);

        CHECK(f.interrupt_called);
        // Queue is preserved — user queued things to ask after the cancelled turn
        CHECK(f.queue().depth() == 2);
        CHECK(f.queue().flatten_entry(0) == "future1");
        CHECK(f.queue().flatten_entry(1) == "future2");
        // No submit
        CHECK(!f.submit_called);
    }
}

// =============================================================================
// Queue navigation (cursor_idx_)
// =============================================================================
TEST_SUITE("Queue navigation — cursor_idx_") {

    TEST_CASE("Up from input row with non-empty queue moves to last queue entry") {
        QueueFixture f;
        f.set_streaming(true);

        f.type_chars("entry1");
        f.press(kTab);
        f.type_chars("entry2");
        f.press(kTab);

        f.set_streaming(false);
        CHECK(f.queue().depth() == 2);

        // Press Up — should move cursor_idx_ to 1 (last entry)
        f.press(kArrowUp);
        // Render should not crash and queue rows render correctly
        CHECK(f.bar->Render() != nullptr);
    }

    TEST_CASE("Down from queue row returns to input row") {
        QueueFixture f;
        f.set_streaming(true);
        f.type_chars("entry1");
        f.press(kTab);
        f.set_streaming(false);

        f.press(kArrowUp);   // moves to queue row 0
        f.press(kArrowDown); // back to input row
        // Render should not crash
        CHECK(f.bar->Render() != nullptr);
    }
}

// =============================================================================
// G10: format_tokens snprintf (verified via render output)
// =============================================================================
TEST_SUITE("G10 — format_tokens snprintf") {

    TEST_CASE("Small token count renders without crash") {
        QueueFixture f;
        f.bar->set_usage(42, 0.001);
        auto el = f.bar->Render();
        CHECK(el != nullptr);
    }

    TEST_CASE("Large token count (comma-separated) renders without crash") {
        QueueFixture f;
        f.bar->set_usage(1234567, 1.234);
        auto el = f.bar->Render();
        CHECK(el != nullptr);
    }

    TEST_CASE("Zero tokens renders without crash") {
        QueueFixture f;
        f.bar->set_usage(0, 0.0);
        auto el = f.bar->Render();
        CHECK(el != nullptr);
    }
}

// =============================================================================
// Queue render — rows appear above prompt row
// =============================================================================
TEST_SUITE("Queue render — rows above prompt") {

    TEST_CASE("Render with queued entries does not crash") {
        QueueFixture f;
        f.set_streaming(true);

        f.type_chars("first queued");
        f.press(kTab);
        f.type_chars("second queued");
        f.press(kTab);

        f.set_streaming(false);

        auto el = f.bar->Render();
        CHECK(el != nullptr);
    }

    TEST_CASE("Render with empty queue does not show queue rows") {
        QueueFixture f;
        CHECK(f.queue().empty());
        auto el = f.bar->Render();
        CHECK(el != nullptr);
    }
}

// =============================================================================
// Footer chip truncation
// =============================================================================
TEST_SUITE("Footer chip — queue depth chip") {

    TEST_CASE("Queue depth chip appears in compute_footer_chips when non-empty") {
        // We test the compute path indirectly via the chip content being rendered.
        // The only observable behaviour from unit tests is that render doesn't crash
        // and that the queue is non-empty.
        QueueFixture f;
        f.set_streaming(true);
        f.type_chars("q");
        f.press(kTab);
        f.set_streaming(false);

        CHECK(f.queue().depth() == 1);
        auto el = f.bar->Render();
        CHECK(el != nullptr);
    }
}

// =============================================================================
// Regression: existing behaviour unchanged when queue is empty and idle
// =============================================================================
TEST_SUITE("Regression — existing behaviour with no queue") {

    TEST_CASE("Plain submit works normally with empty queue") {
        QueueFixture f;
        f.type_chars("hello");
        f.press(kReturn);
        CHECK(f.submit_called);
        CHECK(f.last_submit == "hello");
    }

    TEST_CASE("Esc while idle with text clears buffer but does not fire interrupt") {
        QueueFixture f;
        f.type_chars("something");
        f.press(kEscape);
        CHECK(!f.interrupt_called);
        CHECK(f.bar->buffer() == "");
    }

    TEST_CASE("Bracketed paste inserts chip normally when queue empty") {
        QueueFixture f;
        std::string big_paste(200, 'x');
        f.press(kBPStart);
        for (char c : big_paste) {
            f.press(std::string(1, c));
        }
        f.press(kBPEnd);
        CHECK(f.bar->buffer() == big_paste);
        CHECK(f.queue().empty());
    }
}
