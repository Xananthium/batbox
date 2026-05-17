// include/batbox/repl/VimMode.hpp
// ---------------------------------------------------------------------------
// batbox::repl::VimMode — Vim-mode state machine for the REPL input buffer.
//
// Operates entirely on a UTF-8 string buffer + integer cursor position. No
// FTXUI dependency so it can be unit-tested without a terminal.
//
// Usage:
//
//   VimMode vm;
//   vm.set_enabled(true);              // or let BATBOX_VIM_MODE=true do it
//
//   // In the input event handler:
//   auto action = vm.process_key(key_str, buf, cursor);
//   // apply action to InputBar state
//
// Actions are plain discriminated unions. Callers switch on the kind and
// apply the mutation to whatever buffer they own.
//
// Blueprint contract (Non-technical Deb lock):
//   class      batbox::repl::VimMode
//   method     VimMode::handle_key(Event, buffer) — mutates buffer; mode transitions
//   (process_key is the primary public entry point; handle_key is an alias in
//    the class that delegates to process_key for compatibility.)
// ---------------------------------------------------------------------------

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace batbox::repl {

// ---------------------------------------------------------------------------
// Mode — the three canonical vim states
// ---------------------------------------------------------------------------
enum class VimModeState {
    Insert,  ///< Characters typed go into the buffer (default / initial state)
    Normal,  ///< Motion & operator commands; no direct character insertion
    Visual,  ///< Character-wise visual selection (v key enters this)
};

// ---------------------------------------------------------------------------
// Action — what the caller must do after processing a key.
//
// The VimMode state machine mutates NOTHING on the caller's buffer directly;
// it only tells the caller what to do.  This keeps the state machine pure and
// easily testable.
// ---------------------------------------------------------------------------
enum class VimActionKind {
    NoOp,           ///< Key consumed; nothing to do (e.g., partial operator pending)
    InsertChar,     ///< Insert action.ch at action.cursor_pos
    DeleteRange,    ///< Delete buffer[start..end)
    ReplaceRange,   ///< Replace buffer[start..end) with action.text
    MoveCursor,     ///< Move cursor to action.cursor_pos
    SetBuffer,      ///< Replace entire buffer + cursor (e.g., paste, dd-yank restore)
    ChangeMode,     ///< Mode changed; action.new_mode reflects the new state
    Passthrough,    ///< Not in vim mode or key should be handled by caller as-is
    SendLine,       ///< User pressed Enter in Insert mode — submit the line
    ClearLine,      ///< dd equivalent — buffer cleared, yanked saved internally
};

struct VimAction {
    VimActionKind kind{VimActionKind::NoOp};

    // For InsertChar
    char ch{'\0'};

    // For DeleteRange / ReplaceRange
    std::size_t start{0};
    std::size_t end{0};    ///< exclusive upper bound

    // For ReplaceRange and SetBuffer
    std::string text;

    // For MoveCursor
    std::size_t cursor_pos{0};

    // For ChangeMode
    VimModeState new_mode{VimModeState::Insert};

    // ---- Convenience factories ----

    static VimAction noop()               { return {}; }
    static VimAction passthrough()        { VimAction a; a.kind = VimActionKind::Passthrough; return a; }
    static VimAction send_line()          { VimAction a; a.kind = VimActionKind::SendLine; return a; }

    static VimAction move_to(std::size_t pos) {
        VimAction a;
        a.kind       = VimActionKind::MoveCursor;
        a.cursor_pos = pos;
        return a;
    }

    static VimAction insert_char(char c, std::size_t pos) {
        VimAction a;
        a.kind       = VimActionKind::InsertChar;
        a.ch         = c;
        a.cursor_pos = pos;
        return a;
    }

    static VimAction delete_range(std::size_t s, std::size_t e) {
        VimAction a;
        a.kind  = VimActionKind::DeleteRange;
        a.start = s;
        a.end   = e;
        return a;
    }

    static VimAction replace_range(std::size_t s, std::size_t e, std::string txt) {
        VimAction a;
        a.kind  = VimActionKind::ReplaceRange;
        a.start = s;
        a.end   = e;
        a.text  = std::move(txt);
        return a;
    }

    static VimAction set_buffer(std::string buf, std::size_t pos) {
        VimAction a;
        a.kind       = VimActionKind::SetBuffer;
        a.text       = std::move(buf);
        a.cursor_pos = pos;
        return a;
    }

    static VimAction change_mode(VimModeState m) {
        VimAction a;
        a.kind     = VimActionKind::ChangeMode;
        a.new_mode = m;
        return a;
    }

    static VimAction clear_line() {
        VimAction a;
        a.kind = VimActionKind::ClearLine;
        return a;
    }
};

// ---------------------------------------------------------------------------
// VimMode — the state machine
//
// Thread-safety: NOT thread-safe. All calls must come from the same thread
// (the FTXUI event loop / input thread).
// ---------------------------------------------------------------------------
class VimMode {
public:
    // ---- Lifecycle ---------------------------------------------------------

    VimMode();
    ~VimMode() = default;

    VimMode(const VimMode&)            = delete;
    VimMode& operator=(const VimMode&) = delete;
    VimMode(VimMode&&)                 = default;
    VimMode& operator=(VimMode&&)      = default;

    // ---- Enable / disable --------------------------------------------------

    /// Enable or disable vim mode entirely.
    /// When disabled, process_key always returns Passthrough.
    void set_enabled(bool enabled);

    /// Returns true when vim mode is active.
    [[nodiscard]] bool is_enabled() const noexcept { return enabled_; }

    /// Toggle enabled state (called by /vim slash command).
    void toggle() { set_enabled(!enabled_); }

    // ---- State queries -----------------------------------------------------

    [[nodiscard]] VimModeState mode()        const noexcept { return mode_; }
    [[nodiscard]] std::string  yank_buffer() const          { return yank_buf_; }

    /// Human-readable mode indicator for the status line ("-- INSERT --", etc.)
    [[nodiscard]] std::string mode_indicator() const;

    // ---- Key processing ----------------------------------------------------

    /// Primary entry point.
    ///
    /// @param key     The key string from FTXUI (e.g., "h", "j", "\x1b", "dd", etc.)
    ///                Single printable chars arrive as single-char strings.
    ///                Special FTXUI keys arrive as multi-byte escape sequences.
    ///                Callers may also pass synthesised strings for testing.
    /// @param buf     Current input buffer content (read-only view).
    /// @param cursor  Current cursor position (byte offset into buf, 0-based).
    ///
    /// @returns A VimAction describing what the caller must do.  The caller is
    ///          responsible for applying the mutation to the actual buffer.
    ///
    VimAction process_key(std::string_view key, std::string_view buf, std::size_t cursor);

    /// Blueprint contract alias — delegates to process_key.
    /// Kept for naming consistency with the Deb-locked symbol
    /// "handle_key(Event, buffer) → mutates buffer".
    ///
    /// Callers that hold a buffer reference may use this overload; the VimMode
    /// still does not mutate buf directly — it returns the action and then
    /// the caller applies it.
    VimAction handle_key(std::string_view key, std::string& buf, std::size_t& cursor);

    // ---- Reset -------------------------------------------------------------

    /// Reset to Insert mode and clear any pending operator state.
    /// Called when a new prompt line is started.
    void reset();

private:
    // ---- Internal state ----------------------------------------------------

    bool          enabled_{false};
    VimModeState  mode_{VimModeState::Insert};

    // Pending operator character ('d', 'c', 'y', or '\0' when none)
    char pending_operator_{'\0'};

    // Pending count digit accumulation (e.g. "3w" → count=3, pending_motion='w')
    int  pending_count_{0};

    // Yank buffer (unnamed register)
    std::string yank_buf_;

    // Visual-selection anchor
    std::size_t visual_anchor_{0};

    // Text-object prefix: 'i' or 'a' after an operator, waiting for object type
    char obj_prefix_{'\0'};

    // ---- Internal helpers --------------------------------------------------

    // Normal-mode dispatch
    VimAction normal_key(std::string_view key, std::string_view buf, std::size_t cursor);

    // Insert-mode dispatch
    VimAction insert_key(std::string_view key, std::string_view buf, std::size_t cursor);

    // Visual-mode dispatch
    VimAction visual_key(std::string_view key, std::string_view buf, std::size_t cursor);

    // Motion resolution — returns the cursor position after the motion.
    // Returns the current cursor if the motion is unknown.
    std::size_t resolve_motion(char motion, std::string_view buf,
                               std::size_t cursor, int count = 1) const;

    // Word-motion helpers (operate on byte offsets; ASCII-correct for Latin text)
    static std::size_t word_forward_start(std::string_view buf, std::size_t pos, int n);
    static std::size_t word_backward_start(std::string_view buf, std::size_t pos, int n);
    static std::size_t word_forward_end(std::string_view buf, std::size_t pos, int n);

    // Text-object helpers: return {start, end} exclusive range
    static std::pair<std::size_t, std::size_t>
    inner_word(std::string_view buf, std::size_t cursor);

    static std::pair<std::size_t, std::size_t>
    a_word(std::string_view buf, std::size_t cursor);

    static std::pair<std::size_t, std::size_t>
    inner_quoted(std::string_view buf, std::size_t cursor, char quote);

    static std::pair<std::size_t, std::size_t>
    outer_quoted(std::string_view buf, std::size_t cursor, char quote);

    // Case toggle for 'x' / '~'
    static char toggle_case(char c);

    // Clamp cursor to valid range [0, buf.size()] (insert) or [0, max(0, buf.size()-1)] (normal)
    std::size_t clamp_cursor(std::size_t pos, std::string_view buf) const;
};

} // namespace batbox::repl
