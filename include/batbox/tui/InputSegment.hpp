// include/batbox/tui/InputSegment.hpp
// ---------------------------------------------------------------------------
// InputSegment — tagged-union type for the InputBar segment-model buffer.
//
// G1 (TUI-FIX-T10): Replaced std::variant<TextSegment,PasteSegment> with a
// plain tagged-union struct.  Saves ~16 bytes/segment × 50 queue segments =
// ~800 B resident; eliminates std::visit lambdas in hot paths.
//
// The InputBar buffer is represented as a std::vector<InputSegment> rather
// than a flat std::string.  Each segment is one of:
//
//   kind == 0 (Text)  — ordinary typed text, may contain '\n' (from Shift+Enter).
//       body   : std::string  — the text content.
//       Cursor can navigate byte-by-byte within a Text segment.
//
//   kind == 1 (Paste) — a bracketed-paste block.  Rendered as a one-line chip:
//       body       : std::string  — full original paste content.
//       paste_id   : int          — per-session paste counter, starting at 1.
//       line_count : int          — number of '\n' characters in body.
//       char_count : int          — UTF-8 character count of body.
//       ATOMIC: cursor cannot enter the segment; arrow keys skip it as one
//       unit; a single Backspace deletes the entire segment.
//
// SegCursor — cursor position within the segment vector.
//   seg_idx   — index into segments vector
//   byte_off  — byte offset within a Text segment body
//               (always 0 for Paste segments; the cursor sits before them)
//
// TUI-FIX-T4 / TUI-FIX-T10 (G1) blueprint contract.
// ---------------------------------------------------------------------------
#pragma once

#include <string>
#include <cstdint>

namespace batbox::tui {

// ---------------------------------------------------------------------------
// InputSegment — tagged union (replaces std::variant<TextSegment,PasteSegment>)
// ---------------------------------------------------------------------------
// kind == 0 → Text segment
// kind == 1 → Paste segment
// ---------------------------------------------------------------------------
struct InputSegment {
    uint8_t     kind{0};         ///< 0 = Text, 1 = Paste
    uint8_t     _pad[3]{};       ///< padding for alignment
    int         paste_id{0};     ///< valid when kind == 1
    int         line_count{0};   ///< valid when kind == 1
    int         char_count{0};   ///< valid when kind == 1
    std::string body;            ///< both kinds: text content or paste body

    /// Construct a Text segment.
    static InputSegment make_text(std::string text) {
        InputSegment s;
        s.kind = 0;
        s.body = std::move(text);
        return s;
    }

    /// Construct a Paste segment.
    static InputSegment make_paste(int id, std::string text, int lc, int cc) {
        InputSegment s;
        s.kind       = 1;
        s.paste_id   = id;
        s.line_count = lc;
        s.char_count = cc;
        s.body       = std::move(text);
        return s;
    }
};

/// Returns true when s is a Text segment.
inline bool is_text(const InputSegment& s)  { return s.kind == 0; }
/// Returns true when s is a Paste segment.
inline bool is_paste(const InputSegment& s) { return s.kind == 1; }

// ---------------------------------------------------------------------------
// Backward-compatible shims — kept so existing call sites in InputBar.cpp
// that construct TextSegment{} or PasteSegment{} still compile.
// These are thin wrappers; all state lives in InputSegment.
// ---------------------------------------------------------------------------

/// Legacy TextSegment — constructed from a string body, converts to InputSegment.
struct TextSegment {
    std::string body;
    explicit TextSegment(std::string b = {}) : body(std::move(b)) {}
    operator InputSegment() const { return InputSegment::make_text(body); }
};

/// Legacy PasteSegment — carries paste metadata; converts to InputSegment.
struct PasteSegment {
    int         id{0};
    std::string body;
    int         line_count{0};
    int         char_count{0};
    operator InputSegment() const {
        return InputSegment::make_paste(id, body, line_count, char_count);
    }
};

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
