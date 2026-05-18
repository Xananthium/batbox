// tests/unit/test_chatview_user_display_content.cpp
// =============================================================================
// PEXT 6.2 — K-10: MessageEntry::display_content precomputed for user messages
//
// Acceptance criteria tested:
//   [K10-AC1] Short user message (<= 120 chars, no newline): display_content equals
//             raw_content verbatim; is_collapsible is false; render shows full text
//             without truncation (ellipsis does not appear in the "> " prompt row).
//   [K10-AC2] Long user message (> 120 chars, no newline): display_content is
//             truncated to 119 chars + UTF-8 ellipsis; render shows truncated text.
//   [K10-AC3] User message with embedded newline: display_content is the first-line
//             text + ellipsis; render shows only the first line.
//   [K10-AC4] Render 10 times with a 200-char user message — output is stable
//             (same on every frame) and raw_content is never visible past char 119.
//   [K10-AC5] Non-User roles (Assistant) have no display_content influence —
//             assistant content renders without spurious truncation.
//   [K10-AC6] Expanded path (ctrl+o) shows full raw_content (regression guard).
//   [K10-AC7] Message consisting only of a newline character: no crash, shows "> ".
//   [K10-AC8] Message exactly 120 chars: NOT truncated in the "> " prompt row.
//   [K10-AC9] Message of 121 chars: IS truncated in the "> " prompt row.
//   [K10-AC10] Multiple user messages: each gets its own correct display_content.
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/tui/ChatView.hpp>
#include <batbox/tui/Events.hpp>
#include <batbox/theme/Theme.hpp>
#include <batbox/conversation/Message.hpp>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <memory>
#include <string>

using namespace batbox::tui;
using namespace batbox::theme;
using namespace batbox::conversation;

// =============================================================================
// Helpers
// =============================================================================

static Theme make_theme() {
    return theme_from_name("miss-kittin");
}

// Render ChatView into a fixed-size screen and return the raw screen string.
static std::string render_screen(ChatView& cv, int cols = 160, int rows = 40) {
    auto element = cv.OnRender();
    auto screen  = ftxui::Screen::Create(ftxui::Dimension::Fixed(cols),
                                          ftxui::Dimension::Fixed(rows));
    ftxui::Render(screen, element);
    return screen.ToString();
}

// Strip ANSI escape codes for content comparison.
static std::string strip_ansi(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool in_esc = false;
    for (char c : s) {
        if (c == '\033') { in_esc = true; continue; }
        if (in_esc) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) in_esc = false;
            continue;
        }
        out += c;
    }
    return out;
}

// Extract the line(s) containing the "> " user prompt prefix from the rendered output.
// The ChatView renders user messages as a row beginning with "> " followed by the
// display_content.  This helper returns the first line that starts with "> " (after
// stripping leading whitespace) so tests can assert on only the prompt row content,
// not on the spinner tagline or other unrelated rows.
static std::string extract_prompt_line(const std::string& stripped_render) {
    // Split by newlines and find line starting with "> ".
    std::size_t pos = 0;
    while (pos < stripped_render.size()) {
        std::size_t nl = stripped_render.find('\n', pos);
        std::string line = stripped_render.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        // Trim leading spaces.
        std::size_t first = line.find_first_not_of(' ');
        if (first != std::string::npos && line.substr(first, 2) == "> ") {
            return line;
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return "";
}

// The UTF-8 encoding of U+2026 HORIZONTAL ELLIPSIS.
static const std::string kEllipsis = "\xe2\x80\xa6";

// =============================================================================
// K10-AC1: Short message — not truncated, not collapsible
// =============================================================================

TEST_CASE("K10-AC1: short user message renders in full without ellipsis in prompt row") {
    auto theme = make_theme();
    ChatView cv(theme);
    cv.set_screen_post_fn([](ftxui::Event) {});

    cv.OnEvent(batbox::tui::make_user_message_event("Hello, world!"));
    std::string rendered = strip_ansi(render_screen(cv));

    std::string prompt_line = extract_prompt_line(rendered);
    // The prompt row must exist.
    CHECK(!prompt_line.empty());
    // Full text must appear in the prompt row.
    CHECK(prompt_line.find("Hello, world!") != std::string::npos);
    // No ellipsis in the prompt row for a short message.
    CHECK(prompt_line.find(kEllipsis) == std::string::npos);

    // 5 more renders — stable output.
    std::string first = rendered;
    for (int i = 0; i < 5; ++i) {
        std::string r = strip_ansi(render_screen(cv));
        CHECK(r == first);
    }
}

// =============================================================================
// K10-AC2: Long message — truncated at 120 chars with ellipsis in prompt row
// =============================================================================

TEST_CASE("K10-AC2: user message > 120 chars is truncated with ellipsis in prompt row") {
    auto theme = make_theme();
    ChatView cv(theme);
    cv.set_screen_post_fn([](ftxui::Event) {});

    // 200-character message using a recognisable sentinel character, no newlines.
    std::string long_msg(200, 'A');
    cv.OnEvent(batbox::tui::make_user_message_event(long_msg));

    std::string rendered = strip_ansi(render_screen(cv));
    std::string prompt_line = extract_prompt_line(rendered);

    CHECK(!prompt_line.empty());
    // Prompt row must contain the ellipsis (truncated display_content).
    CHECK(prompt_line.find(kEllipsis) != std::string::npos);
    // The full 200-char untruncated string must NOT appear anywhere on screen.
    CHECK(rendered.find(std::string(200, 'A')) == std::string::npos);
    // A run of 120+ A's (untruncated) must not appear in the prompt row.
    CHECK(prompt_line.find(std::string(120, 'A')) == std::string::npos);
}

// =============================================================================
// K10-AC3: Message with newline — first line only + ellipsis in prompt row
// =============================================================================

TEST_CASE("K10-AC3: user message with newline shows first line + ellipsis in prompt row") {
    auto theme = make_theme();
    ChatView cv(theme);
    cv.set_screen_post_fn([](ftxui::Event) {});

    cv.OnEvent(batbox::tui::make_user_message_event("First line content\nSecond line"));
    std::string rendered = strip_ansi(render_screen(cv));
    std::string prompt_line = extract_prompt_line(rendered);

    CHECK(!prompt_line.empty());
    // First line visible in prompt row.
    CHECK(prompt_line.find("First line content") != std::string::npos);
    // Second line must NOT appear in the prompt row.
    CHECK(prompt_line.find("Second line") == std::string::npos);
    // Ellipsis must appear in prompt row (truncated due to newline).
    CHECK(prompt_line.find(kEllipsis) != std::string::npos);
}

// =============================================================================
// K10-AC4: 200-char message, 10 renders — stable output, raw content not shown
// =============================================================================

TEST_CASE("K10-AC4: 10 renders of 200-char user message produce stable truncated output") {
    auto theme = make_theme();
    ChatView cv(theme);
    cv.set_screen_post_fn([](ftxui::Event) {});

    std::string long_msg(200, 'B');
    cv.OnEvent(batbox::tui::make_user_message_event(long_msg));

    std::string first = strip_ansi(render_screen(cv));
    std::string prompt_line = extract_prompt_line(first);

    // Must have ellipsis in prompt row (message is truncated).
    CHECK(!prompt_line.empty());
    CHECK(prompt_line.find(kEllipsis) != std::string::npos);
    // The full 200-B string must NOT appear anywhere on screen.
    CHECK(first.find(std::string(200, 'B')) == std::string::npos);

    // All 10 renders must produce identical output (K-10 ensures no per-frame recompute).
    for (int i = 1; i < 10; ++i) {
        std::string r = strip_ansi(render_screen(cv));
        CHECK(r == first);
    }
}

// =============================================================================
// K10-AC5: Non-User roles — assistant content renders without spurious truncation
// =============================================================================

TEST_CASE("K10-AC5: assistant message renders content without display_content interference") {
    auto theme = make_theme();
    ChatView cv(theme);
    cv.set_screen_post_fn([](ftxui::Event) {});

    Message msg;
    msg.role    = Role::Assistant;
    msg.content = "This is the assistant reply.";
    cv.append_message(msg);

    std::string rendered = strip_ansi(render_screen(cv));
    // Assistant content must appear somewhere on screen.
    CHECK(rendered.find("assistant reply") != std::string::npos);
    // "Batbox: " label should appear (assistant role label).
    CHECK(rendered.find("Batbox") != std::string::npos);
}

// =============================================================================
// K10-AC6: Expanded path (ctrl+o) shows full raw_content — regression guard
// =============================================================================

TEST_CASE("K10-AC6: ctrl+o expanded view shows multi-line raw_content") {
    auto theme = make_theme();
    ChatView cv(theme);
    cv.set_screen_post_fn([](ftxui::Event) {});

    // Use a multi-line message (guaranteed collapsible via newline, simpler to
    // verify in expanded view than a very long single-line message).
    const std::string msg = "First visible line\nSecond line only in expanded view";
    cv.OnEvent(batbox::tui::make_user_message_event(msg));

    // Collapsed render: "Second line" must NOT appear.
    std::string collapsed = strip_ansi(render_screen(cv));
    CHECK(collapsed.find("Second line only in expanded view") == std::string::npos);

    // Expand via ctrl+o.
    ftxui::Event ctrl_o = ftxui::Event::Special("\x0f");
    cv.OnEvent(ctrl_o);

    // Expanded render: the second line must appear.
    std::string expanded = strip_ansi(render_screen(cv));
    CHECK(expanded.find("Second line only in expanded view") != std::string::npos);
    // The collapse hint must appear.
    CHECK(expanded.find("ctrl+o to collapse") != std::string::npos);
}

// =============================================================================
// K10-AC7: Message consisting only of a newline — no crash, prompt row visible
// =============================================================================

TEST_CASE("K10-AC7: user message consisting only of a newline does not crash") {
    auto theme = make_theme();
    ChatView cv(theme);
    cv.set_screen_post_fn([](ftxui::Event) {});

    cv.OnEvent(batbox::tui::make_user_message_event("\n"));

    // Must not crash.
    CHECK_NOTHROW(render_screen(cv));

    std::string rendered = strip_ansi(render_screen(cv));
    std::string prompt_line = extract_prompt_line(rendered);
    // Prompt row must exist.
    CHECK(!prompt_line.empty());
    // Ellipsis must appear in prompt row (truncated due to newline).
    CHECK(prompt_line.find(kEllipsis) != std::string::npos);
}

// =============================================================================
// K10-AC8: Message exactly 120 chars — NOT truncated in prompt row
// =============================================================================

TEST_CASE("K10-AC8: user message of exactly 120 chars is not truncated") {
    auto theme = make_theme();
    ChatView cv(theme);
    cv.set_screen_post_fn([](ftxui::Event) {});

    // Exactly 120 ASCII characters, no newline.
    // 120 == kUserPromptMaxChars; condition is size() > 120, so NOT truncated.
    std::string exact_msg(120, 'D');
    cv.OnEvent(batbox::tui::make_user_message_event(exact_msg));

    std::string rendered = strip_ansi(render_screen(cv));
    std::string prompt_line = extract_prompt_line(rendered);

    CHECK(!prompt_line.empty());
    // No ellipsis in the prompt row for an exactly-at-limit message.
    CHECK(prompt_line.find(kEllipsis) == std::string::npos);
}

// =============================================================================
// K10-AC9: Message of 121 chars — IS truncated in prompt row
// =============================================================================

TEST_CASE("K10-AC9: user message of 121 chars is truncated with ellipsis") {
    auto theme = make_theme();
    ChatView cv(theme);
    cv.set_screen_post_fn([](ftxui::Event) {});

    // 121 ASCII characters, no newline.
    // 121 > kUserPromptMaxChars=120, so IS truncated.
    std::string msg(121, 'E');
    cv.OnEvent(batbox::tui::make_user_message_event(msg));

    std::string rendered = strip_ansi(render_screen(cv));
    std::string prompt_line = extract_prompt_line(rendered);

    CHECK(!prompt_line.empty());
    // Ellipsis must appear in the prompt row.
    CHECK(prompt_line.find(kEllipsis) != std::string::npos);
}

// =============================================================================
// K10-AC10: Multiple user messages — each gets its own correct display_content
// =============================================================================

TEST_CASE("K10-AC10: multiple user messages each precomputed independently") {
    auto theme = make_theme();
    ChatView cv(theme);
    cv.set_screen_post_fn([](ftxui::Event) {});

    cv.OnEvent(batbox::tui::make_user_message_event("short"));

    std::string long2(150, 'F');
    cv.OnEvent(batbox::tui::make_user_message_event(long2));

    cv.OnEvent(batbox::tui::make_user_message_event("line one\nline two"));

    std::string rendered = strip_ansi(render_screen(cv));

    // Short message appears in full somewhere.
    CHECK(rendered.find("short") != std::string::npos);
    // Full 150-F string must not appear (truncated).
    CHECK(rendered.find(std::string(150, 'F')) == std::string::npos);
    // "line one" must appear; "line two" must NOT (collapsed view).
    CHECK(rendered.find("line one") != std::string::npos);
    CHECK(rendered.find("line two") == std::string::npos);
}
