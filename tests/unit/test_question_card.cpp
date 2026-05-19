// tests/unit/test_question_card.cpp
// ---------------------------------------------------------------------------
// doctest suite for batbox::tui::QuestionCard (TUI-ASKQ-T2 + TUI-ASKQ-T3).
//
// Build note: registered via tests/unit/CMakeLists.txt guard block (see below).
// Requires batbox_tui target to be built.
//
// Acceptance criteria tested:
//   [AC1]  After set_spec(payload), is_visible() returns true.
//   [AC2]  OnRender() contains the question text.
//   [AC3]  OnRender() contains all option labels.
//   [AC4]  OnRender() shows ▸ cursor marker on row 0 (initial position).
//   [AC5]  OnRender() shows the footer hint text "Enter".
//   [AC6]  Multi-select: set_spec with multi_select=true → OnRender shows ☐ boxes.
//   [AC7]  Multi-select: ○ circles do NOT appear when multi_select=true.
//   [AC8]  hide() sets is_visible() to false.
//   [AC9]  await_user_answer() is now the real blocking implementation (T3).
//   [AC10] Header truncation: header > 12 chars is truncated with ellipsis.
//   [AC11] OnEvent consumes navigation events (T3 — no longer returns false always).
//
//   TUI-ASKQ-T3 keyboard tests:
//   [KB1]  ↑ / ↓ moves cursor_index (clamped at 0 and last index).
//   [KB2]  1-9 jumps cursor to that 1-indexed option (if in range).
//   [KB3]  Space toggles checked_[cursor] only when multi_select=true.
//   [KB4]  Enter on single-select at cursor index 2 → chosen_labels == {labels[2]}.
//   [KB5]  Enter on multi-select with rows 0,2 checked → chosen_labels == {labels[0], labels[2]}.
//   [KB6]  Esc → cancelled == true.
//   [KB7]  Enter on freeform "Other" row → chosen_labels empty, freeform_text is "".
//   [KB8]  Blocking semantics: worker thread blocks until UI thread fires Enter.
//   [KB9]  Escape-hatch row Enter → escape_hatch == true.
//   [KB10] Multi-select: no rows checked + Enter falls back to cursor row.
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/tui/QuestionCard.hpp>
#include <batbox/tui/Events.hpp>
#include "fixtures/TestTheme.hpp"

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>

using namespace batbox::tui;
using namespace batbox::theme;

// =============================================================================
// Helpers
// =============================================================================

using batbox::test_fixtures::make_test_theme;

/// Render a QuestionCard to a string via ftxui::Screen.
static std::string render_to_string(QuestionCard& card, int width = 80, int height = 30) {
    auto elem    = card.OnRender();
    auto screen  = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(width),
        ftxui::Dimension::Fixed(height));
    ftxui::Render(screen, elem);
    return screen.ToString();
}

/// Build a minimal QuestionShowPayload for single-select tests.
static QuestionShowPayload make_single_select_payload() {
    QuestionShowPayload p;
    p.header       = "Library";
    p.question     = "Which library should we use for date formatting?";
    p.multi_select = false;
    p.labels       = {"date-fns", "dayjs", "luxon"};
    p.descriptions = {
        "Modern, modular, immutable.",
        "Smaller, simpler API.",
        "Full IANA timezone support."
    };
    p.allow_freeform     = false;
    p.allow_escape_hatch = false;
    p.callback           = [](const QuestionResolvedPayload&) {};
    return p;
}

/// Build a multi-select payload.
static QuestionShowPayload make_multi_select_payload() {
    QuestionShowPayload p;
    p.header       = "Features";
    p.question     = "Select features to enable:";
    p.multi_select = true;
    p.labels       = {"Dark mode", "Notifications", "Analytics"};
    p.descriptions = {"", "", ""};
    p.allow_freeform     = false;
    p.allow_escape_hatch = false;
    p.callback           = [](const QuestionResolvedPayload&) {};
    return p;
}

/// Helper: run await_user_answer on a background thread, fire event on the
/// calling thread after a short delay, then return the resolved payload.
static QuestionResolvedPayload fire_event_and_await(
    QuestionCard& card,
    ftxui::Event ev,
    std::chrono::milliseconds delay_ms = std::chrono::milliseconds(25))
{
    auto fut = std::async(std::launch::async, [&]() {
        return card.await_user_answer();
    });

    std::this_thread::sleep_for(delay_ms);
    card.OnEvent(ev);

    return fut.get();
}

// =============================================================================
// Test cases — T2 rendering (regression)
// =============================================================================

TEST_CASE("QuestionCard — single-select rendering") {
    auto theme = make_test_theme();
    QuestionCard card(theme);

    SUBCASE("[AC1] is_visible() false before set_spec") {
        CHECK_FALSE(card.is_visible());
    }

    SUBCASE("[AC1] is_visible() true after set_spec") {
        card.set_spec(make_single_select_payload());
        CHECK(card.is_visible());
    }

    SUBCASE("[AC2] rendered output contains the question text") {
        card.set_spec(make_single_select_payload());
        const std::string rendered = render_to_string(card);
        CHECK(rendered.find("Which library should we use") != std::string::npos);
    }

    SUBCASE("[AC3] rendered output contains all option labels") {
        card.set_spec(make_single_select_payload());
        const std::string rendered = render_to_string(card);
        CHECK(rendered.find("date-fns") != std::string::npos);
        CHECK(rendered.find("dayjs")    != std::string::npos);
        CHECK(rendered.find("luxon")    != std::string::npos);
    }

    SUBCASE("[AC4] cursor arrow ▸ appears on row 0 initially") {
        card.set_spec(make_single_select_payload());
        const std::string rendered = render_to_string(card);
        // ▸ = UTF-8: E2 96 B8
        const std::string arrow = "\xe2\x96\xb8";
        CHECK(rendered.find(arrow) != std::string::npos);
    }

    SUBCASE("[AC5] footer hint text contains 'Enter'") {
        card.set_spec(make_single_select_payload());
        const std::string rendered = render_to_string(card);
        CHECK(rendered.find("Enter") != std::string::npos);
    }

    SUBCASE("[AC7] single-select shows ○ circles (not ☐ boxes)") {
        card.set_spec(make_single_select_payload());
        const std::string rendered = render_to_string(card);
        // ○ = UTF-8: E2 97 8B
        const std::string circle = "\xe2\x97\x8b";
        CHECK(rendered.find(circle) != std::string::npos);
        // ☐ = UTF-8: E2 98 90 — must NOT appear in single-select
        const std::string checkbox = "\xe2\x98\x90";
        CHECK(rendered.find(checkbox) == std::string::npos);
    }

    SUBCASE("[AC8] hide() clears is_visible") {
        card.set_spec(make_single_select_payload());
        CHECK(card.is_visible());
        card.hide();
        CHECK_FALSE(card.is_visible());
    }
}

TEST_CASE("QuestionCard — multi-select rendering") {
    auto theme = make_test_theme();
    QuestionCard card(theme);

    SUBCASE("[AC6] multi-select shows ☐ checkbox markers") {
        card.set_spec(make_multi_select_payload());
        const std::string rendered = render_to_string(card);
        // ☐ = UTF-8: E2 98 90
        const std::string checkbox = "\xe2\x98\x90";
        CHECK(rendered.find(checkbox) != std::string::npos);
    }

    SUBCASE("[AC7] multi-select does NOT show ○ radio circles") {
        card.set_spec(make_multi_select_payload());
        const std::string rendered = render_to_string(card);
        // ○ = UTF-8: E2 97 8B — must NOT appear in multi-select
        const std::string circle = "\xe2\x97\x8b";
        CHECK(rendered.find(circle) == std::string::npos);
    }

    SUBCASE("[AC3] multi-select contains option labels") {
        card.set_spec(make_multi_select_payload());
        const std::string rendered = render_to_string(card);
        CHECK(rendered.find("Dark mode")     != std::string::npos);
        CHECK(rendered.find("Notifications") != std::string::npos);
    }

    SUBCASE("[AC5] multi-select footer shows 'Space to toggle'") {
        card.set_spec(make_multi_select_payload());
        const std::string rendered = render_to_string(card);
        CHECK(rendered.find("Space to toggle") != std::string::npos);
    }
}

TEST_CASE("QuestionCard — header truncation") {
    auto theme = make_test_theme();
    QuestionCard card(theme);

    SUBCASE("[AC10] short header (<=12 chars) renders without truncation") {
        QuestionShowPayload p = make_single_select_payload();
        p.header = "Library";  // 7 chars — fits
        card.set_spec(p);
        const std::string rendered = render_to_string(card);
        CHECK(rendered.find("Library") != std::string::npos);
    }

    SUBCASE("[AC10] long header (>12 chars) is truncated with ellipsis") {
        QuestionShowPayload p = make_single_select_payload();
        p.header = "VeryLongHeaderThatExceedsTheLimit";  // 33 chars
        card.set_spec(p);
        const std::string rendered = render_to_string(card);
        // Ellipsis U+2026 = UTF-8 E2 80 A6
        const std::string ellipsis = "\xe2\x80\xa6";
        CHECK(rendered.find(ellipsis) != std::string::npos);
        // Full header must NOT appear verbatim
        CHECK(rendered.find("VeryLongHeaderThatExceedsTheLimit") == std::string::npos);
    }
}

TEST_CASE("QuestionCard — allow_freeform and allow_escape_hatch") {
    auto theme = make_test_theme();
    QuestionCard card(theme);

    SUBCASE("allow_freeform adds 'Type something' row") {
        QuestionShowPayload p = make_single_select_payload();
        p.allow_freeform = true;
        card.set_spec(p);
        const std::string rendered = render_to_string(card, 80, 40);
        CHECK(rendered.find("Type something") != std::string::npos);
    }

    SUBCASE("allow_escape_hatch adds 'Chat about this' row") {
        QuestionShowPayload p = make_single_select_payload();
        p.allow_escape_hatch = true;
        card.set_spec(p);
        const std::string rendered = render_to_string(card, 80, 40);
        CHECK(rendered.find("Chat about this") != std::string::npos);
    }
}

TEST_CASE("QuestionCard — initial selection state") {
    auto theme = make_test_theme();
    QuestionCard card(theme);

    SUBCASE("cursor_index starts at 0") {
        CHECK(card.cursor_index() == 0);
    }

    SUBCASE("checked vector is empty before set_spec") {
        CHECK(card.checked().empty());
    }

    SUBCASE("set_spec resets checked to all-false") {
        card.set_spec(make_multi_select_payload());
        const auto& c = card.checked();
        CHECK(c.size() == 3u);
        CHECK_FALSE(c[0]);
        CHECK_FALSE(c[1]);
        CHECK_FALSE(c[2]);
    }
}

// =============================================================================
// Test cases — T3 keyboard handling
// =============================================================================

TEST_CASE("[KB1] ArrowDown moves cursor down, clamped at last index") {
    auto theme = make_test_theme();
    QuestionCard card(theme);
    card.set_spec(make_single_select_payload());  // 3 labels

    CHECK(card.cursor_index() == 0);

    card.OnEvent(ftxui::Event::ArrowDown);
    CHECK(card.cursor_index() == 1);

    card.OnEvent(ftxui::Event::ArrowDown);
    CHECK(card.cursor_index() == 2);

    // Clamp: can't go past last row (index 2 for 3 labels).
    card.OnEvent(ftxui::Event::ArrowDown);
    CHECK(card.cursor_index() == 2);
}

TEST_CASE("[KB1] ArrowUp moves cursor up, clamped at 0") {
    auto theme = make_test_theme();
    QuestionCard card(theme);
    card.set_spec(make_single_select_payload());  // 3 labels

    // Move to row 2 first.
    card.OnEvent(ftxui::Event::ArrowDown);
    card.OnEvent(ftxui::Event::ArrowDown);
    CHECK(card.cursor_index() == 2);

    card.OnEvent(ftxui::Event::ArrowUp);
    CHECK(card.cursor_index() == 1);

    card.OnEvent(ftxui::Event::ArrowUp);
    CHECK(card.cursor_index() == 0);

    // Clamp: can't go below 0.
    card.OnEvent(ftxui::Event::ArrowUp);
    CHECK(card.cursor_index() == 0);
}

TEST_CASE("[KB1] 'j' and 'k' vim-style navigation") {
    auto theme = make_test_theme();
    QuestionCard card(theme);
    card.set_spec(make_single_select_payload());

    CHECK(card.cursor_index() == 0);

    card.OnEvent(ftxui::Event::Character('j'));
    CHECK(card.cursor_index() == 1);

    card.OnEvent(ftxui::Event::Character('k'));
    CHECK(card.cursor_index() == 0);
}

TEST_CASE("[KB2] Digit keys 1-9 jump cursor to that 1-indexed row") {
    auto theme = make_test_theme();
    QuestionCard card(theme);
    card.set_spec(make_single_select_payload());  // 3 labels (indices 0,1,2)

    // '3' → index 2 (0-based)
    bool consumed = card.OnEvent(ftxui::Event::Character('3'));
    CHECK(consumed);
    CHECK(card.cursor_index() == 2);

    // '1' → index 0
    card.OnEvent(ftxui::Event::Character('1'));
    CHECK(card.cursor_index() == 0);

    // '2' → index 1
    card.OnEvent(ftxui::Event::Character('2'));
    CHECK(card.cursor_index() == 1);

    // '9' → out of range for 3-label card, should not move
    int before = card.cursor_index();
    bool consumed9 = card.OnEvent(ftxui::Event::Character('9'));
    // Not consumed since target is out of range
    CHECK_FALSE(consumed9);
    CHECK(card.cursor_index() == before);
}

TEST_CASE("[KB3] Space toggles checked only in multi_select mode") {
    auto theme = make_test_theme();
    QuestionCard card(theme);

    SUBCASE("multi_select: Space toggles current row") {
        card.set_spec(make_multi_select_payload());
        CHECK_FALSE(card.checked()[0]);

        bool consumed = card.OnEvent(ftxui::Event::Character(' '));
        CHECK(consumed);
        CHECK(card.checked()[0]);  // toggled on

        card.OnEvent(ftxui::Event::Character(' '));
        CHECK_FALSE(card.checked()[0]);  // toggled off
    }

    SUBCASE("single_select: Space is a no-op") {
        card.set_spec(make_single_select_payload());

        bool consumed = card.OnEvent(ftxui::Event::Character(' '));
        CHECK_FALSE(consumed);
        // cursor_index still 0
        CHECK(card.cursor_index() == 0);
    }
}

TEST_CASE("[KB4] Enter on single-select at cursor index 2 resolves with that label") {
    auto theme = make_test_theme();
    QuestionCard card(theme);
    card.set_spec(make_single_select_payload());  // labels: date-fns, dayjs, luxon

    // Move cursor to index 2 (luxon).
    card.OnEvent(ftxui::Event::ArrowDown);
    card.OnEvent(ftxui::Event::ArrowDown);
    CHECK(card.cursor_index() == 2);

    auto result = fire_event_and_await(card, ftxui::Event::Return);

    CHECK_FALSE(result.cancelled);
    REQUIRE(result.chosen_labels.size() == 1u);
    CHECK(result.chosen_labels[0] == "luxon");
    CHECK_FALSE(result.escape_hatch);
}

TEST_CASE("[KB5] Enter on multi-select with rows 0 and 2 checked") {
    auto theme = make_test_theme();
    QuestionCard card(theme);
    card.set_spec(make_multi_select_payload());  // Dark mode, Notifications, Analytics

    // Check row 0 (cursor starts at 0).
    card.OnEvent(ftxui::Event::Character(' '));
    // Move to row 2 and check it.
    card.OnEvent(ftxui::Event::ArrowDown);
    card.OnEvent(ftxui::Event::ArrowDown);
    card.OnEvent(ftxui::Event::Character(' '));

    CHECK(card.checked()[0]);
    CHECK_FALSE(card.checked()[1]);
    CHECK(card.checked()[2]);

    auto result = fire_event_and_await(card, ftxui::Event::Return);

    CHECK_FALSE(result.cancelled);
    REQUIRE(result.chosen_labels.size() == 2u);
    // Order follows label index order.
    CHECK(result.chosen_labels[0] == "Dark mode");
    CHECK(result.chosen_labels[1] == "Analytics");
}

TEST_CASE("[KB6] Esc returns cancelled=true") {
    auto theme = make_test_theme();
    QuestionCard card(theme);
    card.set_spec(make_single_select_payload());

    auto result = fire_event_and_await(card, ftxui::Event::Escape);

    CHECK(result.cancelled);
    CHECK(result.chosen_labels.empty());
    CHECK_FALSE(result.escape_hatch);
}

TEST_CASE("[KB7] Enter on freeform row returns empty chosen_labels") {
    auto theme = make_test_theme();
    QuestionCard card(theme);

    QuestionShowPayload p = make_single_select_payload();
    p.allow_freeform = true;
    card.set_spec(p);

    // Navigate to the freeform row (index 3 for 3 labels).
    card.OnEvent(ftxui::Event::Character('4'));  // 1-indexed: '4' → index 3
    CHECK(card.cursor_index() == 3);

    auto result = fire_event_and_await(card, ftxui::Event::Return);

    CHECK_FALSE(result.cancelled);
    // Freeform row: chosen_labels empty, freeform_text is "" (T4 fills it in).
    CHECK(result.chosen_labels.empty());
    CHECK(result.freeform_text.empty());
    CHECK_FALSE(result.escape_hatch);
}

TEST_CASE("[KB9] Enter on escape_hatch row sets escape_hatch=true") {
    auto theme = make_test_theme();
    QuestionCard card(theme);

    QuestionShowPayload p = make_single_select_payload();
    p.allow_escape_hatch = true;
    card.set_spec(p);

    // Navigate to the escape-hatch row (index 3 for 3 labels, no freeform).
    card.OnEvent(ftxui::Event::Character('4'));  // '4' → index 3
    CHECK(card.cursor_index() == 3);

    auto result = fire_event_and_await(card, ftxui::Event::Return);

    CHECK_FALSE(result.cancelled);
    CHECK(result.escape_hatch);
    CHECK(result.chosen_labels.empty());
}

TEST_CASE("[KB8] Blocking semantics: worker blocks until UI fires Return") {
    auto theme = make_test_theme();
    QuestionCard card(theme);
    card.set_spec(make_single_select_payload());

    std::atomic<bool> worker_started{false};
    std::atomic<bool> worker_done{false};

    std::thread worker([&]() {
        worker_started.store(true, std::memory_order_release);
        (void)card.await_user_answer();
        worker_done.store(true, std::memory_order_release);
    });

    // Wait for worker to enter await.
    while (!worker_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    // Give it time to block — it must NOT be done yet.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK_FALSE(worker_done.load(std::memory_order_acquire));

    // Resolve it — worker should now unblock.
    card.OnEvent(ftxui::Event::Return);
    worker.join();

    CHECK(worker_done.load(std::memory_order_acquire));
}

TEST_CASE("[KB8] Blocking semantics: worker blocks until UI fires Esc") {
    auto theme = make_test_theme();
    QuestionCard card(theme);
    card.set_spec(make_single_select_payload());

    auto fut = std::async(std::launch::async, [&]() {
        return card.await_user_answer();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(25));

    // Should not be ready yet.
    CHECK(fut.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);

    card.OnEvent(ftxui::Event::Escape);
    auto result = fut.get();

    CHECK(result.cancelled);
}

TEST_CASE("[KB10] Multi-select: no rows checked + Enter falls back to cursor row") {
    auto theme = make_test_theme();
    QuestionCard card(theme);
    card.set_spec(make_multi_select_payload());

    // Do NOT check any row; cursor is at 0.
    CHECK_FALSE(card.checked()[0]);
    CHECK_FALSE(card.checked()[1]);
    CHECK_FALSE(card.checked()[2]);

    auto result = fire_event_and_await(card, ftxui::Event::Return);

    CHECK_FALSE(result.cancelled);
    // Fallback: cursor row 0 → "Dark mode"
    REQUIRE(result.chosen_labels.size() == 1u);
    CHECK(result.chosen_labels[0] == "Dark mode");
}

TEST_CASE("[AC9] await_user_answer before set_spec returns cancelled immediately") {
    auto theme = make_test_theme();
    QuestionCard card(theme);
    // Do NOT call set_spec — empty payload.
    auto result = card.await_user_answer();
    CHECK(result.cancelled);
}

TEST_CASE("QuestionCard reuse: two sequential questions") {
    auto theme = make_test_theme();
    QuestionCard card(theme);

    // First question: select "dayjs" (index 1).
    {
        card.set_spec(make_single_select_payload());
        card.OnEvent(ftxui::Event::ArrowDown);  // index → 1
        auto result = fire_event_and_await(card, ftxui::Event::Return);
        CHECK_FALSE(result.cancelled);
        REQUIRE(result.chosen_labels.size() == 1u);
        CHECK(result.chosen_labels[0] == "dayjs");
    }

    // Second question: cancel via Esc.
    {
        card.set_spec(make_single_select_payload());
        auto result = fire_event_and_await(card, ftxui::Event::Escape);
        CHECK(result.cancelled);
    }
}
