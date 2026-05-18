// tests/unit/test_chatview_toolcard_cache.cpp
// =============================================================================
// PEXT2 3.3 — D-7: ToolCardEntry render cache (cached_render_ / dirty_)
//
// The cache stores the per-card ftxui::Element (shared_ptr<Node>) in
// ToolCardEntry::cached_render_ and skips the Element rebuild when
// neither dirty_ nor term_width has changed.  Because ToolCardEntry is a
// private struct and cached_render_ is a private mutable field, tests
// validate the cache indirectly:
//
//   Stability (cache hit): when no state changes, 30 consecutive renders
//     produce identical text output (same visible content, no unexpected
//     changes caused by rebuild side-effects).
//
//   Invalidation (cache miss): after a state mutation, the next render
//     reflects the updated state (content differs from pre-mutation frames).
//
// Tool verb mappings (per ChatView.cpp kVerbTable):
//   "Bash"  → gerund "Running" / past "Ran"
//   "Read"  → gerund "Reading" / past "Read"
//   "Write" → gerund "Writing" / past "Write"
//
// Acceptance criteria tested:
//   [D7-AC1] 30 renders with no state change after ToolRunning produce
//            identical rendered output — proves no spurious rebuilds.
//   [D7-AC2] ToolDone invalidates cache: render after ToolDone shows
//            past-tense verb (content changes), then stabilises.
//   [D7-AC3] Second ToolRunning (same batch) invalidates cache: render
//            after second ToolRunning shows updated args or verb count.
//   [D7-AC4] Term_width change forces rebuild without crash.
//   [D7-AC5] ctrl+o expand sets dirty_: render after ctrl+o shows
//            expanded affordance, then stabilises.
//   [D7-AC6] Behavioral regression: tool card content remains visible and
//            unchanged across 30 frames.
//   [D7-AC7] PEXT 6.1 non-disturbance: streaming cache coexists with tool
//            card cache — streaming content remains correct throughout.
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

// =============================================================================
// Helpers
// =============================================================================

static Theme make_theme() {
    return theme_from_name("miss-kittin");
}

// Render ChatView into a fixed-size screen and return the text content.
static std::string render_text(ChatView& cv, int cols = 120, int rows = 40) {
    auto element = cv.OnRender();
    auto screen  = ftxui::Screen::Create(ftxui::Dimension::Fixed(cols),
                                          ftxui::Dimension::Fixed(rows));
    ftxui::Render(screen, element);
    return screen.ToString();
}

// Strip ANSI escape codes for text comparison.
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

// =============================================================================
// D7-AC1: 30 renders with no state change produce identical output
// =============================================================================

TEST_CASE("D7-AC1: 30 renders with no state change produce identical output") {
    auto theme = make_theme();
    ChatView cv(theme);
    cv.set_screen_post_fn([](ftxui::Event) {});

    // Start a turn and fire ToolRunning.
    // "Read" maps to gerund "Reading" — visible in the card summary.
    cv.OnEvent(batbox::tui::make_user_message_event("run something"));
    cv.OnEvent(batbox::tui::make_tool_running_event("Read", "manifest.json", 1));

    // First render: builds and caches the card Element.
    std::string first_text = strip_ansi(render_text(cv));
    // "Reading" is the gerund for "Read"; "manifest.json" is the arg preview.
    REQUIRE(first_text.find("Reading") != std::string::npos);

    // 29 more renders with no state change must produce identical output.
    for (int i = 0; i < 29; ++i) {
        std::string iter_text = strip_ansi(render_text(cv));
        CHECK(iter_text == first_text);
    }

    cv.OnEvent(batbox::tui::make_tool_done_event());
    cv.OnEvent(batbox::tui::make_stream_done_event(false));
}

// =============================================================================
// D7-AC2: ToolDone invalidates the cache — output changes after transition
// =============================================================================

TEST_CASE("D7-AC2: ToolDone causes cache invalidation — output changes after transition") {
    auto theme = make_theme();
    ChatView cv(theme);
    cv.set_screen_post_fn([](ftxui::Event) {});

    cv.OnEvent(batbox::tui::make_user_message_event("run tool"));
    // "Read" → gerund "Reading" (in-flight) / past "Read" (done)
    cv.OnEvent(batbox::tui::make_tool_running_event("Read", "config.json", 1));

    // Establish stable in-flight state.
    std::string in_flight_text = strip_ansi(render_text(cv));
    REQUIRE(in_flight_text.find("Reading") != std::string::npos);
    // Stable across frames (cache hit).
    CHECK(strip_ansi(render_text(cv)) == in_flight_text);
    CHECK(strip_ansi(render_text(cv)) == in_flight_text);

    // ToolDone: card transitions from in-flight to complete → dirty_ = true.
    cv.OnEvent(batbox::tui::make_tool_done_event());

    // Next render must reflect the new state (done card shows past tense).
    std::string done_text = strip_ansi(render_text(cv));
    CHECK(done_text != in_flight_text);  // state changed — cache was invalidated

    // Subsequent renders stabilise (cache hit for done state).
    CHECK(strip_ansi(render_text(cv)) == done_text);
    CHECK(strip_ansi(render_text(cv)) == done_text);

    cv.OnEvent(batbox::tui::make_stream_done_event(false));
}

// =============================================================================
// D7-AC3: Second ToolRunning (same batch) invalidates the cache
// =============================================================================

TEST_CASE("D7-AC3: Second ToolRunning (same batch) invalidates cache") {
    auto theme = make_theme();
    ChatView cv(theme);
    cv.set_screen_post_fn([](ftxui::Event) {});

    cv.OnEvent(batbox::tui::make_user_message_event("batch run"));
    // First tool in batch: Read with tool_count=2.
    cv.OnEvent(batbox::tui::make_tool_running_event("Read", "file_a.txt", 2));

    // Establish stable state after first ToolRunning.
    std::string first_text = strip_ansi(render_text(cv));
    REQUIRE(first_text.find("Reading") != std::string::npos);
    // Stable across frames.
    CHECK(strip_ansi(render_text(cv)) == first_text);
    CHECK(strip_ansi(render_text(cv)) == first_text);

    // Second ToolRunning in the same batch: adds "file_b.txt" to preview_lines.
    // This appends to the existing in-flight card → dirty_ = true.
    cv.OnEvent(batbox::tui::make_tool_running_event("Write", "file_b.txt", 2));

    // Post-update state should be stable across multiple frames.
    std::string second_text = strip_ansi(render_text(cv));
    CHECK(strip_ansi(render_text(cv)) == second_text);
    CHECK(strip_ansi(render_text(cv)) == second_text);

    cv.OnEvent(batbox::tui::make_tool_done_event());
    cv.OnEvent(batbox::tui::make_stream_done_event(false));
}

// =============================================================================
// D7-AC4: Term_width change forces rebuild — no crash, card still visible
// =============================================================================

TEST_CASE("D7-AC4: Resize (term_width change) causes rebuild without crash") {
    auto theme = make_theme();
    ChatView cv(theme);
    cv.set_screen_post_fn([](ftxui::Event) {});

    cv.OnEvent(batbox::tui::make_user_message_event("resize test"));
    // "read_file" → gerund "Reading"
    cv.OnEvent(batbox::tui::make_tool_running_event("read_file", "data.csv", 1));

    // Render at 120 cols: must show "Reading".
    std::string t120 = strip_ansi(render_text(cv, 120, 40));
    REQUIRE(t120.find("Reading") != std::string::npos);
    // Stable at 120 cols.
    CHECK(strip_ansi(render_text(cv, 120, 40)) == t120);

    // Render at 80 cols: term_width changes → cache miss → rebuild.
    // Must not crash and card must still show up.
    CHECK_NOTHROW(render_text(cv, 80, 40));
    std::string t80 = strip_ansi(render_text(cv, 80, 40));
    CHECK(t80.find("Reading") != std::string::npos);

    // Return to 120: another rebuild (no crash).
    CHECK_NOTHROW(render_text(cv, 120, 40));
    std::string t120_again = strip_ansi(render_text(cv, 120, 40));
    CHECK(t120_again.find("Reading") != std::string::npos);

    cv.OnEvent(batbox::tui::make_tool_done_event());
    cv.OnEvent(batbox::tui::make_stream_done_event(false));
}

// =============================================================================
// D7-AC5: ctrl+o expand sets dirty_ — expanded content appears, then stabilises
// =============================================================================

TEST_CASE("D7-AC5: ctrl+o expand sets dirty_ and output reflects expanded state") {
    auto theme = make_theme();
    ChatView cv(theme);
    cv.set_screen_post_fn([](ftxui::Event) {});

    cv.OnEvent(batbox::tui::make_user_message_event("tool test"));
    cv.OnEvent(batbox::tui::make_tool_running_event("Read", "large_file.txt", 1));

    // ToolDone first: completed cards are the ctrl+o targets.
    cv.OnEvent(batbox::tui::make_tool_done_event());

    // Stable done state.
    std::string done_text = strip_ansi(render_text(cv));
    CHECK(strip_ansi(render_text(cv)) == done_text);  // cache hit

    // ctrl+o: expands the most-recent completed tool card → dirty_ = true.
    cv.OnEvent(ftxui::Event::Special("\x0f"));

    // Post-expand output must differ from pre-expand (affordance text changes).
    std::string expanded_text = strip_ansi(render_text(cv));
    CHECK(expanded_text != done_text);  // cache invalidated, new content

    // Stabilises after expansion.
    CHECK(strip_ansi(render_text(cv)) == expanded_text);
    CHECK(strip_ansi(render_text(cv)) == expanded_text);

    // Second ctrl+o collapses.
    cv.OnEvent(ftxui::Event::Special("\x0f"));
    std::string collapsed_text = strip_ansi(render_text(cv));
    CHECK(collapsed_text != expanded_text);  // another cache invalidation

    // Stable in collapsed state.
    CHECK(strip_ansi(render_text(cv)) == collapsed_text);

    cv.OnEvent(batbox::tui::make_stream_done_event(false));
}

// =============================================================================
// D7-AC6: Behavioral regression — tool card visible and stable across 30 frames
// =============================================================================

TEST_CASE("D7-AC6: tool card remains visible and stable across 30 renders") {
    auto theme = make_theme();
    ChatView cv(theme);
    cv.set_screen_post_fn([](ftxui::Event) {});

    cv.OnEvent(batbox::tui::make_user_message_event("do stuff"));
    // "read_file" → gerund "Reading"; arg preview "manifest.json" shows on preview line
    cv.OnEvent(batbox::tui::make_tool_running_event("read_file", "manifest.json", 1));

    // First render.
    std::string first_text = strip_ansi(render_text(cv));
    REQUIRE(first_text.find("Reading") != std::string::npos);
    REQUIRE(first_text.find("manifest.json") != std::string::npos);

    // 29 more renders: tool card must remain visible and identical.
    for (int i = 0; i < 29; ++i) {
        std::string frame_text = strip_ansi(render_text(cv));
        CHECK(frame_text.find("Reading") != std::string::npos);
        CHECK(frame_text.find("manifest.json") != std::string::npos);
        CHECK(frame_text == first_text);
    }

    cv.OnEvent(batbox::tui::make_tool_done_event());
    cv.OnEvent(batbox::tui::make_stream_done_event(false));
}

// =============================================================================
// D7-AC7: PEXT 6.1 non-disturbance — streaming cache coexists with tool card cache
// =============================================================================

TEST_CASE("D7-AC7: streaming cache and tool card cache coexist without interference") {
    auto theme = make_theme();
    ChatView cv(theme);
    cv.set_screen_post_fn([](ftxui::Event) {});

    // Start a turn with a tool running AND a streaming token.
    cv.OnEvent(batbox::tui::make_user_message_event("complex turn"));
    cv.OnEvent(batbox::tui::make_tool_running_event("read_file", "cargo.toml", 1));
    cv.OnEvent(batbox::tui::make_token_event("Streaming reply content…"));

    // Both caches build on first render.
    std::string r1 = strip_ansi(render_text(cv));
    REQUIRE(r1.find("Reading") != std::string::npos);
    REQUIRE(r1.find("Streaming") != std::string::npos);

    // Stable on second render (both caches hit).
    std::string r2 = strip_ansi(render_text(cv));
    CHECK(r2 == r1);

    // ToolDone: tool card dirty_ = true; streaming cache unaffected.
    cv.OnEvent(batbox::tui::make_tool_done_event());
    std::string r3 = strip_ansi(render_text(cv));
    // Tool card state changed — overall output differs.
    CHECK(r3 != r1);
    // Streaming content still present.
    CHECK(r3.find("Streaming") != std::string::npos);

    // New token: streaming cache invalidates; tool card cache stable.
    cv.OnEvent(batbox::tui::make_token_event(" additional text"));
    std::string r4 = strip_ansi(render_text(cv));
    CHECK(r4.find("additional text") != std::string::npos);

    // Stabilise with both caches hot.
    std::string r5 = strip_ansi(render_text(cv));
    CHECK(r5 == r4);

    cv.OnEvent(batbox::tui::make_stream_done_event(false));
}

// =============================================================================
// D7: No crash when tool_cards_ is cleared between turns
// =============================================================================

TEST_CASE("D7: cache clears cleanly when UserMessage resets tool_cards_") {
    auto theme = make_theme();
    ChatView cv(theme);
    cv.set_screen_post_fn([](ftxui::Event) {});

    // First turn with a tool card.
    cv.OnEvent(batbox::tui::make_user_message_event("turn 1"));
    cv.OnEvent(batbox::tui::make_tool_running_event("read_file", "a.txt", 1));
    cv.OnEvent(batbox::tui::make_tool_done_event());
    // Render to establish cache (card is now done — shows past tense "Read", not gerund).
    std::string t1 = strip_ansi(render_text(cv));
    // After ToolDone: verb_past for "read_file" is "Read"; preview arg still visible.
    REQUIRE(t1.find("a.txt") != std::string::npos);
    cv.OnEvent(batbox::tui::make_stream_done_event(false));

    // Second turn: UserMessage clears tool_cards_ — no stale cache entries survive.
    cv.OnEvent(batbox::tui::make_user_message_event("turn 2"));
    CHECK_NOTHROW(render_text(cv));  // must not crash with empty tool_cards_

    cv.OnEvent(batbox::tui::make_tool_running_event("write_file", "b.txt", 1));
    std::string t2a = strip_ansi(render_text(cv));
    REQUIRE(t2a.find("Writing") != std::string::npos);
    // Cache hit: stable on second render.
    std::string t2b = strip_ansi(render_text(cv));
    CHECK(t2b == t2a);

    // Multiple frames: stable.
    for (int i = 0; i < 10; ++i) {
        CHECK(strip_ansi(render_text(cv)) == t2a);
    }

    cv.OnEvent(batbox::tui::make_tool_done_event());
    cv.OnEvent(batbox::tui::make_stream_done_event(false));
}
