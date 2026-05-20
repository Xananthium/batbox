// src/tui/InputBar.cpp
// ---------------------------------------------------------------------------
// batbox::tui::InputBar implementation — TUI-FIX-T10 input-queue + G1/G10.
//
// See include/batbox/tui/InputBar.hpp for design notes and API contract.
// See include/batbox/tui/InputSegment.hpp for the tagged-union segment type.
//
// Changes in TUI-FIX-T10:
//   - G1: InputSegment is now a tagged-union struct (kind 0=Text, 1=Paste).
//         All std::get_if<> call sites replaced with .kind checks.
//   - G10: format_tokens() uses snprintf into a stack buffer (single alloc).
//   - A2: InputQueue SoA; Tab/Enter/Esc edge-case dispatch (10 cases).
//   - Queue rendering inside OnRender (NOT a new FTXUI Component).
// ---------------------------------------------------------------------------

#include <batbox/tui/InputBar.hpp>
#include <batbox/perf/PerfSnapshot.hpp>
#include <batbox/tui/Events.hpp>
#include <batbox/core/Logging.hpp>
#include <batbox/permissions/PermissionGate.hpp>
#include <batbox/permissions/PermissionMode.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <numeric>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

using namespace ftxui;

namespace batbox::tui {

// =============================================================================
// FTXUI event input-string constants (matching Keybindings.cpp)
// =============================================================================
namespace {

constexpr std::string_view kInputBackspace   = "\x7f";   // DEL
constexpr std::string_view kInputBackspace2  = "\x08";   // Ctrl+H (some terminals)
constexpr std::string_view kInputDelete      = "\x1b[3~";
constexpr std::string_view kInputArrowLeft   = "\x1b[D";
constexpr std::string_view kInputArrowRight  = "\x1b[C";
constexpr std::string_view kInputArrowUp     = "\x1b[A";
constexpr std::string_view kInputArrowDown   = "\x1b[B";
constexpr std::string_view kInputHome        = "\x1b[H";
constexpr std::string_view kInputEnd         = "\x1b[F";
constexpr std::string_view kInputTab         = "\x09";
constexpr std::string_view kInputEscape      = "\x1b";
constexpr std::string_view kInputReturn      = "\x0a";
constexpr std::string_view kInputCtrlM       = "\x0d";

// TUI-FIX-T4: bracketed paste sequences
constexpr std::string_view kBracketedPasteStart = "\x1b[200~";
constexpr std::string_view kBracketedPasteEnd   = "\x1b[201~";

// TUI-FIX-T4: additional Shift+Enter encodings (beyond kitty \x1b[13;2u
// which is already handled by Keybindings via ReplAction::Newline).
// "\x1b\r" — Alt+Enter / Shift+Enter in many terminals (xterm, Terminal.app)
// "\x1b[27;2;13~" — libvte / some CSI-u implementations
constexpr std::string_view kShiftEnterAlt   = "\x1b\r";
constexpr std::string_view kShiftEnterCSIU  = "\x1b[27;2;13~";

/// Default contextual placeholder shown when the splash banner is visible.
constexpr std::string_view kSplashPlaceholder = "Try '/help' or 'plan a feature'";

/// Number of render frames between placeholder template advances (TUI-FLOW-T9).
constexpr int kPlaceholderFrameThrottle = 120;

/// G10 — Format a token count with comma separators using snprintf (single alloc).
std::string format_tokens(uint32_t n) {
    if (n < 1000) {
        char buf[16];
        int len = std::snprintf(buf, sizeof(buf), "%utk", n);
        return std::string(buf, len > 0 ? static_cast<std::size_t>(len) : 0u);
    }
    // Insert commas manually into a stack buffer.
    // Max uint32 is 4,294,967,295 → 10 digits + 3 commas + "tk" + NUL = 17 chars.
    char digits[11];
    int  dlen = std::snprintf(digits, sizeof(digits), "%u", n);
    if (dlen <= 0) return "0tk";

    char buf[24];
    int  bi      = 0;
    int  di      = 0;
    int  to_comma = dlen % 3;
    if (to_comma == 0) to_comma = 3;

    while (di < dlen) {
        if (di > 0 && (di - (dlen % 3 == 0 ? 3 : dlen % 3)) % 3 == 0) {
            buf[bi++] = ',';
        }
        buf[bi++] = digits[di++];
    }
    buf[bi++] = 't';
    buf[bi++] = 'k';
    return std::string(buf, static_cast<std::size_t>(bi));
}

/// Format a cost in USD as "$X.XXX" using snprintf (single alloc, no ostringstream).
std::string format_cost(double usd) {
    char buf[32];
    int n = std::snprintf(buf, sizeof(buf), "$%.3f", usd);
    return std::string(buf, n > 0 ? static_cast<std::size_t>(n) : 0u);
}

/// Map a PermissionMode to the short display label shown in the footer chip.
std::string mode_chip_label(batbox::permissions::PermissionMode mode) {
    using M = batbox::permissions::PermissionMode;
    switch (mode) {
        case M::Default:     return "default";
        case M::Plan:        return "plan";
        case M::AcceptEdits: return "accept edits";
        case M::Nuclear:     return "NUCLEAR";
    }
    return "default";
}

/// Returns true if the event input string is a single printable character
/// (non-control, non-escape-sequence).
bool is_printable_char(const ftxui::Event& ev) {
    const auto& inp = ev.input();
    if (inp.empty() || inp.size() > 4) return false;
    if (inp.size() == 1) {
        unsigned char c = static_cast<unsigned char>(inp[0]);
        return c >= 0x20 && c != 0x7f;
    }
    unsigned char first = static_cast<unsigned char>(inp[0]);
    return first >= 0xC0;
}

/// Case-insensitive substring check.
bool ci_contains(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    if (haystack.size() < needle.size()) return false;
    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
        });
    return it != haystack.end();
}

/// Tagged-union segment helpers — return pointer or nullptr, matching
/// the std::get_if<> interface pattern from the old variant API.
inline const InputSegment* as_text_seg(const InputSegment& s) {
    return is_text(s) ? &s : nullptr;
}
inline InputSegment* as_text_seg(InputSegment& s) {
    return is_text(s) ? &s : nullptr;
}
inline const InputSegment* as_paste_seg(const InputSegment& s) {
    return is_paste(s) ? &s : nullptr;
}
inline InputSegment* as_paste_seg(InputSegment& s) {
    return is_paste(s) ? &s : nullptr;
}

/// Returns true when the event is a ToolRunning payload event.
bool has_prefix_tool_running(const ftxui::Event& ev) {
    static constexpr const char* kPrefix = "batbox.tool-running";
    const std::string& s = ev.input();
    const std::size_t plen = std::strlen(kPrefix);
    return s.size() > plen && s.compare(0, plen, kPrefix) == 0 && s[plen] == ':';
}

/// Returns true when the event is a ToolDone payload event.
bool has_prefix_tool_done(const ftxui::Event& ev) {
    static constexpr const char* kPrefix = "batbox.tool-done";
    const std::string& s = ev.input();
    const std::size_t plen = std::strlen(kPrefix);
    return s.size() > plen && s.compare(0, plen, kPrefix) == 0 && s[plen] == ':';
}

/// Returns true when the event is a ThinkingStarted payload event.
bool has_prefix_thinking_started(const ftxui::Event& ev) {
    static constexpr const char* kPrefix = "batbox.thinking-started";
    const std::string& s = ev.input();
    const std::size_t plen = std::strlen(kPrefix);
    return s.size() > plen && s.compare(0, plen, kPrefix) == 0 && s[plen] == ':';
}

/// Returns true when the event is a ThinkingStopped payload event.
bool has_prefix_thinking_stopped(const ftxui::Event& ev) {
    static constexpr const char* kPrefix = "batbox.thinking-stopped";
    const std::string& s = ev.input();
    const std::size_t plen = std::strlen(kPrefix);
    return s.size() > plen && s.compare(0, plen, kPrefix) == 0 && s[plen] == ':';
}

/// Returns true when the event is a StatusUpdate payload event (A3 / TUI-FIX-T6).
bool has_prefix_status_update(const ftxui::Event& ev) {
    static constexpr const char* kPrefix = "batbox.status-update";
    const std::string& s = ev.input();
    const std::size_t plen = std::strlen(kPrefix);
    return s.size() > plen && s.compare(0, plen, kPrefix) == 0 && s[plen] == ':';
}

/// Count UTF-8 characters in a string (approximate: counts leading bytes only).
int count_utf8_chars(const std::string& s) {
    int count = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) ++count;
    }
    return count;
}

/// Count newlines in a string.
int count_newlines(const std::string& s) {
    return static_cast<int>(std::count(s.begin(), s.end(), '\n'));
}

} // anonymous namespace

// =============================================================================
// Construction
// =============================================================================

InputBar::InputBar(batbox::theme::ThemeRef       theme,
                   batbox::repl::History&        history,
                   batbox::repl::Keybindings&    keybindings,
                   SubmitCallback                on_submit,
                   SlashCommandProvider          slash_provider,
                   AutocompleteProvider          ac_provider)
    : theme_(theme)
    , history_(history)
    , keybindings_(keybindings)
    , on_submit_(std::move(on_submit))
    , slash_provider_(std::move(slash_provider))
    , ac_provider_(std::move(ac_provider))
{
    // Initialise with a single empty Text segment so the buffer is never empty
    // of segments (cursor operations always have a valid target).
    segments_.push_back(InputSegment::make_text(""));
    seg_cursor_ = SegCursor{0, 0};

    // Respect BATBOX_VIM_MODE env var at construction
    const char* vim_env = std::getenv("BATBOX_VIM_MODE");
    if (vim_env && std::string_view(vim_env) == "true") {
        vim_mode_.set_enabled(true);
    }

    // TUI-FLOW-T3: check BATBOX_PERF_HUD once at construction
    {
        const char* hud_env = std::getenv("BATBOX_PERF_HUD");
        perf_hud_enabled_ = (hud_env != nullptr && hud_env[0] != '\0'
                             && std::string_view(hud_env) != "0"
                             && std::string_view(hud_env) != "false");
    }
}

// =============================================================================
// Public API — status line
// =============================================================================

void InputBar::set_status(StatusLine status) {
    status_ = std::move(status);
}

void InputBar::set_model(std::string name) {
    status_.model_name = std::move(name);
}

void InputBar::set_usage(uint32_t tokens, double cost_usd) {
    status_.token_count = tokens;
    status_.cost_usd    = cost_usd;
}

void InputBar::set_mode(std::string label) {
    status_.mode_label = std::move(label);
}

void InputBar::set_running_tool(std::optional<std::string> tool) {
    running_tool_ = std::move(tool);
}

// =============================================================================
// Public API — splash placeholder (TUI-FLOW-T4)
// =============================================================================

void InputBar::set_splash_showing(bool showing) {
    splash_showing_ = showing;
}

// =============================================================================
// Public API — contextual placeholder templates (TUI-FLOW-T9)
// =============================================================================

void InputBar::set_placeholder_templates(std::vector<std::string> templates) {
    placeholder_templates_ = std::move(templates);
    placeholder_frame_counter_ = 0;
}

// =============================================================================
// Public API — footer hint chips (TUI-FLOW-T6)
// =============================================================================

void InputBar::set_stream_active(bool active) {
    stream_active_ = active;
}

void InputBar::set_on_interrupt(InterruptCallback cb) {
    on_interrupt_ = std::move(cb);
}

void InputBar::set_effort_level(std::string level) {
    effort_level_ = std::move(level);
}

void InputBar::set_mcp_failed(int n) {
    mcp_failed_.store(n, std::memory_order_relaxed);
}

// =============================================================================
// Public API — permission gate wiring (TUI-PERM-T1)
// =============================================================================

void InputBar::set_permission_gate(batbox::permissions::PermissionGate* gate) {
    perm_gate_ = gate;
    if (perm_gate_) {
        status_.mode_label = mode_chip_label(perm_gate_->current_mode());
    }
}

// =============================================================================
// Public API — buffer (backward-compatible flat API)
// =============================================================================

std::string InputBar::buffer() const {
    return flatten_for_submit();
}

std::size_t InputBar::cursor() const noexcept {
    return flat_cursor_offset();
}

// =============================================================================
// Public API — vim mode
// =============================================================================

void InputBar::set_vim_enabled(bool enabled) {
    vim_mode_.set_enabled(enabled);
}

void InputBar::toggle_vim() {
    vim_mode_.toggle();
}

// =============================================================================
// Public API — programmatic control
// =============================================================================

void InputBar::clear() {
    segments_.clear();
    segments_.push_back(InputSegment::make_text(""));
    seg_cursor_ = SegCursor{0, 0};
    paste_id_counter_ = 0;  // reset paste counter on clear
    in_paste_seq_ = false;
    paste_accumulator_.clear();
    history_.reset_cursor();
    palette_close();
    autocomplete_reset();
    vim_mode_.reset();
    cursor_idx_ = -1;
}

void InputBar::set_buffer(std::string text) {
    segments_.clear();
    segments_.push_back(InputSegment::make_text(text));
    seg_cursor_ = SegCursor{0, text.size()};
    in_paste_seq_ = false;
    paste_accumulator_.clear();
    autocomplete_reset();
    cursor_idx_ = -1;
}

// =============================================================================
// Public API — queue control
// =============================================================================

void InputBar::clear_queue() {
    queue_.clear();
    cursor_idx_ = -1;
}

// =============================================================================
// TUI-FIX-T4: segment model — flatten and cursor helpers
// G1: all std::get_if replaced with .kind checks
// =============================================================================

std::string InputBar::flatten_for_submit() const {
    std::string result;
    for (const auto& seg : segments_) {
        result += seg.body;  // both Text and Paste use body
    }
    return result;
}

std::size_t InputBar::flat_cursor_offset() const noexcept {
    std::size_t offset = 0;
    for (std::size_t i = 0; i < seg_cursor_.seg_idx && i < segments_.size(); ++i) {
        offset += segments_[i].body.size();
    }
    // Add byte offset within the current segment (only applies to Text segments)
    if (seg_cursor_.seg_idx < segments_.size()) {
        if (is_text(segments_[seg_cursor_.seg_idx])) {
            offset += seg_cursor_.byte_off;
        }
        // For Paste segments, byte_off is always 0; the cursor sits before them.
    }
    return offset;
}

void InputBar::set_seg_cursor_from_flat(std::size_t flat_offset) {
    std::size_t remaining = flat_offset;
    for (std::size_t i = 0; i < segments_.size(); ++i) {
        const auto& seg = segments_[i];
        std::size_t seg_len = seg.body.size();
        if (is_text(seg)) {
            if (remaining <= seg_len) {
                seg_cursor_ = SegCursor{i, remaining};
                return;
            }
        } else {
            // Paste segment
            if (remaining < seg_len) {
                // Cursor cannot enter paste; place it before this segment.
                seg_cursor_ = SegCursor{i, 0};
                return;
            }
            // If remaining == seg_len, the cursor is right after the paste.
        }
        remaining -= seg_len;
    }
    // Cursor at end of buffer
    if (!segments_.empty()) {
        std::size_t last = segments_.size() - 1;
        if (is_text(segments_[last])) {
            seg_cursor_ = SegCursor{last, std::min(remaining, segments_[last].body.size())};
        } else {
            seg_cursor_ = SegCursor{segments_.size(), 0};
        }
    } else {
        seg_cursor_ = SegCursor{0, 0};
    }
}

bool InputBar::is_empty() const noexcept {
    for (const auto& seg : segments_) {
        if (!seg.body.empty()) return false;
    }
    return true;
}

void InputBar::normalize_segments() {
    std::vector<InputSegment> result;
    for (auto& seg : segments_) {
        if (is_text(seg)) {
            if (seg.body.empty()) continue;
            // Try to merge with last segment if it's also Text
            if (!result.empty() && is_text(result.back())) {
                result.back().body += seg.body;
            } else {
                result.push_back(seg);
            }
        } else {
            result.push_back(seg);
        }
    }

    // Always keep at least one (empty) Text segment
    if (result.empty()) {
        result.push_back(InputSegment::make_text(""));
    } else if (is_paste(result.back())) {
        // Ensure we end with a Text segment so the cursor can sit at end
        result.push_back(InputSegment::make_text(""));
    }

    std::size_t flat = flat_cursor_offset();
    segments_ = std::move(result);
    set_seg_cursor_from_flat(flat);
}

// static
std::string InputBar::paste_chip_label(const InputSegment& ps) {
    if (ps.line_count > 0) {
        return "[Pasted text #" + std::to_string(ps.paste_id)
             + " +" + std::to_string(ps.line_count) + " lines]";
    } else {
        return "[Pasted text #" + std::to_string(ps.paste_id)
             + " (" + std::to_string(ps.char_count) + " chars)]";
    }
}

// =============================================================================
// TUI-FIX-T4: insert_paste
// G1: constructs InputSegment directly via make_paste
// =============================================================================

void InputBar::insert_paste(std::string body) {
    const int newlines  = count_newlines(body);
    const int char_cnt  = count_utf8_chars(body);

    // Sub-threshold: insert as plain text (no chip)
    if (newlines == 0 && char_cnt < kPasteChipMinChars) {
        insert_at_cursor(body);
        return;
    }

    ++paste_id_counter_;
    auto ps = InputSegment::make_paste(paste_id_counter_, std::move(body), newlines, char_cnt);

    // Insert at current cursor:
    // Split the current Text segment at the cursor position, insert paste in middle.
    std::size_t si = seg_cursor_.seg_idx;
    std::size_t bo = seg_cursor_.byte_off;

    if (si < segments_.size() && is_text(segments_[si])) {
        std::string before = segments_[si].body.substr(0, bo);
        std::string after  = segments_[si].body.substr(bo);

        std::vector<InputSegment> replacement;
        if (!before.empty()) replacement.push_back(InputSegment::make_text(before));
        replacement.push_back(std::move(ps));
        replacement.push_back(InputSegment::make_text(after));

        segments_.erase(segments_.begin() + static_cast<std::ptrdiff_t>(si));
        segments_.insert(segments_.begin() + static_cast<std::ptrdiff_t>(si),
                         std::make_move_iterator(replacement.begin()),
                         std::make_move_iterator(replacement.end()));

        std::size_t paste_idx = si + (before.empty() ? 0 : 1);
        seg_cursor_ = SegCursor{paste_idx + 1, 0};
    } else {
        if (si >= segments_.size()) {
            segments_.push_back(std::move(ps));
            segments_.push_back(InputSegment::make_text(""));
            seg_cursor_ = SegCursor{segments_.size() - 1, 0};
        } else {
            segments_.insert(segments_.begin() + static_cast<std::ptrdiff_t>(si),
                             std::move(ps));
            seg_cursor_ = SegCursor{si + 1, 0};
        }
    }

    normalize_segments();
}

// =============================================================================
// Internal: cursor_text_segment
// G1: returns InputSegment& instead of TextSegment&
// =============================================================================

InputSegment& InputBar::cursor_text_segment() {
    if (seg_cursor_.seg_idx < segments_.size()) {
        if (is_text(segments_[seg_cursor_.seg_idx])) {
            return segments_[seg_cursor_.seg_idx];
        }
        // Cursor is AT a Paste segment (byte_off == 0).
        // Insert a new Text segment before the paste segment.
        segments_.insert(
            segments_.begin() + static_cast<std::ptrdiff_t>(seg_cursor_.seg_idx),
            InputSegment::make_text(""));
        return segments_[seg_cursor_.seg_idx];
    }
    // Cursor is past the end of segments_.
    segments_.push_back(InputSegment::make_text(""));
    seg_cursor_.seg_idx = segments_.size() - 1;
    seg_cursor_.byte_off = 0;
    return segments_.back();
}

// =============================================================================
// Internal: insert_at_cursor
// =============================================================================

void InputBar::insert_at_cursor(std::string_view text) {
    auto& ts = cursor_text_segment();
    ts.body.insert(seg_cursor_.byte_off, text);
    seg_cursor_.byte_off += text.size();
}

// =============================================================================
// TUI-FIX-T10: queue helpers
// =============================================================================

bool InputBar::queue_append_current() {
    // Snapshot the current segment buffer (skip if logically empty)
    if (is_empty()) return false;
    // Limit queue depth
    if (queue_.depth() >= InputQueue::kMaxQueueDepth) return false;
    // Normalise first so we don't store dangling empty segments
    normalize_segments();
    queue_.append(segments_);
    // Clear input row
    segments_.clear();
    segments_.push_back(InputSegment::make_text(""));
    seg_cursor_ = SegCursor{0, 0};
    autocomplete_reset();
    cursor_idx_ = -1;
    return true;
}

std::string InputBar::build_combined_submit() {
    std::string current = flatten_for_submit();
    std::string queued  = queue_.flatten_all();
    std::string combined;
    if (!current.empty() && !queued.empty()) {
        combined = current + "\n" + queued;
    } else if (!current.empty()) {
        combined = current;
    } else {
        combined = queued;
    }
    // Clear both
    segments_.clear();
    segments_.push_back(InputSegment::make_text(""));
    seg_cursor_ = SegCursor{0, 0};
    queue_.clear();
    cursor_idx_ = -1;
    return combined;
}

// =============================================================================
// Render
// =============================================================================

ftxui::Element InputBar::OnRender() {
    Elements rows;

    if (palette_open_) {
        rows.push_back(render_palette_overlay());
    }

    // TUI-FIX-T10: render queue rows above the active input row.
    // Oldest entry at top, newest just above the input row.
    for (std::size_t i = 0; i < queue_.depth(); ++i) {
        bool sel = (cursor_idx_ == static_cast<int>(i));
        rows.push_back(render_queue_row(i, sel));
    }

    rows.push_back(render_prompt_row());
    rows.push_back(render_status_row());
    rows.push_back(render_footer_chips_row());

    return vbox(std::move(rows));
}

ftxui::Element InputBar::render_queue_row(std::size_t entry_idx, bool selected) const {
    auto muted_color   = color_for(theme_, ThemeRole::Muted);
    auto accent_color  = color_for(theme_, ThemeRole::AccentCyan);
    auto fg_color      = color_for(theme_, ThemeRole::Fg);

    // Build the flattened entry text (truncate for display if very long)
    std::string entry_text = queue_.flatten_entry(entry_idx);
    // Collapse newlines to spaces for single-line display
    for (char& c : entry_text) {
        if (c == '\n') c = ' ';
    }
    // Truncate to 60 chars to keep it readable
    if (entry_text.size() > 60) {
        entry_text = entry_text.substr(0, 57) + "...";
    }

    char prefix_buf[12];
    std::snprintf(prefix_buf, sizeof(prefix_buf), " [%zu] ", entry_idx + 1);

    auto row_color = selected ? fg_color : muted_color;

    return hbox({
        text(prefix_buf) | ftxui::color(selected ? accent_color : muted_color),
        text(entry_text) | ftxui::color(row_color),
    });
}

ftxui::Element InputBar::render_prompt_row() const {
    auto prefix_color = color_for(theme_, ThemeRole::PromptPrefix);
    auto fg_color     = color_for(theme_, ThemeRole::Fg);
    auto muted_color  = color_for(theme_, ThemeRole::Muted);

    // TUI-FLOW-T4 + TUI-FLOW-T9: show contextual placeholder when splash is
    // visible and the input buffer is logically empty.
    if (splash_showing_ && is_empty()) {
        std::string placeholder_text;
        if (placeholder_templates_.empty()) {
            placeholder_text = std::string(kSplashPlaceholder);
        } else {
            int slot = (placeholder_frame_counter_ / kPlaceholderFrameThrottle)
                       % static_cast<int>(placeholder_templates_.size());
            placeholder_text = placeholder_templates_[static_cast<std::size_t>(slot)];
        }
        ++placeholder_frame_counter_;

        return hbox({
            text("> ")             | ftxui::color(prefix_color) | bold,
            text(placeholder_text) | ftxui::color(muted_color),
        });
    }

    // UX-B: Build a flat display string with an inline cursor marker so
    // paragraph() can wrap the entire content at the terminal width.
    // Cursor is shown as "|" (U+007C, plain ASCII pipe) at the insertion
    // point — visible at any wrap position without additional styled spans.
    // Paste segments are inlined as their chip labels (readable plain text).
    // The "> " prefix stays on the first row; paragraph() handles wrapping
    // of the content area that follows it.
    const std::size_t flat_cur = flat_cursor_offset();
    std::size_t flat_pos = 0;
    std::string display;

    for (const auto& seg : segments_) {
        if (is_text(seg)) {
            const std::string& body = seg.body;
            if (flat_cur >= flat_pos && flat_cur <= flat_pos + body.size()) {
                // Cursor lands inside this text segment
                std::size_t local_cur = flat_cur - flat_pos;
                display += body.substr(0, local_cur);
                display += '|';   // inline cursor marker
                display += body.substr(local_cur);
            } else {
                display += body;
            }
            flat_pos += body.size();
        } else {
            // Paste segment: cursor before chip?
            if (flat_cur == flat_pos) {
                display += '|';
            }
            display += paste_chip_label(seg);
            flat_pos += seg.body.size();
        }
    }

    // Cursor at the very end (after all segments)
    if (flat_cur == flat_pos && !display.empty() && display.back() != '|') {
        display += '|';
    } else if (display.empty()) {
        display = '|';
    }

    std::string vim_indicator;
    if (vim_mode_.is_enabled()) {
        vim_indicator = "  " + vim_mode_.mode_indicator();
    }
    if (!vim_indicator.empty()) {
        display += vim_indicator;
    }

    // paragraph() wraps the content at the available column width and correctly
    // propagates multi-row height back to the parent vbox via Check().
    // The "> " prefix stays on the first visual row as the first hbox child.
    return hbox({
        text("> ") | ftxui::color(prefix_color) | bold,
        paragraph(display) | ftxui::color(fg_color),
    });
}

ftxui::Element InputBar::render_status_row() const {
    auto muted_color   = color_for(theme_, ThemeRole::Muted);
    auto accent_color  = color_for(theme_, ThemeRole::AccentCyan);
    auto magenta_color = color_for(theme_, ThemeRole::AccentMagenta);
    std::string model  = status_.model_name.empty() ? "no model" : status_.model_name;

    // A3: dirty-string cache — rebuild formatted strings only when usage changed.
    // set_usage() sets status_strs_dirty_ = true; we clear it after rebuild.
    // Combined with snprintf helpers: 1 allocation per turn instead of per frame.
    if (status_strs_dirty_) {
        cached_token_str_  = format_tokens(status_.token_count);
        cached_cost_str_   = format_cost(status_.cost_usd);
        status_strs_dirty_ = false;
    }
    const std::string& tokens = cached_token_str_;
    const std::string& cost   = cached_cost_str_;
    std::string mode   = status_.mode_label.empty() ? "default" : status_.mode_label;

    Elements parts;
    parts.push_back(text("  ◉ ") | ftxui::color(magenta_color));
    parts.push_back(text(model)  | ftxui::color(accent_color));
    parts.push_back(text(" · ")  | ftxui::color(muted_color));
    parts.push_back(text(tokens) | ftxui::color(muted_color));
    parts.push_back(text(" · ")  | ftxui::color(muted_color));
    parts.push_back(text(cost)   | ftxui::color(muted_color));
    parts.push_back(text(" · ")  | ftxui::color(muted_color));
    parts.push_back(text(mode)   | ftxui::color(muted_color));

    if (running_tool_.has_value() && !running_tool_->empty()) {
        parts.push_back(text(" · ") | ftxui::color(muted_color));
        parts.push_back(text("running: ") | ftxui::color(magenta_color));
        parts.push_back(text(*running_tool_) | ftxui::color(magenta_color) | bold);
    } else if (thinking_) {
        parts.push_back(text(" · ") | ftxui::color(muted_color));
        parts.push_back(text("thinking...") | ftxui::color(muted_color));
    }

    if (perf_hud_enabled_) {
        auto snap = batbox::perf::g_perf.snapshot();
        char hud_buf[128];
        std::snprintf(hud_buf, sizeof(hud_buf),
            " ⚡ first=%ldms · paint=%ldms · frame=%ldms",
            static_cast<long>(snap.first_token_ms),
            static_cast<long>(snap.stream_to_paint_ms),
            static_cast<long>(snap.frame_ms));
        parts.push_back(text(hud_buf) | ftxui::color(magenta_color));
    }

    return hbox(std::move(parts));
}

ftxui::Element InputBar::render_palette_overlay() const {
    auto fg_color      = color_for(theme_, ThemeRole::Fg);
    auto bg_color      = color_for(theme_, ThemeRole::Bg);
    auto accent_color  = color_for(theme_, ThemeRole::AccentMagenta);
    auto muted_color   = color_for(theme_, ThemeRole::Muted);
    auto select_color  = color_for(theme_, ThemeRole::AccentCyan);

    Elements rows;

    rows.push_back(hbox({
        text("/ ") | ftxui::color(accent_color) | bold,
        text(palette_filter_str_) | ftxui::color(fg_color),
        text("_") | ftxui::color(fg_color) | bold,
    }));

    rows.push_back(separator());

    constexpr int kMaxVisible = 8;
    int start = 0;
    if (palette_selected_ >= kMaxVisible) {
        start = palette_selected_ - kMaxVisible + 1;
    }

    if (palette_filtered_.empty()) {
        rows.push_back(text("  (no matches)") | ftxui::color(muted_color));
    } else {
        int end = std::min(start + kMaxVisible,
                           static_cast<int>(palette_filtered_.size()));
        for (int i = start; i < end; ++i) {
            const auto& item = palette_filtered_[static_cast<std::size_t>(i)];
            if (i == palette_selected_) {
                rows.push_back(
                    hbox({
                        text(" > ") | ftxui::color(accent_color) | bold,
                        text("/" + item) | ftxui::color(select_color) | bold,
                    })
                );
            } else {
                rows.push_back(
                    hbox({
                        text("   "),
                        text("/" + item) | ftxui::color(fg_color),
                    })
                );
            }
        }
    }

    return vbox(std::move(rows))
         | border
         | bgcolor(bg_color);
}

// =============================================================================
// Footer hint chips (TUI-FLOW-T6 + TUI-FIX-T10)
// =============================================================================

std::pair<std::string, std::string> InputBar::compute_footer_chips() const {
    if (splash_showing_) {
        return {"? for shortcuts", "@ for agents"};
    }

    std::string left  = stream_active_ ? "esc to interrupt" : "";
    std::string right;

    // TUI-FIX-T10: queue chip takes priority when queue is non-empty.
    if (!queue_.empty()) {
        const std::size_t depth = queue_.depth();
        char chip_buf[64];
        // Truncation rule: TERM_WIDTH < 80 → drop hint, keep "[N queued]"
        // We check terminal width via the COLUMNS env var (set by most shells),
        // or fall back to 80 columns.  This is resolved here (not in hot render).
        int term_width = 80;
        const char* cols_env = std::getenv("COLUMNS");
        if (cols_env) {
            int w = std::atoi(cols_env);
            if (w > 0) term_width = w;
        }
        if (term_width < 80) {
            std::snprintf(chip_buf, sizeof(chip_buf), "[%zu queued]", depth);
        } else {
            std::snprintf(chip_buf, sizeof(chip_buf),
                          "[%zu queued · Tab: add · Enter: steer · ↑: edit]", depth);
        }
        right = chip_buf;
        return {left, right};
    }

    const int mcp_n = mcp_failed_.load(std::memory_order_relaxed);
    if (mcp_n > 0) {
        right = std::to_string(mcp_n) + " MCP server failed · /mcp";
    } else {
        right = "thinking effort: " + effort_level_;
    }
    return {left, right};
}

ftxui::Element InputBar::render_footer_chips_row() const {
    auto muted_color   = color_for(theme_, ThemeRole::Muted);
    auto magenta_color = color_for(theme_, ThemeRole::AccentMagenta);
    auto cyan_color    = color_for(theme_, ThemeRole::AccentCyan);
    auto error_color   = color_for(theme_, ThemeRole::Error);
    auto [left, right] = compute_footer_chips();

    Elements parts;

    if (!left.empty()) {
        parts.push_back(text("  ") | ftxui::color(muted_color));
        parts.push_back(text(left) | ftxui::color(muted_color));
    }

    {
        const std::string& mode_str = status_.mode_label.empty()
                                      ? "default"
                                      : status_.mode_label;

        ftxui::Color mode_color = muted_color;
        bool         mode_bold  = false;
        if (mode_str == "plan") {
            mode_color = magenta_color;
        } else if (mode_str == "accept edits") {
            mode_color = cyan_color;
        } else if (mode_str == "NUCLEAR") {
            mode_color = error_color;
            mode_bold  = true;
        }

        if (!left.empty()) {
            parts.push_back(text(" · ") | ftxui::color(muted_color));
        } else {
            parts.push_back(text("  ") | ftxui::color(muted_color));
        }

        auto mode_elem = text("mode: " + mode_str) | ftxui::color(mode_color);
        if (mode_bold) {
            mode_elem = mode_elem | bold;
        }
        parts.push_back(mode_elem);
    }

    if (!right.empty()) {
        parts.push_back(text(" · ") | ftxui::color(muted_color));
        parts.push_back(text(right) | ftxui::color(muted_color));
    }

    return hbox(std::move(parts));
}

// =============================================================================
// OnEvent — all 10 TUI-FIX-T10 edge cases plus existing behaviour
// =============================================================================

bool InputBar::OnEvent(ftxui::Event event) {
    BATBOX_LOG_TRACE("InputBar::OnEvent: input_size={} input=[{}]",
                     event.input().size(), event.input());

    // --- Tool running / done events ---
    if (has_prefix_tool_running(event)) {
        auto p = batbox::tui::extract_tool_running(event);
        if (p.has_value()) {
            set_running_tool(p->tool_name.empty()
                ? std::optional<std::string>{}
                : std::optional<std::string>{p->tool_name});
        }
        return true;
    }
    if (has_prefix_tool_done(event)) {
        (void)batbox::tui::extract_tool_done(event);
        set_running_tool(std::nullopt);
        return true;
    }

    // --- TUI-T15: Thinking indicator events ---
    if (has_prefix_thinking_started(event)) {
        (void)batbox::tui::extract_thinking_started(event);
        thinking_ = true;
        return true;
    }
    if (has_prefix_thinking_stopped(event)) {
        (void)batbox::tui::extract_thinking_stopped(event);
        thinking_ = false;
        return true;
    }

    // --- A3: StatusUpdate with usage counters (TUI-FIX-T6) ---
    // Worker thread posts make_status_update_event_with_usage() from Conversation
    // after each terminal UsageDelta.  We extract here on the UI thread and call
    // set_usage() — never cross threads directly onto InputBar::status_.
    if (has_prefix_status_update(event)) {
        auto p = batbox::tui::extract_status_update(event);
        if (p.has_value() && p->tokens.has_value() && p->cost_usd.has_value()) {
            set_usage(*p->tokens, *p->cost_usd);
        }
        return true;
    }

    const auto& inp = event.input();

    // --- TUI-FIX-T4: Bracketed paste accumulation ---
    // Edge case #4: Tab during in_paste_seq_ is ignored as a queue command
    // because the paste accumulator owns all keystrokes until \e[201~.
    if (in_paste_seq_) {
        if (inp == kBracketedPasteEnd) {
            in_paste_seq_ = false;
            std::string body = std::move(paste_accumulator_);
            paste_accumulator_.clear();
            insert_paste(std::move(body));
            autocomplete_reset();
            return true;
        }
        // Accumulate bytes (including any Tab that arrives during paste)
        paste_accumulator_ += inp;
        return true;
    }
    if (inp == kBracketedPasteStart) {
        in_paste_seq_ = true;
        paste_accumulator_.clear();
        return true;
    }

    // --- TUI-FIX-T4: Additional Shift+Enter aliases ---
    if (inp == kShiftEnterAlt || inp == kShiftEnterCSIU) {
        insert_at_cursor("\n");
        autocomplete_reset();
        return true;
    }

    // --- Palette overlay intercepts most keys when open ---
    if (palette_open_) {
        if (inp == kInputEscape) {
            palette_close();
            return true;
        }
        if (inp == kInputReturn || inp == kInputCtrlM) {
            palette_commit();
            return true;
        }
        if (inp == kInputArrowUp) {
            if (palette_selected_ > 0) --palette_selected_;
            return true;
        }
        if (inp == kInputArrowDown) {
            if (!palette_filtered_.empty() &&
                palette_selected_ < static_cast<int>(palette_filtered_.size()) - 1) {
                ++palette_selected_;
            }
            return true;
        }
        if (inp == kInputBackspace || inp == kInputBackspace2) {
            if (!palette_filter_str_.empty()) {
                palette_filter_str_.pop_back();
                palette_filter();
            } else {
                palette_close();
            }
            return true;
        }
        if (is_printable_char(event)) {
            palette_filter_str_ += event.input();
            palette_filter();
            return true;
        }
        return false;
    }

    // =========================================================================
    // TUI-FIX-T10: Tab key — queue dispatch
    // Edge case #1: Tab idle + queue empty → existing autocomplete.
    // Edge case #2: Tab idle + queue non-empty → existing autocomplete.
    // Edge case #3: Tab mid-turn → append current to queue; clear input row.
    // Edge case #4: Tab during in_paste_seq_ → already handled above (IGNORED).
    // =========================================================================
    if (inp == kInputTab) {
        if (stream_active_) {
            // Edge case #3: turn in flight — append to queue
            queue_append_current();
            // cursor_idx_ stays at -1 (input row); queue depth shown in footer chip
        } else {
            // Edge case #1 / #2: idle — existing autocomplete (regardless of queue depth)
            autocomplete_next();
        }
        return true;
    }

    // =========================================================================
    // TUI-FIX-T10: Escape key — cancel-stream + preserve queue
    // Edge case #9: Esc streaming + queue empty → existing cancel (T3 path).
    // Edge case #10: Esc streaming + queue non-empty → cancel, PRESERVE queue.
    // =========================================================================
    if (inp == kInputEscape) {
        if (stream_active_ && on_interrupt_) {
            // Edge case #9 and #10: fire interrupt; queue is NOT cleared.
            on_interrupt_();
            return true;
        }
        if (!is_empty()) {
            clear();
            return true;
        }
        return false;
    }

    // =========================================================================
    // TUI-FIX-T10: Enter key — four paths
    // Edge case #5: Enter mid-turn → interrupt + submit combined message.
    // Edge case #6: Enter idle + queue empty → existing submit.
    // Edge case #7: Enter idle + current empty + queue non-empty → submit queue.
    // Edge case #8: Enter idle + current non-empty + queue non-empty → submit combined.
    // =========================================================================
    if (inp == kInputReturn || inp == kInputCtrlM) {
        if (stream_active_) {
            // Edge case #5: interrupt and submit combined message.
            if (on_interrupt_) on_interrupt_();
            std::string combined = build_combined_submit();
            if (!combined.empty()) {
                history_.push(combined);
                if (on_submit_) on_submit_(std::move(combined));
            }
            return true;
        }

        // Idle paths:
        if (queue_.empty()) {
            // Edge case #6: existing submit.
            if (!is_empty()) {
                auto submitted = flatten_for_submit();
                history_.push(submitted);
                clear();
                if (on_submit_) on_submit_(std::move(submitted));
                return true;
            }
            return false;
        }

        // Queue is non-empty:
        if (is_empty()) {
            // Edge case #7: empty current + non-empty queue → submit queue alone.
            std::string queued = build_combined_submit();
            if (!queued.empty()) {
                history_.push(queued);
                if (on_submit_) on_submit_(std::move(queued));
            }
            return true;
        }

        // Edge case #8: non-empty current + non-empty queue → submit combined.
        std::string combined = build_combined_submit();
        if (!combined.empty()) {
            history_.push(combined);
            if (on_submit_) on_submit_(std::move(combined));
        }
        return true;
    }

    // --- ReplAction dispatch via Keybindings ---
    auto action = keybindings_.event_to_action(event);
    if (action != batbox::repl::ReplAction::None) {
        return handle_action(action);
    }

    // --- Vim mode ---
    if (vim_mode_.is_enabled()) {
        if (!inp.empty() && event != ftxui::Event::Custom) {
            std::string flat_buf = flatten_for_submit();
            std::size_t flat_cur = flat_cursor_offset();
            auto vim_action = vim_mode_.handle_key(inp, flat_buf, flat_cur);
            using K = batbox::repl::VimActionKind;
            if (vim_action.kind == K::SendLine) {
                if (on_submit_) {
                    auto submitted = flatten_for_submit();
                    history_.push(submitted);
                    clear();
                    on_submit_(std::move(submitted));
                }
                return true;
            }
            if (vim_action.kind != K::Passthrough) {
                apply_vim_action(vim_action);
                return true;
            }
        }
    }

    // =========================================================================
    // TUI-FIX-T10: Arrow keys — queue navigation
    // Up from input row (-1) with non-empty queue → cursor_idx_ = N-1.
    // Down from queue row → cursor_idx_ = -1.
    // =========================================================================
    if (inp == kInputArrowUp) {
        if (cursor_idx_ == -1 && !queue_.empty()) {
            // Move cursor to last (most recent) queue row for editing
            cursor_idx_ = static_cast<int>(queue_.depth()) - 1;
            return true;
        }
        if (cursor_idx_ > 0) {
            --cursor_idx_;
            return true;
        }
        if (cursor_idx_ == -1) {
            // History navigation when queue is empty
            if (auto prev = history_.previous()) {
                set_buffer(*prev);
                return true;
            }
            return false;
        }
        return true;
    }
    if (inp == kInputArrowDown) {
        if (cursor_idx_ >= 0) {
            if (cursor_idx_ < static_cast<int>(queue_.depth()) - 1) {
                ++cursor_idx_;
            } else {
                // Bottom of queue → back to input row
                cursor_idx_ = -1;
            }
            return true;
        }
        // cursor_idx_ == -1: existing history-down behaviour
        if (auto nxt = history_.next()) {
            set_buffer(*nxt);
        } else {
            clear();
        }
        return true;
    }

    // --- Non-queue arrow/cursor movements (when cursor_idx_ == -1) ---
    if (inp == kInputArrowLeft) {
        if (seg_cursor_.seg_idx == 0 && seg_cursor_.byte_off == 0) {
            return true; // at start, clamp
        }
        if (seg_cursor_.byte_off > 0) {
            --seg_cursor_.byte_off;
            const auto& ts = segments_[seg_cursor_.seg_idx];
            while (seg_cursor_.byte_off > 0 &&
                   (static_cast<unsigned char>(ts.body[seg_cursor_.byte_off]) & 0xC0) == 0x80) {
                --seg_cursor_.byte_off;
            }
        } else {
            if (seg_cursor_.seg_idx > 0) {
                --seg_cursor_.seg_idx;
                const auto& prev = segments_[seg_cursor_.seg_idx];
                if (is_text(prev)) {
                    seg_cursor_.byte_off = prev.body.size();
                } else {
                    seg_cursor_.byte_off = 0;
                }
            }
        }
        return true;
    }
    if (inp == kInputArrowRight) {
        if (seg_cursor_.seg_idx >= segments_.size()) return true;

        const auto& seg = segments_[seg_cursor_.seg_idx];
        if (is_text(seg)) {
            if (seg_cursor_.byte_off < seg.body.size()) {
                unsigned char c = static_cast<unsigned char>(seg.body[seg_cursor_.byte_off]);
                std::size_t char_len = 1;
                if (c >= 0xF0)      char_len = 4;
                else if (c >= 0xE0) char_len = 3;
                else if (c >= 0xC0) char_len = 2;
                seg_cursor_.byte_off += std::min(char_len, seg.body.size() - seg_cursor_.byte_off);
            } else {
                if (seg_cursor_.seg_idx + 1 < segments_.size()) {
                    ++seg_cursor_.seg_idx;
                    seg_cursor_.byte_off = 0;
                    if (is_paste(segments_[seg_cursor_.seg_idx])) {
                        if (seg_cursor_.seg_idx + 1 < segments_.size()) {
                            ++seg_cursor_.seg_idx;
                            seg_cursor_.byte_off = 0;
                        }
                    }
                }
            }
        } else {
            if (seg_cursor_.seg_idx + 1 < segments_.size()) {
                ++seg_cursor_.seg_idx;
                seg_cursor_.byte_off = 0;
            }
        }
        return true;
    }
    if (inp == kInputHome) {
        seg_cursor_ = SegCursor{0, 0};
        return true;
    }
    if (inp == kInputEnd) {
        if (!segments_.empty()) {
            std::size_t last = segments_.size() - 1;
            if (is_text(segments_[last])) {
                seg_cursor_ = SegCursor{last, segments_[last].body.size()};
            } else {
                seg_cursor_ = SegCursor{last + 1, 0};
            }
        }
        return true;
    }

    // --- Backspace ---
    if (inp == kInputBackspace || inp == kInputBackspace2) {
        return handle_backspace();
    }

    // --- Delete ---
    if (inp == kInputDelete) {
        return handle_delete();
    }

    // --- '/' as first character: open palette ---
    if (inp == "/" && is_empty() && seg_cursor_.seg_idx == 0 && seg_cursor_.byte_off == 0) {
        segments_.clear();
        segments_.push_back(InputSegment::make_text("/"));
        seg_cursor_ = SegCursor{0, 1};
        palette_open();
        return true;
    }

    // --- Printable character ---
    if (is_printable_char(event)) {
        return handle_printable(event);
    }

    return false;
}

// =============================================================================
// Event handlers
// =============================================================================

bool InputBar::handle_printable(const ftxui::Event& ev) {
    insert_at_cursor(ev.input());
    autocomplete_reset();
    return true;
}

bool InputBar::handle_backspace() {
    if (seg_cursor_.seg_idx == 0 && seg_cursor_.byte_off == 0) return false;

    if (seg_cursor_.byte_off > 0) {
        auto& ts = segments_[seg_cursor_.seg_idx];
        std::size_t byte_pos = seg_cursor_.byte_off;
        --byte_pos;
        while (byte_pos > 0 &&
               (static_cast<unsigned char>(ts.body[byte_pos]) & 0xC0) == 0x80) {
            --byte_pos;
        }
        ts.body.erase(byte_pos, seg_cursor_.byte_off - byte_pos);
        seg_cursor_.byte_off = byte_pos;
        autocomplete_reset();
        normalize_segments();
        return true;
    }

    if (seg_cursor_.seg_idx > 0) {
        std::size_t prev_idx = seg_cursor_.seg_idx - 1;
        if (is_paste(segments_[prev_idx])) {
            // TUI-FIX-T4 AC4: single backspace deletes entire paste segment
            segments_.erase(segments_.begin() + static_cast<std::ptrdiff_t>(prev_idx));
            seg_cursor_.seg_idx = prev_idx;
            seg_cursor_.byte_off = 0;
            autocomplete_reset();
            normalize_segments();
            return true;
        }
        if (is_text(segments_[prev_idx])) {
            auto& ts = segments_[prev_idx];
            if (!ts.body.empty()) {
                std::size_t byte_pos = ts.body.size();
                --byte_pos;
                while (byte_pos > 0 &&
                       (static_cast<unsigned char>(ts.body[byte_pos]) & 0xC0) == 0x80) {
                    --byte_pos;
                }
                ts.body.erase(byte_pos, ts.body.size() - byte_pos);
                seg_cursor_.seg_idx = prev_idx;
                seg_cursor_.byte_off = byte_pos;
                autocomplete_reset();
                normalize_segments();
                return true;
            }
        }
    }
    return false;
}

bool InputBar::handle_delete() {
    if (seg_cursor_.seg_idx >= segments_.size()) return false;

    const auto& seg = segments_[seg_cursor_.seg_idx];
    if (is_text(seg)) {
        if (seg_cursor_.byte_off < seg.body.size()) {
            auto& ts_mut = segments_[seg_cursor_.seg_idx];
            std::size_t char_len = 1;
            unsigned char c = static_cast<unsigned char>(ts_mut.body[seg_cursor_.byte_off]);
            if (c >= 0xF0)      char_len = 4;
            else if (c >= 0xE0) char_len = 3;
            else if (c >= 0xC0) char_len = 2;
            char_len = std::min(char_len, ts_mut.body.size() - seg_cursor_.byte_off);
            ts_mut.body.erase(seg_cursor_.byte_off, char_len);
            autocomplete_reset();
            normalize_segments();
            return true;
        }
        if (seg_cursor_.seg_idx + 1 < segments_.size()) {
            if (is_paste(segments_[seg_cursor_.seg_idx + 1])) {
                segments_.erase(segments_.begin() +
                    static_cast<std::ptrdiff_t>(seg_cursor_.seg_idx + 1));
                autocomplete_reset();
                normalize_segments();
                return true;
            }
        }
    } else if (is_paste(seg)) {
        return false;
    }
    return false;
}

bool InputBar::handle_action(batbox::repl::ReplAction action) {
    using RA = batbox::repl::ReplAction;
    switch (action) {
        case RA::Send: {
            // ReplAction::Send mirrors the Enter key path (non-keybinding Enter
            // is intercepted above, but a keybinding-mapped Send lands here).
            if (stream_active_) {
                // Mid-turn: interrupt + submit combined
                if (on_interrupt_) on_interrupt_();
                std::string combined = build_combined_submit();
                if (!combined.empty()) {
                    history_.push(combined);
                    if (on_submit_) on_submit_(std::move(combined));
                }
                return true;
            }
            if (!queue_.empty()) {
                // Idle with queue: combined submit
                std::string combined = build_combined_submit();
                if (!combined.empty()) {
                    history_.push(combined);
                    if (on_submit_) on_submit_(std::move(combined));
                }
                return true;
            }
            // Plain idle submit
            if (is_empty()) return false;
            auto submitted = flatten_for_submit();
            history_.push(submitted);
            clear();
            if (on_submit_) on_submit_(std::move(submitted));
            return true;
        }
        case RA::Newline: {
            insert_at_cursor("\n");
            autocomplete_reset();
            return true;
        }
        case RA::Cancel: {
            if (stream_active_ && on_interrupt_) {
                // Edge case #9 / #10: Esc cancels stream; queue preserved.
                on_interrupt_();
                return true;
            }
            if (palette_open_) {
                palette_close();
                return true;
            }
            if (!is_empty()) {
                clear();
                return true;
            }
            return false;
        }
        case RA::HistoryUp: {
            if (cursor_idx_ == -1 && !queue_.empty()) {
                cursor_idx_ = static_cast<int>(queue_.depth()) - 1;
                return true;
            }
            if (cursor_idx_ == -1) {
                if (auto prev = history_.previous()) {
                    set_buffer(*prev);
                    return true;
                }
                return false;
            }
            if (cursor_idx_ > 0) { --cursor_idx_; return true; }
            return true;
        }
        case RA::HistoryDown: {
            if (cursor_idx_ >= 0) {
                if (cursor_idx_ < static_cast<int>(queue_.depth()) - 1) {
                    ++cursor_idx_;
                } else {
                    cursor_idx_ = -1;
                }
                return true;
            }
            if (auto nxt = history_.next()) {
                set_buffer(*nxt);
            } else {
                clear();
            }
            return true;
        }
        case RA::Clear: {
            clear();
            return true;
        }
        case RA::VimToggle: {
            vim_mode_.toggle();
            return true;
        }
        case RA::CycleMode: {
            if (perm_gate_) {
                const auto next = batbox::permissions::cycle_next(
                    perm_gate_->current_mode());
                perm_gate_->set_mode(next);
                status_.mode_label = mode_chip_label(next);
            }
            return true;
        }
        case RA::HistorySearch:
        case RA::None:
        default:
            return false;
    }
}

// =============================================================================
// Palette helpers
// =============================================================================

void InputBar::palette_open() {
    if (slash_provider_) {
        palette_all_ = slash_provider_();
        std::sort(palette_all_.begin(), palette_all_.end());
    } else {
        palette_all_.clear();
    }
    palette_filter_str_.clear();
    palette_filtered_ = palette_all_;
    palette_selected_ = 0;
    palette_open_     = true;
}

void InputBar::palette_close() {
    palette_open_       = false;
    palette_filter_str_.clear();
    palette_filtered_.clear();
    palette_all_.clear();
    palette_selected_   = 0;
}

void InputBar::palette_filter() {
    palette_filtered_.clear();
    palette_selected_ = 0;
    const auto& q = palette_filter_str_;
    for (const auto& item : palette_all_) {
        if (q.empty() || ci_contains(item, q)) {
            palette_filtered_.push_back(item);
        }
    }
}

void InputBar::palette_commit() {
    if (palette_filtered_.empty()) {
        palette_close();
        return;
    }
    std::size_t idx = static_cast<std::size_t>(
        std::max(0, std::min(palette_selected_,
                             static_cast<int>(palette_filtered_.size()) - 1)));
    std::string chosen = "/" + palette_filtered_[idx];
    palette_close();
    segments_.clear();
    segments_.push_back(InputSegment::make_text(chosen));
    seg_cursor_ = SegCursor{0, chosen.size()};
}

// =============================================================================
// Autocomplete helpers
// =============================================================================

void InputBar::autocomplete_next() {
    if (!ac_provider_) return;

    if (ac_candidates_.empty()) {
        ac_prefix_     = flatten_for_submit();
        ac_candidates_ = ac_provider_(ac_prefix_);
        ac_index_      = -1;
        if (ac_candidates_.empty()) return;
    }

    ac_index_ = (ac_index_ + 1) % static_cast<int>(ac_candidates_.size());
    const std::string& candidate = ac_candidates_[static_cast<std::size_t>(ac_index_)];
    segments_.clear();
    segments_.push_back(InputSegment::make_text(candidate));
    seg_cursor_ = SegCursor{0, candidate.size()};
}

void InputBar::autocomplete_reset() {
    ac_candidates_.clear();
    ac_index_ = -1;
    ac_prefix_.clear();
}

// =============================================================================
// VimAction application
// =============================================================================

void InputBar::apply_vim_action(const batbox::repl::VimAction& action) {
    using K = batbox::repl::VimActionKind;

    std::string flat_buf = flatten_for_submit();
    std::size_t flat_cur = flat_cursor_offset();

    switch (action.kind) {
        case K::InsertChar:
            if (action.ch != '\0') {
                flat_buf.insert(flat_cur, 1, action.ch);
                set_buffer(flat_buf);
                set_seg_cursor_from_flat(flat_cur + 1);
            }
            break;
        case K::DeleteRange:
            if (action.start <= action.end && action.end <= flat_buf.size()) {
                flat_buf.erase(action.start, action.end - action.start);
                set_buffer(flat_buf);
                set_seg_cursor_from_flat(action.start);
            }
            break;
        case K::ReplaceRange:
            if (action.start <= action.end && action.end <= flat_buf.size()) {
                flat_buf.replace(action.start, action.end - action.start, action.text);
                set_buffer(flat_buf);
                set_seg_cursor_from_flat(action.start + action.text.size());
            }
            break;
        case K::MoveCursor:
            set_seg_cursor_from_flat(std::min(action.cursor_pos, flat_buf.size()));
            break;
        case K::SetBuffer:
            set_buffer(action.text);
            set_seg_cursor_from_flat(std::min(action.cursor_pos, action.text.size()));
            break;
        case K::ChangeMode:
            break;
        case K::ClearLine:
            clear();
            break;
        case K::SendLine:
            // Handled in OnEvent before apply_vim_action
            break;
        case K::NoOp:
        case K::Passthrough:
        default:
            break;
    }
}

// =============================================================================
// Factory
// =============================================================================

ftxui::Component make_input_bar(
    batbox::theme::ThemeRef          theme,
    batbox::repl::History&           history,
    batbox::repl::Keybindings&       keybindings,
    InputBar::SubmitCallback         on_submit,
    InputBar::SlashCommandProvider   slash_provider,
    InputBar::AutocompleteProvider   ac_provider)
{
    return std::make_shared<InputBar>(
        theme,
        history,
        keybindings,
        std::move(on_submit),
        std::move(slash_provider),
        std::move(ac_provider));
}

} // namespace batbox::tui
