// include/batbox/tui/InputSegment.hpp
// ---------------------------------------------------------------------------
// InputSegment — variant type for the InputBar segment-model buffer.
//
// The InputBar buffer is represented as a std::vector<InputSegment> rather
// than a flat std::string.  Each segment is one of:
//
//   Text  { std::string body }
//       — ordinary typed text, may contain '\n' (from Shift+Enter).
//         Cursor can navigate byte-by-byte within a Text segment.
//
//   Paste { int id, std::string body, int line_count, int char_count }
//       — a bracketed-paste block.  Rendered as a one-line chip:
//           "[Pasted text #N +M lines]"  when M > 0
//           "[Pasted text #N (X chars)]" when M == 0
//         ATOMIC: cursor cannot enter the segment; arrow keys skip it as one
//         unit; a single Backspace deletes the entire segment.
//
// SegCursor — cursor position within the segment vector.
//   seg_idx   — index into segments vector
//   byte_off  — byte offset within a Text segment body
//               (always 0 for Paste segments; the cursor sits before them)
//
// TUI-FIX-T4 blueprint contract.
// ---------------------------------------------------------------------------
#pragma once

#include <string>
#include <variant>

namespace batbox::tui {

// ---------------------------------------------------------------------------
// TextSegment — inline editable text
// ---------------------------------------------------------------------------
struct TextSegment {
    std::string body;
};

// ---------------------------------------------------------------------------
// PasteSegment — atomic bracketed-paste block
// ---------------------------------------------------------------------------
struct PasteSegment {
    int         id{0};         ///< Per-session paste counter, starting at 1.
    std::string body;          ///< Full original paste content.
    int         line_count{0}; ///< Number of '\n' characters in body.
    int         char_count{0}; ///< UTF-8 character count of body.
};

// ---------------------------------------------------------------------------
// InputSegment — the variant
// ---------------------------------------------------------------------------
using InputSegment = std::variant<TextSegment, PasteSegment>;

// ---------------------------------------------------------------------------
// SegCursor — cursor position within the segment vector
// ---------------------------------------------------------------------------
struct SegCursor {
    std::size_t seg_idx{0};  ///< Index into segments vector (0-based).
    std::size_t byte_off{0}; ///< Byte offset within the Text segment at seg_idx.
                              ///< Always 0 when pointing at a Paste segment or
                              ///< when seg_idx == segments.size() (end-of-buffer).
};

} // namespace batbox::tui
