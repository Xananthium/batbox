// src/tui/MarkdownRender.cpp
// ---------------------------------------------------------------------------
// batbox::tui::MarkdownRenderer — implementation
//
// Parses a minimal markdown subset suitable for streaming LLM output.
// See include/batbox/tui/MarkdownRender.hpp for full design notes.
// ---------------------------------------------------------------------------
#include <batbox/tui/MarkdownRender.hpp>
#include <batbox/tui/ThemeApply.hpp>

#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstddef>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace batbox::tui {

// ============================================================================
// Construction
// ============================================================================

MarkdownRenderer::MarkdownRenderer(const batbox::theme::Theme& theme)
    : theme_(theme)
{
    // open_block_ starts as an empty Paragraph ready to receive lines.
    open_block_.kind = BlockKind::Paragraph;
}

// ============================================================================
// Streaming interface
// ============================================================================

void MarkdownRenderer::append(std::string_view text) {
    // Walk through the incoming chunk, accumulating characters into line_buf_.
    // Each time we hit a '\n', process the completed line.
    for (char ch : text) {
        if (ch == '\n') {
            process_line(line_buf_);
            line_buf_.clear();
        } else {
            line_buf_ += ch;
        }
    }
    // Any remaining text in line_buf_ is an incomplete line — kept for next
    // append().  No re-parse needed until the line is terminated.
}

void MarkdownRenderer::reset() {
    cached_elements_.clear();
    open_block_ = Block{};
    open_block_.kind = BlockKind::Paragraph;
    in_code_fence_ = false;
    line_buf_.clear();
}

// ============================================================================
// Render interface
// ============================================================================

ftxui::Element MarkdownRenderer::render() const {
    if (cached_elements_.empty() && open_block_.lines.empty() && line_buf_.empty()) {
        return ftxui::emptyElement();
    }

    // Build the final element list: cached + in-progress open block.
    ftxui::Elements all;
    all.reserve(cached_elements_.size() + 1);
    for (const auto& el : cached_elements_) {
        all.push_back(el);
    }

    // Render the open block (possibly incomplete) on-the-fly.
    // We may need to include the partial line_buf_ too.
    Block current_view = open_block_;
    if (!line_buf_.empty()) {
        current_view.lines.push_back(line_buf_);
    }

    if (!current_view.lines.empty()) {
        all.push_back(render_block(current_view));
    }

    if (all.empty()) {
        return ftxui::emptyElement();
    }
    return ftxui::vbox(std::move(all));
}

// ============================================================================
// Inspection
// ============================================================================

std::size_t MarkdownRenderer::cached_block_count() const noexcept {
    return cached_elements_.size();
}

bool MarkdownRenderer::in_code_fence() const noexcept {
    return in_code_fence_;
}

// ============================================================================
// Internal: line processing
// ============================================================================

void MarkdownRenderer::process_line(std::string_view line) {
    // -----------------------------------------------------------------
    // CASE 1: Inside a fenced code block.
    //         Only closing ``` terminates the fence; everything else is
    //         raw code content.
    // -----------------------------------------------------------------
    if (in_code_fence_) {
        // Detect closing fence: line is exactly ``` (optionally preceded by
        // whitespace).
        std::string_view trimmed = line;
        while (!trimmed.empty() && trimmed.front() == ' ') {
            trimmed.remove_prefix(1);
        }
        if (trimmed.size() >= 3 &&
            trimmed[0] == '`' && trimmed[1] == '`' && trimmed[2] == '`') {
            // Closing fence found — finalise this code block.
            in_code_fence_ = false;
            flush_open_block();
        } else {
            // Still inside: accumulate as raw code line.
            open_block_.lines.push_back(std::string(line));
        }
        return;
    }

    // -----------------------------------------------------------------
    // CASE 2: Opening fence detection.
    //         A line starting with ``` (with optional leading spaces and
    //         optional language tag) starts a code block.
    // -----------------------------------------------------------------
    {
        std::string_view trimmed = line;
        while (!trimmed.empty() && trimmed.front() == ' ') {
            trimmed.remove_prefix(1);
        }
        if (trimmed.size() >= 3 &&
            trimmed[0] == '`' && trimmed[1] == '`' && trimmed[2] == '`') {
            // Flush whatever was open before.
            flush_open_block();
            // Start a new code-fence block.
            open_block_.kind = BlockKind::CodeFence;
            // Extract language tag (everything after the three backticks).
            std::string_view lang_tag = trimmed.substr(3);
            while (!lang_tag.empty() && (lang_tag.front() == ' ' || lang_tag.front() == '\t')) {
                lang_tag.remove_prefix(1);
            }
            open_block_.lang = std::string(lang_tag);
            in_code_fence_ = true;
            return;
        }
    }

    // -----------------------------------------------------------------
    // CASE 3: Blank line — acts as a block separator.
    // -----------------------------------------------------------------
    {
        bool blank = true;
        for (char ch : line) {
            if (ch != ' ' && ch != '\t') { blank = false; break; }
        }
        if (blank) {
            if (!open_block_.lines.empty()) {
                flush_open_block();
            }
            // Don't start a new block for blank lines; next content line will.
            return;
        }
    }

    // -----------------------------------------------------------------
    // CASE 4: Detect block kind for this line.
    // -----------------------------------------------------------------
    BlockKind new_kind = detect_block_kind(line);

    // -----------------------------------------------------------------
    // CASE 5: Continuation vs. new block.
    //
    // Rules:
    //  - Paragraphs accumulate consecutive non-blank, non-heading lines.
    //  - Blockquotes, lists, and tables accumulate same-kind lines.
    //  - Headings are single-line blocks (always flush before + after).
    //  - A change of block kind → flush existing, start new.
    // -----------------------------------------------------------------
    bool is_heading = (new_kind >= BlockKind::Heading1 &&
                       new_kind <= BlockKind::Heading6);

    if (is_heading) {
        // Headings are single-line — flush open block, emit heading, flush.
        flush_open_block();
        open_block_.kind = new_kind;
        open_block_.lines.push_back(strip_block_prefix(line, new_kind));
        flush_open_block();
        return;
    }

    // Non-heading: check if we need to start a new block.
    bool same_kind = (open_block_.kind == new_kind) ||
                     (open_block_.kind == BlockKind::Paragraph &&
                      new_kind == BlockKind::Paragraph);

    if (!same_kind && !open_block_.lines.empty()) {
        flush_open_block();
        open_block_.kind = new_kind;
    } else if (open_block_.lines.empty()) {
        open_block_.kind = new_kind;
    }

    open_block_.lines.push_back(strip_block_prefix(line, new_kind));
}

void MarkdownRenderer::flush_open_block() {
    if (!open_block_.lines.empty()) {
        cached_elements_.push_back(render_block(open_block_));
    }
    open_block_ = Block{};
    open_block_.kind = BlockKind::Paragraph;
}

// ============================================================================
// Internal: block kind detection
// ============================================================================

MarkdownRenderer::BlockKind MarkdownRenderer::detect_block_kind(std::string_view line) {
    // Heading: 1–6 '#' characters followed by a space.
    if (!line.empty() && line[0] == '#') {
        int level = 0;
        while (level < static_cast<int>(line.size()) && line[static_cast<std::size_t>(level)] == '#') {
            ++level;
        }
        if (level <= 6 &&
            level < static_cast<int>(line.size()) &&
            line[static_cast<std::size_t>(level)] == ' ') {
            switch (level) {
                case 1: return BlockKind::Heading1;
                case 2: return BlockKind::Heading2;
                case 3: return BlockKind::Heading3;
                case 4: return BlockKind::Heading4;
                case 5: return BlockKind::Heading5;
                case 6: return BlockKind::Heading6;
                default: break;
            }
        }
    }

    // Blockquote: starts with '>' (optional leading space).
    if (!line.empty()) {
        std::size_t i = 0;
        while (i < line.size() && line[i] == ' ') ++i;
        if (i < line.size() && line[i] == '>') {
            return BlockKind::Blockquote;
        }
    }

    // Unordered list: starts with "- ", "* ", or "+ " (optional leading spaces).
    if (!line.empty()) {
        std::size_t i = 0;
        while (i < line.size() && line[i] == ' ') ++i;
        if (i + 1 < line.size() &&
            (line[i] == '-' || line[i] == '*' || line[i] == '+') &&
            line[i + 1] == ' ') {
            return BlockKind::UnorderedList;
        }
    }

    // Ordered list: starts with digits followed by ". ".
    if (!line.empty()) {
        std::size_t i = 0;
        while (i < line.size() && line[i] == ' ') ++i;
        std::size_t j = i;
        while (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j]))) ++j;
        if (j > i && j + 1 < line.size() && line[j] == '.' && line[j + 1] == ' ') {
            return BlockKind::OrderedList;
        }
    }

    // Table: contains '|' (simple heuristic).
    if (line.find('|') != std::string_view::npos) {
        return BlockKind::Table;
    }

    return BlockKind::Paragraph;
}

std::string MarkdownRenderer::strip_block_prefix(std::string_view line, BlockKind kind) {
    switch (kind) {
        case BlockKind::Heading1:
        case BlockKind::Heading2:
        case BlockKind::Heading3:
        case BlockKind::Heading4:
        case BlockKind::Heading5:
        case BlockKind::Heading6: {
            // Strip leading '#' characters and the following space.
            int level = 0;
            while (level < static_cast<int>(line.size()) && line[static_cast<std::size_t>(level)] == '#') {
                ++level;
            }
            // Skip the space after hashes.
            if (level < static_cast<int>(line.size()) && line[static_cast<std::size_t>(level)] == ' ') {
                ++level;
            }
            return std::string(line.substr(static_cast<std::size_t>(level)));
        }

        case BlockKind::Blockquote: {
            // Strip leading whitespace, '>', and optional single space.
            std::size_t i = 0;
            while (i < line.size() && line[i] == ' ') ++i;
            if (i < line.size() && line[i] == '>') ++i;
            if (i < line.size() && line[i] == ' ') ++i;
            return std::string(line.substr(i));
        }

        case BlockKind::UnorderedList: {
            // Strip leading whitespace, bullet char, and mandatory space.
            std::size_t i = 0;
            while (i < line.size() && line[i] == ' ') ++i;
            if (i < line.size() && (line[i] == '-' || line[i] == '*' || line[i] == '+')) ++i;
            if (i < line.size() && line[i] == ' ') ++i;
            return std::string(line.substr(i));
        }

        case BlockKind::OrderedList: {
            // Strip leading whitespace, digits, '.', and mandatory space.
            std::size_t i = 0;
            while (i < line.size() && line[i] == ' ') ++i;
            while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) ++i;
            if (i < line.size() && line[i] == '.') ++i;
            if (i < line.size() && line[i] == ' ') ++i;
            return std::string(line.substr(i));
        }

        // Paragraphs, tables, and code (handled elsewhere) keep content as-is.
        default:
            return std::string(line);
    }
}

// ============================================================================
// Internal: block rendering
// ============================================================================

ftxui::Element MarkdownRenderer::render_block(const Block& block) const {
    switch (block.kind) {
        case BlockKind::Heading1:  return render_heading(block.lines.empty() ? "" : block.lines[0], 1);
        case BlockKind::Heading2:  return render_heading(block.lines.empty() ? "" : block.lines[0], 2);
        case BlockKind::Heading3:  return render_heading(block.lines.empty() ? "" : block.lines[0], 3);
        case BlockKind::Heading4:  return render_heading(block.lines.empty() ? "" : block.lines[0], 4);
        case BlockKind::Heading5:  return render_heading(block.lines.empty() ? "" : block.lines[0], 5);
        case BlockKind::Heading6:  return render_heading(block.lines.empty() ? "" : block.lines[0], 6);
        case BlockKind::CodeFence:     return render_code_fence(block);
        case BlockKind::Blockquote:    return render_blockquote(block);
        case BlockKind::UnorderedList: return render_unordered_list(block);
        case BlockKind::OrderedList:   return render_ordered_list(block);
        case BlockKind::Table:         return render_table(block);
        case BlockKind::Paragraph:     return render_paragraph(block);
    }
    // Unreachable — suppress compiler warning.
    return ftxui::emptyElement();
}

ftxui::Element MarkdownRenderer::render_heading(const std::string& text, int level) const {
    ftxui::Color col;
    if (level <= 2) {
        col = color_for(theme_, ThemeRole::AccentMagenta);
    } else if (level <= 4) {
        col = color_for(theme_, ThemeRole::AccentCyan);
    } else {
        col = color_for(theme_, ThemeRole::Fg);
    }

    // H1 and H2 get bold; h3+ get just colour.
    ftxui::Element el = ftxui::text(text);
    if (level <= 2) {
        el = ftxui::bold(el);
    }
    el = el | ftxui::color(col);
    return el;
}

ftxui::Element MarkdownRenderer::render_code_fence(const Block& block) const {
    ftxui::Color bg  = color_for(theme_, ThemeRole::CodeBg);
    ftxui::Color fg  = color_for(theme_, ThemeRole::Fg);

    ftxui::Elements lines;
    lines.reserve(block.lines.size());
    for (const auto& line : block.lines) {
        lines.push_back(
            ftxui::text(line) | ftxui::color(fg) | ftxui::bgcolor(bg)
        );
    }
    if (lines.empty()) {
        return ftxui::text("") | ftxui::bgcolor(bg);
    }
    return ftxui::vbox(std::move(lines));
}

ftxui::Element MarkdownRenderer::render_blockquote(const Block& block) const {
    ftxui::Color muted = color_for(theme_, ThemeRole::Muted);

    ftxui::Elements lines;
    lines.reserve(block.lines.size());
    for (const auto& line : block.lines) {
        // Render inline styles on blockquote text, then tint with muted.
        auto inline_el = render_inline(line);
        lines.push_back(
            ftxui::hbox({
                ftxui::text("│ ") | ftxui::color(muted),
                std::move(inline_el)
            })
        );
    }
    if (lines.empty()) return ftxui::emptyElement();
    return ftxui::vbox(std::move(lines));
}

ftxui::Element MarkdownRenderer::render_unordered_list(const Block& block) const {
    ftxui::Color bullet_col = color_for(theme_, ThemeRole::AccentCyan);

    ftxui::Elements items;
    items.reserve(block.lines.size());
    for (const auto& line : block.lines) {
        items.push_back(
            ftxui::hbox({
                ftxui::bold(ftxui::text("▸ ")) | ftxui::color(bullet_col),
                render_inline(line)
            })
        );
    }
    if (items.empty()) return ftxui::emptyElement();
    return ftxui::vbox(std::move(items));
}

ftxui::Element MarkdownRenderer::render_ordered_list(const Block& block) const {
    ftxui::Color num_col = color_for(theme_, ThemeRole::AccentCyan);

    ftxui::Elements items;
    items.reserve(block.lines.size());
    for (std::size_t i = 0; i < block.lines.size(); ++i) {
        int num = block.list_start + static_cast<int>(i);
        items.push_back(
            ftxui::hbox({
                ftxui::text(std::to_string(num) + ". ") | ftxui::color(num_col),
                render_inline(block.lines[i])
            })
        );
    }
    if (items.empty()) return ftxui::emptyElement();
    return ftxui::vbox(std::move(items));
}

ftxui::Element MarkdownRenderer::render_table(const Block& block) const {
    // Simple table renderer: split on '|', trim cells, render as padded text.
    // No alignment row required — just render every row.
    ftxui::Color fg   = color_for(theme_, ThemeRole::Fg);
    ftxui::Color muted = color_for(theme_, ThemeRole::Muted);

    // First pass: collect cells per row and compute column widths.
    std::vector<std::vector<std::string>> rows;
    rows.reserve(block.lines.size());
    std::vector<std::size_t> col_widths;

    auto split_row = [](const std::string& line) -> std::vector<std::string> {
        std::vector<std::string> cells;
        std::istringstream ss(line);
        std::string cell;
        while (std::getline(ss, cell, '|')) {
            // Trim whitespace.
            auto start = cell.find_first_not_of(" \t");
            auto end   = cell.find_last_not_of(" \t");
            if (start == std::string::npos) {
                cells.push_back("");
            } else {
                cells.push_back(cell.substr(start, end - start + 1));
            }
        }
        // Remove empty leading/trailing cells that result from '|' at line edges.
        if (!cells.empty() && cells.front().empty()) cells.erase(cells.begin());
        if (!cells.empty() && cells.back().empty())  cells.pop_back();
        return cells;
    };

    auto is_separator_row = [](const std::vector<std::string>& cells) -> bool {
        for (const auto& c : cells) {
            bool sep = true;
            for (char ch : c) {
                if (ch != '-' && ch != ':' && ch != ' ') { sep = false; break; }
            }
            if (!sep) return false;
        }
        return !cells.empty();
    };

    for (const auto& line : block.lines) {
        auto cells = split_row(line);
        if (is_separator_row(cells)) continue; // skip alignment row
        // Expand col_widths if needed.
        while (col_widths.size() < cells.size()) col_widths.push_back(0);
        for (std::size_t ci = 0; ci < cells.size(); ++ci) {
            col_widths[ci] = std::max(col_widths[ci], cells[ci].size());
        }
        rows.push_back(std::move(cells));
    }

    if (rows.empty()) return ftxui::emptyElement();

    // Second pass: render each row as an hbox of padded cells.
    ftxui::Elements rendered_rows;
    rendered_rows.reserve(rows.size());
    for (std::size_t ri = 0; ri < rows.size(); ++ri) {
        const auto& cells = rows[ri];
        ftxui::Elements cell_els;
        bool is_header = (ri == 0);
        for (std::size_t ci = 0; ci < cells.size(); ++ci) {
            std::size_t width = (ci < col_widths.size()) ? col_widths[ci] : cells[ci].size();
            // Pad cell to column width.
            std::string padded = cells[ci];
            while (padded.size() < width) padded += ' ';
            ftxui::Element cell_el = ftxui::text(padded);
            if (is_header) {
                cell_el = ftxui::bold(cell_el) | ftxui::color(fg);
            } else {
                cell_el = cell_el | ftxui::color(fg);
            }
            cell_els.push_back(std::move(cell_el));
            // Column separator (except after last cell).
            if (ci + 1 < (col_widths.empty() ? cells.size() : col_widths.size())) {
                cell_els.push_back(ftxui::text(" │ ") | ftxui::color(muted));
            }
        }
        rendered_rows.push_back(ftxui::hbox(std::move(cell_els)));
        // Separator line under header row.
        if (is_header && rows.size() > 1) {
            std::string sep_line;
            for (std::size_t ci = 0; ci < col_widths.size(); ++ci) {
                sep_line += std::string(col_widths[ci], '-');
                if (ci + 1 < col_widths.size()) sep_line += "-+-";
            }
            if (sep_line.empty()) {
                // Fallback: just a horizontal separator.
                rendered_rows.push_back(ftxui::separator() | ftxui::color(muted));
            } else {
                rendered_rows.push_back(ftxui::text(sep_line) | ftxui::color(muted));
            }
        }
    }

    return ftxui::vbox(std::move(rendered_rows));
}

ftxui::Element MarkdownRenderer::render_paragraph(const Block& block) const {
    if (block.lines.empty()) return ftxui::emptyElement();

    ftxui::Elements line_els;
    line_els.reserve(block.lines.size());
    for (const auto& line : block.lines) {
        line_els.push_back(render_inline(line));
    }
    if (line_els.size() == 1) return line_els[0];
    return ftxui::vbox(std::move(line_els));
}

// ============================================================================
// Internal: inline styling
// ============================================================================

ftxui::Element MarkdownRenderer::render_inline(const std::string& line) const {
    // Walk the line character by character, collecting spans.
    // Emit each span as a styled ftxui::text element.
    // Spans recognised (in order of priority at each position):
    //   **text** or __text__ → bold
    //   *text*  or _text_   → italic
    //   `code`               → code span (code_bg + fg)
    //   [label](url)         → label only, AccentCyan + underlined
    //   filename/path token  → AccentCyan (detected by extension or leading / or ~/)
    //   plain text           → fg colour

    ftxui::Color fg       = color_for(theme_, ThemeRole::Fg);
    ftxui::Color code_bg  = color_for(theme_, ThemeRole::CodeBg);
    ftxui::Color link_col = color_for(theme_, ThemeRole::AccentCyan);
    ftxui::Color path_col = color_for(theme_, ThemeRole::AccentCyan);

    ftxui::Elements spans;
    std::size_t i = 0;
    std::size_t n = line.size();
    std::string plain_buf;

    // Known filename extensions for path detection.
    static const char* const kKnownExts[] = {
        ".cpp", ".hpp", ".c", ".h", ".py", ".ts", ".tsx", ".js", ".jsx",
        ".md", ".yaml", ".yml", ".json", ".toml", ".sh", ".rs", ".go",
        ".swift", ".kt", ".java", ".lock", ".html", ".css", ".txt", nullptr
    };

    // Returns true if `token` ends with a known extension.
    auto has_known_ext = [](const std::string& token) -> bool {
        static const char* const kExts[] = {
            ".cpp", ".hpp", ".c", ".h", ".py", ".ts", ".tsx", ".js", ".jsx",
            ".md", ".yaml", ".yml", ".json", ".toml", ".sh", ".rs", ".go",
            ".swift", ".kt", ".java", ".lock", ".html", ".css", ".txt", nullptr
        };
        for (int ei = 0; kExts[ei] != nullptr; ++ei) {
            const char* ext = kExts[ei];
            std::size_t elen = std::char_traits<char>::length(ext);
            if (token.size() >= elen &&
                token.compare(token.size() - elen, elen, ext) == 0) {
                return true;
            }
        }
        return false;
    };

    // Returns true if `token` is an absolute path (/…) or home-relative (~/).
    auto is_abs_or_home_path = [](const std::string& token) -> bool {
        if (token.empty()) return false;
        if (token[0] == '/') return true;
        if (token.size() >= 2 && token[0] == '~' && token[1] == '/') return true;
        return false;
    };

    // Emit a plain-text buffer, scanning it for filename/path tokens and
    // splitting it into alternating plain + path-coloured spans.
    // Streaming-safe: no lookahead beyond `plain_buf` (a single line fragment).
    //
    // Token characters: [A-Za-z0-9._/~-]
    // A token qualifies for path colour if:
    //   (a) it has a known extension, OR
    //   (b) it starts with '/' or '~/'
    // Relative bare paths (word/word without extension) are NOT highlighted
    // to avoid false positives on prose.
    auto flush_plain = [&]() {
        if (plain_buf.empty()) return;

        // Tokenise plain_buf into segments: path-tokens vs. prose fragments.
        // Single-pass, state-machine, no regex.
        const std::string& s = plain_buf;
        std::size_t sn = s.size();
        std::size_t si = 0;
        std::string prose_seg;  // accumulated non-token text

        auto emit_prose = [&]() {
            if (!prose_seg.empty()) {
                spans.push_back(ftxui::text(prose_seg) | ftxui::color(fg));
                prose_seg.clear();
            }
        };

        while (si < sn) {
            // Check if this character can start a path/filename token.
            char ch = s[si];
            bool can_start = std::isalnum(static_cast<unsigned char>(ch)) ||
                             ch == '/' || ch == '~' || ch == '.' || ch == '_';
            if (!can_start) {
                prose_seg += ch;
                ++si;
                continue;
            }

            // Accumulate token characters.
            std::size_t tok_start = si;
            std::string token;
            while (si < sn) {
                char tc = s[si];
                bool in_tok = std::isalnum(static_cast<unsigned char>(tc)) ||
                              tc == '.' || tc == '/' || tc == '_' ||
                              tc == '-' || tc == '~' || tc == ':';
                if (!in_tok) break;
                token += tc;
                ++si;
            }

            // Strip trailing colons/dots that are punctuation, not part of token.
            while (!token.empty() && (token.back() == '.' || token.back() == ':')) {
                token.pop_back();
                --si;
            }

            if (token.empty()) {
                prose_seg += s[tok_start];
                si = tok_start + 1;
                continue;
            }

            // Decide: is this token a filename/path?
            bool is_path = has_known_ext(token) || is_abs_or_home_path(token);

            if (is_path) {
                emit_prose();
                spans.push_back(ftxui::text(token) | ftxui::color(path_col));
            } else {
                prose_seg += token;
            }
        }

        emit_prose();
        plain_buf.clear();
    };

    (void)kKnownExts; // suppress unused-variable warning

    while (i < n) {
        // --- Bold: ** or __
        if (i + 1 < n &&
            ((line[i] == '*' && line[i + 1] == '*') ||
             (line[i] == '_' && line[i + 1] == '_'))) {
            char delim = line[i];
            std::size_t close = line.find(std::string(2, delim), i + 2);
            if (close != std::string::npos) {
                flush_plain();
                std::string content = line.substr(i + 2, close - (i + 2));
                spans.push_back(
                    ftxui::bold(ftxui::text(content)) | ftxui::color(fg)
                );
                i = close + 2;
                continue;
            }
        }

        // --- Italic: * or _ (single)
        if (i < n &&
            (line[i] == '*' || (line[i] == '_' && (i == 0 || line[i - 1] == ' ')))) {
            char delim = line[i];
            // Find matching closing delimiter (single), not immediately following.
            std::size_t close = std::string::npos;
            for (std::size_t k = i + 1; k < n; ++k) {
                if (line[k] == delim) {
                    // For '_', require either end-of-string or followed by space/punct.
                    if (delim == '_' && k + 1 < n &&
                        line[k + 1] != ' ' && !std::ispunct(static_cast<unsigned char>(line[k + 1]))) {
                        continue;
                    }
                    close = k;
                    break;
                }
            }
            if (close != std::string::npos && close > i + 1) {
                flush_plain();
                std::string content = line.substr(i + 1, close - (i + 1));
                spans.push_back(
                    ftxui::italic(ftxui::text(content)) | ftxui::color(fg)
                );
                i = close + 1;
                continue;
            }
        }

        // --- Inline code: `code`
        if (line[i] == '`') {
            std::size_t close = line.find('`', i + 1);
            if (close != std::string::npos) {
                flush_plain();
                std::string content = line.substr(i + 1, close - (i + 1));
                spans.push_back(
                    ftxui::text(content) | ftxui::color(fg) | ftxui::bgcolor(code_bg)
                );
                i = close + 1;
                continue;
            }
        }

        // --- Link: [label](url)
        if (line[i] == '[') {
            std::size_t label_end = line.find(']', i + 1);
            if (label_end != std::string::npos &&
                label_end + 1 < n && line[label_end + 1] == '(' ) {
                std::size_t url_end = line.find(')', label_end + 2);
                if (url_end != std::string::npos) {
                    flush_plain();
                    std::string label = line.substr(i + 1, label_end - (i + 1));
                    spans.push_back(
                        ftxui::underlined(ftxui::text(label)) | ftxui::color(link_col)
                    );
                    i = url_end + 1;
                    continue;
                }
            }
        }

        // --- Plain character
        plain_buf += line[i];
        ++i;
    }

    flush_plain();

    if (spans.empty()) {
        return ftxui::text("") | ftxui::color(fg);
    }
    if (spans.size() == 1) {
        return spans[0];
    }
    return ftxui::hbox(std::move(spans));
}

} // namespace batbox::tui
