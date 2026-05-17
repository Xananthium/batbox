// src/repl/VimMode.cpp
// ---------------------------------------------------------------------------
// batbox::repl::VimMode — Vim-mode state machine implementation.
//
// Supports:
//   Normal mode:  hjkl motion, w/b/e word motion, 0/$ line start/end,
//                 i/I/a/A enter Insert, o/O open-line enter Insert,
//                 x delete char, ~ toggle case,
//                 dd delete line (stores in yank), yy yank line, p paste,
//                 d{motion}/c{motion}/y{motion} operators,
//                 iw/aw/i"/a" text objects (after d/c/y),
//                 gg (start of buffer), G (end of buffer),
//                 v enter Visual, Esc → stay Normal, numeric count prefix
//   Insert mode:  Esc → Normal, all printable chars are Passthrough so the
//                 caller's InputBar handles them natively (history, etc.)
//   Visual mode:  hjkl/w/b/e/0/$ motions, d/x/y operators, Esc → Normal
// ---------------------------------------------------------------------------

#include <batbox/repl/VimMode.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace batbox::repl {

// ============================================================================
// Lifecycle
// ============================================================================

VimMode::VimMode() = default;

void VimMode::set_enabled(bool enabled) {
    enabled_ = enabled;
    if (enabled_) {
        mode_             = VimModeState::Insert;
        pending_operator_ = '\0';
        pending_count_    = 0;
        obj_prefix_       = '\0';
    }
}

void VimMode::reset() {
    mode_             = VimModeState::Insert;
    pending_operator_ = '\0';
    pending_count_    = 0;
    obj_prefix_       = '\0';
    visual_anchor_    = 0;
}

// ============================================================================
// Public API
// ============================================================================

std::string VimMode::mode_indicator() const {
    switch (mode_) {
        case VimModeState::Insert: return "-- INSERT --";
        case VimModeState::Normal: return "-- NORMAL --";
        case VimModeState::Visual: return "-- VISUAL --";
    }
    return "";
}

VimAction VimMode::process_key(std::string_view key,
                                std::string_view buf,
                                std::size_t      cursor) {
    if (!enabled_) return VimAction::passthrough();

    switch (mode_) {
        case VimModeState::Insert: return insert_key(key, buf, cursor);
        case VimModeState::Normal: return normal_key(key, buf, cursor);
        case VimModeState::Visual: return visual_key(key, buf, cursor);
    }
    return VimAction::passthrough();
}

VimAction VimMode::handle_key(std::string_view key,
                               std::string&     buf,
                               std::size_t&     cursor) {
    VimAction action = process_key(key, buf, cursor);

    switch (action.kind) {
        case VimActionKind::MoveCursor:
            cursor = action.cursor_pos;
            break;

        case VimActionKind::InsertChar:
            buf.insert(action.cursor_pos, 1, action.ch);
            cursor = action.cursor_pos + 1;
            break;

        case VimActionKind::DeleteRange:
            if (action.start <= action.end && action.end <= buf.size()) {
                buf.erase(action.start, action.end - action.start);
                cursor = clamp_cursor(action.start, buf);
            }
            break;

        case VimActionKind::ReplaceRange:
            if (action.start <= action.end && action.end <= buf.size()) {
                buf.replace(action.start, action.end - action.start, action.text);
                cursor = clamp_cursor(action.start + action.text.size(), buf);
            }
            break;

        case VimActionKind::SetBuffer:
            buf    = action.text;
            cursor = clamp_cursor(action.cursor_pos, buf);
            break;

        case VimActionKind::ClearLine:
            buf.clear();
            cursor = 0;
            break;

        case VimActionKind::ChangeMode:
            mode_ = action.new_mode;
            // If action carries a specific cursor position (from a/A/I/i commands)
            // use it; otherwise clamp current cursor for the new mode.
            if (action.cursor_pos != 0 || action.new_mode == VimModeState::Insert) {
                // For Insert mode, cursor_pos=0 is legitimate (e.g., 'O' command),
                // so we always apply it when the mode is Insert.
                cursor = (action.new_mode == VimModeState::Insert)
                            ? action.cursor_pos
                            : clamp_cursor(action.cursor_pos, buf);
            } else {
                cursor = clamp_cursor(cursor, buf);
            }
            break;

        default:
            break;
    }

    return action;
}

// ============================================================================
// Insert mode
// ============================================================================

VimAction VimMode::insert_key(std::string_view key,
                               std::string_view /*buf*/,
                               std::size_t      /*cursor*/) {
    if (key == "\x1b" || key == "Escape") {
        mode_             = VimModeState::Normal;
        pending_operator_ = '\0';
        pending_count_    = 0;
        obj_prefix_       = '\0';
        return VimAction::change_mode(VimModeState::Normal);
    }

    if (key == "\n" || key == "\r" || key == "Return") {
        return VimAction::send_line();
    }

    return VimAction::passthrough();
}

// ============================================================================
// Normal mode
// ============================================================================

VimAction VimMode::normal_key(std::string_view key,
                               std::string_view buf,
                               std::size_t      cursor) {
    // --- Esc: clear pending state ---
    if (key == "\x1b" || key == "Escape") {
        pending_operator_ = '\0';
        pending_count_    = 0;
        obj_prefix_       = '\0';
        return VimAction::noop();
    }

    // --- Numeric count accumulation ---
    // Only do this when no operator/object prefix is pending to avoid consuming
    // digits that are part of register names etc.
    if (pending_operator_ == '\0' && obj_prefix_ == '\0') {
        if (key.size() == 1) {
            char ch = key[0];
            if (std::isdigit(static_cast<unsigned char>(ch))) {
                if (ch != '0' || pending_count_ > 0) {
                    pending_count_ = pending_count_ * 10 + (ch - '0');
                    return VimAction::noop();
                }
                // ch=='0' and no count → fall through to motion '0'
            }
        }
    }

    int count = (pending_count_ > 0) ? pending_count_ : 1;
    pending_count_ = 0;  // consumed

    // --- Two-character sequences: gg, dd, yy ---
    if (key == "gg") {
        return VimAction::move_to(0);
    }
    if (key == "dd") {
        yank_buf_ = std::string(buf);
        return VimAction::clear_line();
    }
    if (key == "yy") {
        yank_buf_ = std::string(buf);
        return VimAction::noop();
    }

    if (key.size() != 1) {
        // Arrow keys and other multi-char keys
        if (key == "\x1b[C" || key == "ArrowRight") {
            std::size_t lim = buf.empty() ? 0 : buf.size() - 1;
            return VimAction::move_to(std::min(cursor + 1, lim));
        }
        if (key == "\x1b[D" || key == "ArrowLeft") {
            return VimAction::move_to(cursor > 0 ? cursor - 1 : 0);
        }
        return VimAction::noop();
    }

    char ch = key[0];

    // =========================================================================
    // STATE: Waiting for text-object type (d/c/y + i/a already consumed)
    // obj_prefix_ is 'i' or 'a', pending_operator_ is 'd'/'c'/'y'
    // =========================================================================
    if (obj_prefix_ != '\0') {
        char op     = pending_operator_;
        char prefix = obj_prefix_;
        pending_operator_ = '\0';
        obj_prefix_       = '\0';

        bool inner = (prefix == 'i');

        std::pair<std::size_t, std::size_t> range{cursor, cursor};

        if (ch == 'w') {
            range = inner ? inner_word(buf, cursor) : a_word(buf, cursor);
        } else if (ch == '"' || ch == '\'' || ch == '`') {
            range = inner ? inner_quoted(buf, cursor, ch)
                          : outer_quoted(buf, cursor, ch);
        } else {
            return VimAction::noop();
        }

        auto [lo, hi] = range;
        if (lo >= hi) return VimAction::noop();

        if (op == 'y') {
            yank_buf_ = std::string(buf.substr(lo, hi - lo));
            return VimAction::noop();
        }
        if (op == 'd') {
            yank_buf_ = std::string(buf.substr(lo, hi - lo));
            return VimAction::delete_range(lo, hi);
        }
        if (op == 'c') {
            yank_buf_ = std::string(buf.substr(lo, hi - lo));
            mode_     = VimModeState::Insert;
            return VimAction::replace_range(lo, hi, "");
        }
        return VimAction::noop();
    }

    // =========================================================================
    // STATE: Waiting for motion/text-object after operator (d/c/y)
    // =========================================================================
    if (pending_operator_ != '\0') {
        char op = pending_operator_;

        // i/a → text-object prefix: wait for object type
        if (ch == 'i' || ch == 'a') {
            obj_prefix_ = ch;
            // pending_operator_ stays as is
            return VimAction::noop();
        }

        // Operator doubled → line operation
        if (ch == op) {
            pending_operator_ = '\0';
            if (op == 'd') {
                yank_buf_ = std::string(buf);
                return VimAction::clear_line();
            }
            if (op == 'y') {
                yank_buf_ = std::string(buf);
                return VimAction::noop();
            }
            if (op == 'c') {
                yank_buf_ = std::string(buf);
                mode_     = VimModeState::Insert;
                return VimAction::set_buffer("", 0);
            }
        }

        // Direct motion after operator
        std::size_t new_cur = resolve_motion(ch, buf, cursor, count);
        pending_operator_   = '\0';

        if (new_cur == cursor) return VimAction::noop();

        std::size_t lo = std::min(cursor, new_cur);
        std::size_t hi = std::max(cursor, new_cur);

        // For line-end ($) and 'G', extend to buf.size() so we delete to the end
        if (ch == '$' || ch == 'G') hi = buf.size();
        // For line-start (0), lo is already 0, hi is cursor — delete from 0 to cursor
        // (cursor is to the right of new_cur=0)

        if (op == 'y') {
            yank_buf_ = std::string(buf.substr(lo, hi - lo));
            return VimAction::noop();
        }
        if (op == 'd') {
            yank_buf_ = std::string(buf.substr(lo, hi - lo));
            return VimAction::delete_range(lo, hi);
        }
        if (op == 'c') {
            yank_buf_ = std::string(buf.substr(lo, hi - lo));
            mode_     = VimModeState::Insert;
            return VimAction::replace_range(lo, hi, "");
        }
        return VimAction::noop();
    }

    // =========================================================================
    // STATE: No pending operator — mode transitions, motions, and commands
    // =========================================================================

    // Mode transitions
    switch (ch) {
        case 'i': {
            mode_ = VimModeState::Insert;
            auto action       = VimAction::change_mode(VimModeState::Insert);
            action.cursor_pos = cursor;
            return action;
        }
        case 'I': {
            mode_ = VimModeState::Insert;
            std::size_t pos = 0;
            while (pos < buf.size() && buf[pos] == ' ') ++pos;
            auto action       = VimAction::change_mode(VimModeState::Insert);
            action.cursor_pos = pos;
            return action;
        }
        case 'a': {
            mode_ = VimModeState::Insert;
            std::size_t new_pos = buf.empty() ? 0 : std::min(cursor + 1, buf.size());
            auto action       = VimAction::change_mode(VimModeState::Insert);
            action.cursor_pos = new_pos;
            return action;
        }
        case 'A': {
            mode_ = VimModeState::Insert;
            auto action       = VimAction::change_mode(VimModeState::Insert);
            action.cursor_pos = buf.size();
            return action;
        }
        case 'o': {
            mode_ = VimModeState::Insert;
            auto action       = VimAction::change_mode(VimModeState::Insert);
            action.cursor_pos = buf.size();
            return action;
        }
        case 'O': {
            mode_ = VimModeState::Insert;
            auto action       = VimAction::change_mode(VimModeState::Insert);
            action.cursor_pos = 0;
            return action;
        }
        case 'v': {
            mode_          = VimModeState::Visual;
            visual_anchor_ = cursor;
            auto action       = VimAction::change_mode(VimModeState::Visual);
            action.cursor_pos = cursor;
            return action;
        }
        default:
            break;
    }

    // Operator starters
    if (ch == 'd' || ch == 'c' || ch == 'y') {
        pending_operator_ = ch;
        return VimAction::noop();
    }

    // Pure motions and direct commands
    switch (ch) {
        case 'h': {
            std::size_t new_pos = (cursor >= (std::size_t)count)
                                    ? cursor - (std::size_t)count
                                    : 0;
            return VimAction::move_to(clamp_cursor(new_pos, buf));
        }
        case 'l': {
            std::size_t lim = buf.empty() ? 0 : buf.size() - 1;
            return VimAction::move_to(std::min(cursor + (std::size_t)count, lim));
        }
        case '0':
            return VimAction::move_to(0);
        case '$':
            return VimAction::move_to(buf.empty() ? 0 : buf.size() - 1);
        case 'w':
            return VimAction::move_to(word_forward_start(buf, cursor, count));
        case 'b':
            return VimAction::move_to(word_backward_start(buf, cursor, count));
        case 'e':
            return VimAction::move_to(word_forward_end(buf, cursor, count));
        case 'G':
            return VimAction::move_to(buf.empty() ? 0 : buf.size() - 1);
        case 'x': {
            if (buf.empty()) return VimAction::noop();
            std::size_t n = std::min((std::size_t)count, buf.size() - cursor);
            yank_buf_ = std::string(buf.substr(cursor, n));
            return VimAction::delete_range(cursor, cursor + n);
        }
        case 'p': {
            if (yank_buf_.empty()) return VimAction::noop();
            std::size_t ins_pos = buf.empty() ? 0 : std::min(cursor + 1, buf.size());
            return VimAction::replace_range(ins_pos, ins_pos, yank_buf_);
        }
        case 'P': {
            if (yank_buf_.empty()) return VimAction::noop();
            return VimAction::replace_range(cursor, cursor, yank_buf_);
        }
        case '~': {
            if (cursor >= buf.size()) return VimAction::noop();
            std::string toggled(1, toggle_case(buf[cursor]));
            std::size_t next = clamp_cursor(cursor + 1, buf);
            VimAction act    = VimAction::replace_range(cursor, cursor + 1, std::move(toggled));
            act.cursor_pos   = next;
            return act;
        }
        case 'j':
        case 'k':
            return VimAction::noop();  // no multiline in single-line REPL
        default:
            break;
    }

    return VimAction::noop();
}

// ============================================================================
// Visual mode
// ============================================================================

VimAction VimMode::visual_key(std::string_view key,
                               std::string_view buf,
                               std::size_t      cursor) {
    if (key == "\x1b" || key == "Escape") {
        mode_ = VimModeState::Normal;
        return VimAction::change_mode(VimModeState::Normal);
    }

    if (key.size() != 1) return VimAction::noop();
    char ch = key[0];

    auto move_in_visual = [&](std::size_t new_pos) -> VimAction {
        return VimAction::move_to(new_pos);
    };

    switch (ch) {
        case 'h': return move_in_visual(cursor > 0 ? cursor - 1 : 0);
        case 'l': {
            std::size_t lim = buf.empty() ? 0 : buf.size() - 1;
            return move_in_visual(std::min(cursor + 1, lim));
        }
        case '0': return move_in_visual(0);
        case '$': return move_in_visual(buf.empty() ? 0 : buf.size() - 1);
        case 'w': return move_in_visual(word_forward_start(buf, cursor, 1));
        case 'b': return move_in_visual(word_backward_start(buf, cursor, 1));
        case 'e': return move_in_visual(word_forward_end(buf, cursor, 1));
        case 'd':
        case 'x': {
            std::size_t lo = std::min(cursor, visual_anchor_);
            std::size_t hi = std::max(cursor, visual_anchor_) + 1;
            hi             = std::min(hi, buf.size());
            yank_buf_      = std::string(buf.substr(lo, hi - lo));
            mode_          = VimModeState::Normal;
            return VimAction::delete_range(lo, hi);
        }
        case 'y': {
            std::size_t lo = std::min(cursor, visual_anchor_);
            std::size_t hi = std::max(cursor, visual_anchor_) + 1;
            hi             = std::min(hi, buf.size());
            yank_buf_      = std::string(buf.substr(lo, hi - lo));
            mode_          = VimModeState::Normal;
            return VimAction::move_to(lo);
        }
        default:
            break;
    }
    return VimAction::noop();
}

// ============================================================================
// Motion resolution
// ============================================================================

std::size_t VimMode::resolve_motion(char motion, std::string_view buf,
                                     std::size_t cursor, int count) const {
    switch (motion) {
        case 'h': return cursor >= (std::size_t)count ? cursor - (std::size_t)count : 0;
        case 'l': {
            std::size_t lim = buf.empty() ? 0 : buf.size() - 1;
            return std::min(cursor + (std::size_t)count, lim);
        }
        case '0': return 0;
        case '$': return buf.empty() ? 0 : buf.size() - 1;
        case 'w': return word_forward_start(buf, cursor, count);
        case 'b': return word_backward_start(buf, cursor, count);
        case 'e': return word_forward_end(buf, cursor, count);
        case 'G': return buf.empty() ? 0 : buf.size() - 1;
        default:  return cursor;
    }
}

// ============================================================================
// Word motion helpers
// ============================================================================

namespace {
    bool is_word_char(char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    }
}

/*static*/ std::size_t VimMode::word_forward_start(std::string_view buf,
                                                     std::size_t pos,
                                                     int n) {
    std::size_t p = pos;
    for (int i = 0; i < n; ++i) {
        if (p >= buf.size()) break;
        while (p < buf.size() && is_word_char(buf[p])) ++p;
        while (p < buf.size() && !is_word_char(buf[p])) ++p;
    }
    return p;
}

/*static*/ std::size_t VimMode::word_backward_start(std::string_view buf,
                                                      std::size_t pos,
                                                      int n) {
    if (buf.empty() || pos == 0) return 0;
    std::size_t p = pos;
    for (int i = 0; i < n; ++i) {
        if (p == 0) break;
        --p;
        while (p > 0 && !is_word_char(buf[p])) --p;
        while (p > 0 && is_word_char(buf[p - 1])) --p;
    }
    return p;
}

/*static*/ std::size_t VimMode::word_forward_end(std::string_view buf,
                                                   std::size_t pos,
                                                   int n) {
    std::size_t p = pos;
    for (int i = 0; i < n; ++i) {
        if (p >= buf.size()) break;
        if (p + 1 < buf.size()) ++p;
        while (p < buf.size() && !is_word_char(buf[p])) ++p;
        while (p + 1 < buf.size() && is_word_char(buf[p + 1])) ++p;
    }
    return p;
}

// ============================================================================
// Text-object helpers
// ============================================================================

/*static*/ std::pair<std::size_t, std::size_t>
VimMode::inner_word(std::string_view buf, std::size_t cursor) {
    if (buf.empty()) return {0, 0};
    std::size_t lo = cursor;
    std::size_t hi = cursor;
    while (lo > 0 && is_word_char(buf[lo - 1])) --lo;
    while (hi < buf.size() && is_word_char(buf[hi])) ++hi;
    return {lo, hi};
}

/*static*/ std::pair<std::size_t, std::size_t>
VimMode::a_word(std::string_view buf, std::size_t cursor) {
    auto [lo, hi] = inner_word(buf, cursor);
    // Extend hi to include trailing whitespace
    std::size_t saved_hi = hi;
    while (hi < buf.size() && buf[hi] == ' ') ++hi;
    // If no trailing whitespace, extend lo to include leading whitespace
    if (hi == saved_hi) {
        while (lo > 0 && buf[lo - 1] == ' ') --lo;
    }
    return {lo, hi};
}

/*static*/ std::pair<std::size_t, std::size_t>
VimMode::inner_quoted(std::string_view buf, std::size_t cursor, char quote) {
    if (buf.empty()) return {0, 0};

    std::size_t scan_from = (cursor < buf.size()) ? cursor : buf.size() - 1;

    // Find opening quote to the left of (or at) cursor
    std::size_t open = std::string_view::npos;
    for (std::size_t i = scan_from; ; --i) {
        if (buf[i] == quote) { open = i; break; }
        if (i == 0) break;
    }
    if (open == std::string_view::npos) return {0, 0};

    // Find closing quote to the right
    std::size_t close = std::string_view::npos;
    for (std::size_t i = open + 1; i < buf.size(); ++i) {
        if (buf[i] == quote) { close = i; break; }
    }
    if (close == std::string_view::npos) return {0, 0};

    return {open + 1, close};
}

/*static*/ std::pair<std::size_t, std::size_t>
VimMode::outer_quoted(std::string_view buf, std::size_t cursor, char quote) {
    auto [lo, hi] = inner_quoted(buf, cursor, quote);
    if (lo == 0 && hi == 0) return {0, 0};
    // Include the surrounding quote chars
    return {lo > 0 ? lo - 1 : 0, std::min(hi + 1, buf.size())};
}

// ============================================================================
// Case toggle
// ============================================================================

/*static*/ char VimMode::toggle_case(char c) {
    if (std::islower(static_cast<unsigned char>(c)))
        return (char)std::toupper(static_cast<unsigned char>(c));
    if (std::isupper(static_cast<unsigned char>(c)))
        return (char)std::tolower(static_cast<unsigned char>(c));
    return c;
}

// ============================================================================
// Cursor clamping
// ============================================================================

std::size_t VimMode::clamp_cursor(std::size_t pos, std::string_view buf) const {
    if (buf.empty()) return 0;
    if (mode_ == VimModeState::Insert) {
        return std::min(pos, buf.size());
    }
    return std::min(pos, buf.size() - 1);
}

} // namespace batbox::repl
