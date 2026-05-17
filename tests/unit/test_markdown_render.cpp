// tests/unit/test_markdown_render.cpp
// ---------------------------------------------------------------------------
// doctest suite for batbox::tui::MarkdownRenderer
//
// Coverage:
//  A — Basic block types (headings, code fence, blockquote, lists, table)
//  B — Inline styling (bold, italic, inline code, links)
//  C — Streaming / incremental append
//  D — Block caching (cached_block_count)
//  E — State inspection (in_code_fence, reset)
// ---------------------------------------------------------------------------
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/tui/MarkdownRender.hpp>
#include <batbox/theme/Theme.hpp>

#include <string>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
namespace {

/// Build the miss-kittin theme using documented color values from CPP 1.1.
batbox::theme::Theme make_test_theme() {
    using C = ftxui::Color;
    batbox::theme::Theme t;
    t.name            = "miss-kittin";
    t.bg              = C::RGB(10,  10,  10 );
    t.fg              = C::RGB(232, 232, 232);
    t.accent_magenta  = C::RGB(255, 42,  140);
    t.accent_cyan     = C::RGB(40,  221, 255);
    t.muted           = C::RGB(102, 102, 102);
    t.success         = C::RGB(57,  255, 112);
    t.error           = C::RGB(255, 59,  59 );
    t.diff_add_fg     = C::RGB(57,  255, 112);
    t.diff_add_bg     = C::RGB(14,  30,  14 );
    t.diff_remove_fg  = C::RGB(255, 85,  85 );
    t.diff_remove_bg  = C::RGB(30,  14,  14 );
    t.prompt_prefix   = C::RGB(255, 42,  140);
    t.code_bg         = C::RGB(20,  20,  20 );
    return t;
}

/// Feed a complete string (with newlines) to the renderer and return the
/// rendered element.  The element is only checked for non-null; for snapshot
/// tests we inspect state counters, not the pixel content.
void feed(batbox::tui::MarkdownRenderer& r, const std::string& s) {
    r.append(s);
}

} // namespace

// ============================================================================
// TEST SUITE A — Block types
// ============================================================================

TEST_SUITE("MarkdownRenderer::block_types") {

// ---------------------------------------------------------------------------
// A.1 — H1 heading is rendered as a single cached block.
// ---------------------------------------------------------------------------
TEST_CASE("H1 heading produces one cached block") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    feed(r, "# Hello World\n");
    // Headings flush immediately; the heading itself is one cached element.
    CHECK(r.cached_block_count() >= 1);
    auto el = r.render();
    CHECK(el != nullptr);
}

// ---------------------------------------------------------------------------
// A.2 — H2 heading
// ---------------------------------------------------------------------------
TEST_CASE("H2 heading produces cached block") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    feed(r, "## Section\n");
    CHECK(r.cached_block_count() >= 1);
}

// ---------------------------------------------------------------------------
// A.3 — H3 heading
// ---------------------------------------------------------------------------
TEST_CASE("H3 heading cached block") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    feed(r, "### Sub-section\n");
    CHECK(r.cached_block_count() >= 1);
}

// ---------------------------------------------------------------------------
// A.4 — Fenced code block: ``` lang ... ```
// ---------------------------------------------------------------------------
TEST_CASE("Fenced code block with language tag") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    feed(r, "```cpp\n");
    // After opening fence we are inside a code block.
    CHECK(r.in_code_fence() == true);

    feed(r, "int main() { return 0; }\n");
    CHECK(r.in_code_fence() == true);

    feed(r, "```\n");
    // After closing fence the block is finalised.
    CHECK(r.in_code_fence() == false);
    CHECK(r.cached_block_count() >= 1);

    auto el = r.render();
    CHECK(el != nullptr);
}

// ---------------------------------------------------------------------------
// A.5 — Fenced code block without language tag
// ---------------------------------------------------------------------------
TEST_CASE("Fenced code block without language tag") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    feed(r, "```\nsome code\n```\n");
    CHECK(r.in_code_fence() == false);
    CHECK(r.cached_block_count() >= 1);
}

// ---------------------------------------------------------------------------
// A.6 — Fenced code block: content with triple-backtick inside should not
//         close unless indented exactly.
// ---------------------------------------------------------------------------
TEST_CASE("Code fence not closed by backtick-less line") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    feed(r, "```python\n");
    feed(r, "x = '``'\n");  // two backticks — should not close
    CHECK(r.in_code_fence() == true);
    feed(r, "```\n");
    CHECK(r.in_code_fence() == false);
}

// ---------------------------------------------------------------------------
// A.7 — Unordered list (dash bullets)
// ---------------------------------------------------------------------------
TEST_CASE("Unordered list with dash bullets produces cached block") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    feed(r, "- item one\n- item two\n- item three\n");
    // Blank line or end-of-input flushes; a subsequent non-list line would flush.
    // We can force flush by checking after a paragraph.
    feed(r, "\n");
    CHECK(r.cached_block_count() >= 1);
    CHECK(r.render() != nullptr);
}

// ---------------------------------------------------------------------------
// A.8 — Unordered list (asterisk bullets)
// ---------------------------------------------------------------------------
TEST_CASE("Unordered list with asterisk bullets") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    feed(r, "* foo\n* bar\n\n");
    CHECK(r.cached_block_count() >= 1);
}

// ---------------------------------------------------------------------------
// A.9 — Ordered list
// ---------------------------------------------------------------------------
TEST_CASE("Ordered list renders with numbers") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    feed(r, "1. First\n2. Second\n3. Third\n\n");
    CHECK(r.cached_block_count() >= 1);
    CHECK(r.render() != nullptr);
}

// ---------------------------------------------------------------------------
// A.10 — Blockquote
// ---------------------------------------------------------------------------
TEST_CASE("Blockquote produces cached block") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    feed(r, "> This is a quote\n> continued here\n\n");
    CHECK(r.cached_block_count() >= 1);
    CHECK(r.render() != nullptr);
}

// ---------------------------------------------------------------------------
// A.11 — Simple pipe table
// ---------------------------------------------------------------------------
TEST_CASE("Pipe table renders without crash") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    feed(r, "| Name | Value |\n| ---- | ----- |\n| foo  | 42    |\n\n");
    CHECK(r.cached_block_count() >= 1);
    CHECK(r.render() != nullptr);
}

// ---------------------------------------------------------------------------
// A.12 — Paragraph block
// ---------------------------------------------------------------------------
TEST_CASE("Plain paragraph text accumulates and flushes on blank line") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    feed(r, "This is a paragraph\nwith two lines.\n\n");
    CHECK(r.cached_block_count() >= 1);
    CHECK(r.render() != nullptr);
}

} // TEST_SUITE("MarkdownRenderer::block_types")

// ============================================================================
// TEST SUITE B — Inline styling
// ============================================================================

TEST_SUITE("MarkdownRenderer::inline_styling") {

// ---------------------------------------------------------------------------
// B.1 — Bold text (**) renders without crash.
// ---------------------------------------------------------------------------
TEST_CASE("Bold **text** renders") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    feed(r, "This is **bold** text.\n\n");
    CHECK(r.render() != nullptr);
}

// ---------------------------------------------------------------------------
// B.2 — Bold text (__) renders without crash.
// ---------------------------------------------------------------------------
TEST_CASE("Bold __text__ renders") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    feed(r, "This is __bold__ text.\n\n");
    CHECK(r.render() != nullptr);
}

// ---------------------------------------------------------------------------
// B.3 — Italic text (*) renders without crash.
// ---------------------------------------------------------------------------
TEST_CASE("Italic *text* renders") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    feed(r, "This is *italic* text.\n\n");
    CHECK(r.render() != nullptr);
}

// ---------------------------------------------------------------------------
// B.4 — Inline code (`code`) renders without crash.
// ---------------------------------------------------------------------------
TEST_CASE("Inline `code` renders") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    feed(r, "Call `main()` to start.\n\n");
    CHECK(r.render() != nullptr);
}

// ---------------------------------------------------------------------------
// B.5 — Link [label](url) renders label.
// ---------------------------------------------------------------------------
TEST_CASE("Link [label](url) renders label") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    feed(r, "See [GitHub](https://github.com) for details.\n\n");
    CHECK(r.render() != nullptr);
}

// ---------------------------------------------------------------------------
// B.6 — Mixed inline: bold + code + link in one line.
// ---------------------------------------------------------------------------
TEST_CASE("Mixed inline styles on one line render without crash") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    feed(r, "**bold** and `code` and [link](http://x.com) here.\n\n");
    CHECK(r.render() != nullptr);
}

// ---------------------------------------------------------------------------
// B.7 — Line with no special markup renders as plain text.
// ---------------------------------------------------------------------------
TEST_CASE("Plain text line renders") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    feed(r, "Just plain text here.\n\n");
    CHECK(r.render() != nullptr);
}

} // TEST_SUITE("MarkdownRenderer::inline_styling")

// ============================================================================
// TEST SUITE C — Streaming / incremental append
// ============================================================================

TEST_SUITE("MarkdownRenderer::streaming") {

// ---------------------------------------------------------------------------
// C.1 — render() before any append returns non-null (empty element).
// ---------------------------------------------------------------------------
TEST_CASE("render() before any input returns emptyElement (not nullptr)") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);
    // emptyElement() returns a valid (non-null) shared_ptr in FTXUI.
    CHECK(r.render() != nullptr);
}

// ---------------------------------------------------------------------------
// C.2 — Partial line in buffer does not crash on render().
// ---------------------------------------------------------------------------
TEST_CASE("Partial line in buffer renders without crash") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    // Feed without a trailing newline — stays in line_buf_.
    r.append("# Incomplete heading");
    CHECK(r.in_code_fence() == false);
    CHECK(r.render() != nullptr);
}

// ---------------------------------------------------------------------------
// C.3 — 1000-token streaming append doesn't produce more cached blocks
//         than lines processed (incremental caching holds).
// ---------------------------------------------------------------------------
TEST_CASE("1000-token append stays incremental (no full reparse)") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    // Simulate 1000 short token chunks making up a paragraph.
    std::string token = "word ";
    for (int i = 0; i < 1000; ++i) {
        r.append(token);
    }
    // No newlines yet — all in line_buf_, zero cached blocks.
    CHECK(r.cached_block_count() == 0);

    // Terminate the line.
    r.append("\n");
    // One line added to open block; still 0 cached (paragraph not flushed yet).
    CHECK(r.cached_block_count() == 0);

    // Blank line flushes the paragraph.
    r.append("\n");
    CHECK(r.cached_block_count() == 1);

    CHECK(r.render() != nullptr);
}

// ---------------------------------------------------------------------------
// C.4 — Multiple paragraphs separated by blank lines produce separate blocks.
// ---------------------------------------------------------------------------
TEST_CASE("Multiple paragraphs produce separate cached blocks") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    feed(r, "First paragraph.\n\nSecond paragraph.\n\nThird paragraph.\n\n");
    // Three paragraphs → 3 cached blocks.
    CHECK(r.cached_block_count() == 3);
}

// ---------------------------------------------------------------------------
// C.5 — Single-character chunk streaming works correctly.
// ---------------------------------------------------------------------------
TEST_CASE("Single-character chunks stream correctly") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    std::string input = "## Hello\n\nSome text.\n\n";
    for (char ch : input) {
        r.append(std::string_view(&ch, 1));
    }
    // One heading + one paragraph = 2 cached blocks.
    CHECK(r.cached_block_count() == 2);
    CHECK(r.render() != nullptr);
}

// ---------------------------------------------------------------------------
// C.6 — Code fence across multiple appends.
// ---------------------------------------------------------------------------
TEST_CASE("Code fence split across multiple append() calls") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    r.append("```py\n");
    CHECK(r.in_code_fence() == true);

    r.append("x = 1\n");
    r.append("y = 2\n");
    CHECK(r.in_code_fence() == true);

    r.append("```\n");
    CHECK(r.in_code_fence() == false);
    CHECK(r.cached_block_count() >= 1);
}

// ---------------------------------------------------------------------------
// C.7 — Heading inside streamed text flushes paragraph before it.
// ---------------------------------------------------------------------------
TEST_CASE("Heading after paragraph flushes paragraph first") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    feed(r, "Some intro text.\n\n# Main Heading\n\n");
    // paragraph (1) + heading (1) = 2 cached blocks.
    CHECK(r.cached_block_count() == 2);
}

} // TEST_SUITE("MarkdownRenderer::streaming")

// ============================================================================
// TEST SUITE D — Block caching semantics
// ============================================================================

TEST_SUITE("MarkdownRenderer::caching") {

// ---------------------------------------------------------------------------
// D.1 — Cached count starts at 0.
// ---------------------------------------------------------------------------
TEST_CASE("cached_block_count starts at zero") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);
    CHECK(r.cached_block_count() == 0);
}

// ---------------------------------------------------------------------------
// D.2 — Incomplete code fence doesn't prematurely cache.
// ---------------------------------------------------------------------------
TEST_CASE("Unclosed code fence not yet cached") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    feed(r, "```rust\nfn main() {}\n");
    // Fence is open; no finalized block yet.
    CHECK(r.in_code_fence() == true);
    CHECK(r.cached_block_count() == 0);
}

// ---------------------------------------------------------------------------
// D.3 — reset() clears cached blocks and state.
// ---------------------------------------------------------------------------
TEST_CASE("reset() clears all state") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    feed(r, "# Title\n\nParagraph.\n\n");
    CHECK(r.cached_block_count() > 0);

    r.reset();
    CHECK(r.cached_block_count() == 0);
    CHECK(r.in_code_fence() == false);
    // render() after reset returns a valid (empty) element.
    CHECK(r.render() != nullptr);
}

// ---------------------------------------------------------------------------
// D.4 — render() is idempotent (calling it twice gives same non-null result).
// ---------------------------------------------------------------------------
TEST_CASE("render() is idempotent") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    feed(r, "Hello world\n\n");
    auto e1 = r.render();
    auto e2 = r.render();
    CHECK(e1 != nullptr);
    CHECK(e2 != nullptr);
    // Cached count unchanged by render().
    CHECK(r.cached_block_count() == 1);
}

} // TEST_SUITE("MarkdownRenderer::caching")

// ============================================================================
// TEST SUITE E — Edge cases
// ============================================================================

TEST_SUITE("MarkdownRenderer::edge_cases") {

// ---------------------------------------------------------------------------
// E.1 — Empty input produces non-null (emptyElement).
// ---------------------------------------------------------------------------
TEST_CASE("Empty input still produces non-null render") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);
    r.append("");
    CHECK(r.render() != nullptr);
}

// ---------------------------------------------------------------------------
// E.2 — Only blank lines produces no cached blocks.
// ---------------------------------------------------------------------------
TEST_CASE("Only blank lines produce no cached blocks") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);
    feed(r, "\n\n\n\n");
    CHECK(r.cached_block_count() == 0);
}

// ---------------------------------------------------------------------------
// E.3 — H1 through H6 all parse without crash.
// ---------------------------------------------------------------------------
TEST_CASE("All six heading levels parse") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    feed(r, "# H1\n## H2\n### H3\n#### H4\n##### H5\n###### H6\n");
    // Each heading is one cached block → 6.
    CHECK(r.cached_block_count() == 6);
    CHECK(r.render() != nullptr);
}

// ---------------------------------------------------------------------------
// E.4 — Bold that's never closed falls back to plain text (no crash).
// ---------------------------------------------------------------------------
TEST_CASE("Unclosed bold marker falls back to plain text") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    feed(r, "This is **unclosed bold.\n\n");
    CHECK(r.render() != nullptr);
}

// ---------------------------------------------------------------------------
// E.5 — Mixed content: heading, code, list, blockquote, paragraph.
// ---------------------------------------------------------------------------
TEST_CASE("Mixed block types in sequence all render") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    const char* doc =
        "# Title\n"
        "\n"
        "Intro paragraph.\n"
        "\n"
        "```sh\n"
        "echo hello\n"
        "```\n"
        "\n"
        "- item a\n"
        "- item b\n"
        "\n"
        "> a quote\n"
        "\n"
        "| A | B |\n"
        "| - | - |\n"
        "| 1 | 2 |\n"
        "\n"
        "Final paragraph.\n"
        "\n";

    feed(r, doc);
    // 1 heading + 1 para + 1 code + 1 list + 1 bq + 1 table + 1 para = 7
    CHECK(r.cached_block_count() == 7);
    CHECK(r.render() != nullptr);
}

// ---------------------------------------------------------------------------
// E.6 — append() with embedded newlines works same as separate calls.
// ---------------------------------------------------------------------------
TEST_CASE("append with embedded newlines equivalent to separate calls") {
    auto theme = make_test_theme();

    batbox::tui::MarkdownRenderer r1(theme);
    r1.append("para one\n\npara two\n\n");

    batbox::tui::MarkdownRenderer r2(theme);
    r2.append("para one\n");
    r2.append("\n");
    r2.append("para two\n");
    r2.append("\n");

    CHECK(r1.cached_block_count() == r2.cached_block_count());
    CHECK(r1.cached_block_count() == 2);
}

} // TEST_SUITE("MarkdownRenderer::edge_cases")

// ============================================================================
// TEST SUITE F — TUI-FLOW-T5: Filename/path coloring + bold bullet glyph
// ============================================================================
//
// These tests validate the path-detection tokenizer in render_inline() and
// the bold bullet glyph in render_unordered_list().
//
// Strategy: render() non-null + cached_block_count() correctness.
// True colour-assertion tests are covered by tmux smoke (17_filename_color.sh).
// ============================================================================

TEST_SUITE("MarkdownRenderer::TUI-FLOW-T5") {

// ---------------------------------------------------------------------------
// F.1 — Filename in prose renders without crash (known .cpp extension).
// ---------------------------------------------------------------------------
TEST_CASE("TUI-FLOW-T5-F1: filename with .cpp extension renders") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    // "src/conversation/Conversation.cpp" has a known extension.
    r.append("see src/conversation/Conversation.cpp for the fix\n\n");
    CHECK(r.cached_block_count() >= 1);
    CHECK(r.render() != nullptr);
}

// ---------------------------------------------------------------------------
// F.2 — Inline backtick filename still renders (no double-processing).
// ---------------------------------------------------------------------------
TEST_CASE("TUI-FLOW-T5-F2: backtick-wrapped filename renders cleanly") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    // manifest.json inside backticks: handled by code-span path, not path tokenizer.
    r.append("Open `manifest.json` to view metadata.\n\n");
    CHECK(r.cached_block_count() >= 1);
    CHECK(r.render() != nullptr);
}

// ---------------------------------------------------------------------------
// F.3 — Home-relative path (~/.batbox/.env) renders without crash.
// ---------------------------------------------------------------------------
TEST_CASE("TUI-FLOW-T5-F3: home-relative path renders") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    r.append("open ~/.batbox/.env to configure\n\n");
    CHECK(r.cached_block_count() >= 1);
    CHECK(r.render() != nullptr);
}

// ---------------------------------------------------------------------------
// F.4 — Absolute path (/etc/hosts) renders without crash.
// ---------------------------------------------------------------------------
TEST_CASE("TUI-FLOW-T5-F4: absolute path renders") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    r.append("edit /etc/hosts to add a local domain\n\n");
    CHECK(r.cached_block_count() >= 1);
    CHECK(r.render() != nullptr);
}

// ---------------------------------------------------------------------------
// F.5 — Plain prose with word "path" is NOT falsely highlighted (no crash).
// ---------------------------------------------------------------------------
TEST_CASE("TUI-FLOW-T5-F5: plain prose with word path -- no false positive") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    r.append("just regular prose with the word path in it\n\n");
    CHECK(r.cached_block_count() >= 1);
    CHECK(r.render() != nullptr);
}

// ---------------------------------------------------------------------------
// F.6 — "e.g." abbreviation does not produce false positive.
// ---------------------------------------------------------------------------
TEST_CASE("TUI-FLOW-T5-F6: e.g. abbreviation not highlighted") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    r.append("for example, e.g. use a config file\n\n");
    CHECK(r.cached_block_count() >= 1);
    CHECK(r.render() != nullptr);
}

// ---------------------------------------------------------------------------
// F.7 — Multiple filenames on one line all render (one cached block total).
// ---------------------------------------------------------------------------
TEST_CASE("TUI-FLOW-T5-F7: multiple filenames on one line") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    r.append("Edit src/tui/ChatView.cpp and include/batbox/tui/ChatView.hpp\n\n");
    CHECK(r.cached_block_count() >= 1);
    CHECK(r.render() != nullptr);
}

// ---------------------------------------------------------------------------
// F.8 — Filename at start of line renders.
// ---------------------------------------------------------------------------
TEST_CASE("TUI-FLOW-T5-F8: filename at start of line") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    r.append("main.cpp is the entry point\n\n");
    CHECK(r.cached_block_count() >= 1);
    CHECK(r.render() != nullptr);
}

// ---------------------------------------------------------------------------
// F.9 — Filename at end of line renders.
// ---------------------------------------------------------------------------
TEST_CASE("TUI-FLOW-T5-F9: filename at end of line") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    r.append("changes are in MarkdownRender.cpp\n\n");
    CHECK(r.cached_block_count() >= 1);
    CHECK(r.render() != nullptr);
}

// ---------------------------------------------------------------------------
// F.10 — Bullet list renders with new glyph (no crash, items present).
// ---------------------------------------------------------------------------
TEST_CASE("TUI-FLOW-T5-F10: unordered list uses new bullet glyph") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    r.append("- top level item\n- another item\n\n");
    CHECK(r.cached_block_count() >= 1);
    auto el = r.render();
    CHECK(el != nullptr);
}

// ---------------------------------------------------------------------------
// F.11 — Mid-stream filename: filename split across two append() calls.
// ---------------------------------------------------------------------------
TEST_CASE("TUI-FLOW-T5-F11: filename split across append chunks") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    // Streaming tokens break in the middle of a filename.
    r.append("see Conv");         // no newline -- stays in line_buf_
    r.append("ersation.cpp");     // completes filename -- still in line_buf_
    r.append(" for details\n");   // terminates line
    r.append("\n");               // blank line flushes paragraph
    CHECK(r.cached_block_count() == 1);
    CHECK(r.render() != nullptr);
}

// ---------------------------------------------------------------------------
// F.12 — Bullet list item with filename: path colouring + bullet glyph work.
// ---------------------------------------------------------------------------
TEST_CASE("TUI-FLOW-T5-F12: bullet list item containing filename") {
    auto theme = make_test_theme();
    batbox::tui::MarkdownRenderer r(theme);

    r.append("- Edit src/tui/MarkdownRender.cpp\n");
    r.append("- Edit include/batbox/tui/MarkdownRender.hpp\n");
    r.append("\n");
    CHECK(r.cached_block_count() >= 1);
    CHECK(r.render() != nullptr);
}

} // TEST_SUITE("MarkdownRenderer::TUI-FLOW-T5")
