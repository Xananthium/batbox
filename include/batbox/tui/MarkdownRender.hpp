// include/batbox/tui/MarkdownRender.hpp
// ---------------------------------------------------------------------------
// batbox::tui::MarkdownRenderer — incremental markdown → ftxui::Element
//
// Design
// ------
// MarkdownRenderer is a stateful, incremental parser for a minimal markdown
// subset suited to streaming LLM output.  It is NOT a CommonMark-compliant
// parser; it covers the constructs that actually appear in assistant responses:
//
//   Block-level:
//     # H1  ## H2  ### H3  #### H4  ##### H5  ###### H6
//     ```[lang]\n ... \n```   (fenced code block)
//     > blockquote line(s)
//     - / * / + unordered list items
//     1. / 2. / ... ordered list items
//     | col | col |   (simple pipe table, no alignment row required)
//     blank line           (paragraph break / block separator)
//     plain paragraph text
//
//   Inline (applied on non-code lines):
//     **bold**   __bold__
//     *italic*   _italic_
//     `code span`
//     [label](url)          (rendered as label, url ignored — terminal)
//     remaining text rendered as plain fg
//
// Incremental / streaming contract
// ---------------------------------
// append(chunk) accepts an arbitrary text fragment (may contain embedded
// newlines, may end mid-line).  The renderer buffers the incomplete current
// line and only re-parses from the last block boundary.  Finalized blocks
// (those that ended before the current stream position) are cached as
// ftxui::Elements and never reparsed.
//
// render() assembles cached_elements_ + current open block, returning a
// vbox suitable for embedding in a parent layout.
//
// Theme integration
// -----------------
// MarkdownRenderer receives a ThemeRef at construction.  Block types map to
// ThemeRole:
//   H1–H6      → AccentMagenta (h1/h2), AccentCyan (h3/h4), Fg (h5/h6)
//   code block → CodeBg background, Fg foreground
//   blockquote → Muted foreground
//   list bullet/number → AccentCyan
//   bold       → bold decorator + Fg
//   italic     → italic decorator + Fg
//   inline code → CodeBg background + Fg
//   links      → AccentCyan + underlined
//   plain      → Fg
// ---------------------------------------------------------------------------
#pragma once

#include <batbox/theme/Theme.hpp>

#include <ftxui/dom/elements.hpp>

#include <string>
#include <string_view>
#include <vector>
#include <cstdint>

namespace batbox::tui {

// ============================================================================
// MarkdownRenderer
// ============================================================================

/// Stateful incremental markdown renderer.
///
/// Typical usage (streaming):
/// @code
///   batbox::tui::MarkdownRenderer md(theme);
///   // … called from FTXUI event handler on each token event:
///   md.append(chunk);
///   auto element = md.render();   // embed in parent vbox
/// @endcode
///
/// Thread safety: NOT thread-safe.  All calls must come from the same thread
/// (the FTXUI render/event thread).  SSE worker threads must post events and
/// let the UI thread call append().
class MarkdownRenderer {
public:
    // -----------------------------------------------------------------------
    // Construction
    // -----------------------------------------------------------------------

    /// Construct with a theme reference.
    ///
    /// @param theme  Active theme; must outlive this MarkdownRenderer instance.
    explicit MarkdownRenderer(const batbox::theme::Theme& theme);

    // Non-copyable, moveable.
    MarkdownRenderer(const MarkdownRenderer&)            = delete;
    MarkdownRenderer& operator=(const MarkdownRenderer&) = delete;
    MarkdownRenderer(MarkdownRenderer&&)                 = default;
    MarkdownRenderer& operator=(MarkdownRenderer&&)      = delete;

    ~MarkdownRenderer() = default;

    // -----------------------------------------------------------------------
    // Streaming interface
    // -----------------------------------------------------------------------

    /// Append a text chunk to the render buffer.
    ///
    /// @param text  Arbitrary fragment — may be a single character, may
    ///              contain multiple newlines, may end mid-line.  Newlines
    ///              are the only mandatory line terminators recognised.
    ///
    /// Finalized lines (lines ending with '\n') are parsed immediately.
    /// Completed blocks (code fences closed, blank-line-separated paragraphs,
    /// etc.) are moved to cached_elements_.  Only the tail since the last
    /// block boundary is reparsed on subsequent appends.
    void append(std::string_view text);

    /// Reset all state — useful when a new assistant turn starts.
    void reset();

    // -----------------------------------------------------------------------
    // Render interface
    // -----------------------------------------------------------------------

    /// Produce an ftxui::Element representing the full rendered markdown.
    ///
    /// Assembles cached_elements_ followed by the current open-block render.
    /// Returns ftxui::emptyElement() if no content has been appended yet.
    ///
    /// Calling render() is read-only with respect to parse state; it is safe
    /// to call repeatedly without side effects.
    [[nodiscard]]
    ftxui::Element render() const;

    // -----------------------------------------------------------------------
    // Inspection (useful for tests)
    // -----------------------------------------------------------------------

    /// Number of finalized (cached) block elements accumulated so far.
    [[nodiscard]] std::size_t cached_block_count() const noexcept;

    /// True if we are currently inside a fenced code block (``` ... ```).
    [[nodiscard]] bool in_code_fence() const noexcept;

private:
    // -----------------------------------------------------------------------
    // Internal types
    // -----------------------------------------------------------------------

    enum class BlockKind : uint8_t {
        Paragraph,
        Heading1, Heading2, Heading3, Heading4, Heading5, Heading6,
        CodeFence,
        Blockquote,
        UnorderedList,
        OrderedList,
        Table,
    };

    struct Block {
        BlockKind          kind   = BlockKind::Paragraph;
        std::string        lang;       ///< For CodeFence: language tag (may be empty)
        std::vector<std::string> lines; ///< Accumulated lines (without trailing \n)
        int                list_start = 1; ///< First number for OrderedList
    };

    // -----------------------------------------------------------------------
    // Render helpers
    // -----------------------------------------------------------------------

    /// Render a finalised Block into an ftxui::Element.
    [[nodiscard]]
    ftxui::Element render_block(const Block& block) const;

    /// Render a heading line with the appropriate colour and weight.
    [[nodiscard]]
    ftxui::Element render_heading(const std::string& text, int level) const;

    /// Render a fenced code block (all lines, monospace, code_bg background).
    [[nodiscard]]
    ftxui::Element render_code_fence(const Block& block) const;

    /// Render a blockquote block (muted, prefixed with "│ ").
    [[nodiscard]]
    ftxui::Element render_blockquote(const Block& block) const;

    /// Render an unordered list block.
    [[nodiscard]]
    ftxui::Element render_unordered_list(const Block& block) const;

    /// Render an ordered list block.
    [[nodiscard]]
    ftxui::Element render_ordered_list(const Block& block) const;

    /// Render a table block.
    [[nodiscard]]
    ftxui::Element render_table(const Block& block) const;

    /// Render a paragraph block (one or more lines, inline styles applied).
    [[nodiscard]]
    ftxui::Element render_paragraph(const Block& block) const;

    /// Parse inline markdown on a single line and return a horizontal box of
    /// styled ftxui Elements.
    [[nodiscard]]
    ftxui::Element render_inline(const std::string& line) const;

    // -----------------------------------------------------------------------
    // Parse helpers
    // -----------------------------------------------------------------------

    /// Process one complete line (without the trailing '\n').
    void process_line(std::string_view line);

    /// Finalise the current open block: render it and push to cached_elements_.
    void flush_open_block();

    /// Detect the BlockKind that a fresh line implies (when not inside a fence).
    static BlockKind detect_block_kind(std::string_view line);

    /// Strip the block-level prefix from a line (e.g. strip "# " from headings,
    /// "> " from blockquotes, "- " from list items) and return the content.
    static std::string strip_block_prefix(std::string_view line, BlockKind kind);

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------

    const batbox::theme::Theme& theme_;

    /// Finalized, fully rendered block elements (never reparsed).
    ftxui::Elements cached_elements_;

    /// The block currently being assembled from incoming lines.
    Block open_block_;

    /// True when we are inside a ``` ... ``` code fence.
    bool in_code_fence_ = false;

    /// Partial line buffer — holds text received after the last '\n'.
    std::string line_buf_;
};

} // namespace batbox::tui
