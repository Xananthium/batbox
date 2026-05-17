// src/tui/InputBar.cpp
// ---------------------------------------------------------------------------
// batbox::tui::InputBar implementation.
//
// See include/batbox/tui/InputBar.hpp for design notes and API contract.
// ---------------------------------------------------------------------------

#include <batbox/tui/InputBar.hpp>
#include <batbox/perf/PerfSnapshot.hpp>
#include <batbox/tui/Events.hpp>
#include <batbox/core/Logging.hpp>
#include <batbox/permissions/PermissionGate.hpp>
#include <batbox/permissions/PermissionMode.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstddef>
#include <sstream>
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

/// Default contextual placeholder shown when the splash banner is visible.
/// Matches the acceptance criterion: 'Try "/help" or "plan a feature"'.
/// Also serves as the index-0 fallback when placeholder_templates_ is empty.
constexpr std::string_view kSplashPlaceholder = "Try '/help' or 'plan a feature'";

/// Number of render frames between placeholder template advances (TUI-FLOW-T9).
/// At ~60fps this is approximately 2 seconds per template.
constexpr int kPlaceholderFrameThrottle = 120;

/// Format a token count with comma separators.
std::string format_tokens(uint32_t n) {
    if (n < 1000) return std::to_string(n) + "tk";
    std::string s = std::to_string(n);
    int insert_pos = static_cast<int>(s.size()) - 3;
    while (insert_pos > 0) {
        s.insert(static_cast<std::size_t>(insert_pos), ",");
        insert_pos -= 3;
    }
    return s + "tk";
}

/// Format a cost in USD as "$X.XXX".
std::string format_cost(double usd) {
    std::ostringstream ss;
    ss.precision(3);
    ss << std::fixed << "$" << usd;
    return ss.str();
}

/// Map a PermissionMode to the short display label shown in the footer chip.
/// Default → "default"  Plan → "plan"  AcceptEdits → "accept edits"
/// Nuclear  → "NUCLEAR" (all-caps to signal danger)
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

/// Returns true when the event is a ToolRunning payload event (has the prefix).
bool has_prefix_tool_running(const ftxui::Event& ev) {
    static constexpr const char* kPrefix = "batbox.tool-running";
    const std::string& s = ev.input();
    const std::size_t plen = std::strlen(kPrefix);
    return s.size() > plen && s.compare(0, plen, kPrefix) == 0 && s[plen] == ':';
}

/// Returns true when the event is a ToolDone payload event (has the prefix).
bool has_prefix_tool_done(const ftxui::Event& ev) {
    static constexpr const char* kPrefix = "batbox.tool-done";
    const std::string& s = ev.input();
    const std::size_t plen = std::strlen(kPrefix);
    return s.size() > plen && s.compare(0, plen, kPrefix) == 0 && s[plen] == ':';
}

/// Returns true when the event is a ThinkingStarted payload event (has the prefix).
bool has_prefix_thinking_started(const ftxui::Event& ev) {
    static constexpr const char* kPrefix = "batbox.thinking-started";
    const std::string& s = ev.input();
    const std::size_t plen = std::strlen(kPrefix);
    return s.size() > plen && s.compare(0, plen, kPrefix) == 0 && s[plen] == ':';
}

/// Returns true when the event is a ThinkingStopped payload event (has the prefix).
bool has_prefix_thinking_stopped(const ftxui::Event& ev) {
    static constexpr const char* kPrefix = "batbox.thinking-stopped";
    const std::string& s = ev.input();
    const std::size_t plen = std::strlen(kPrefix);
    return s.size() > plen && s.compare(0, plen, kPrefix) == 0 && s[plen] == ':';
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
    // Respect BATBOX_VIM_MODE env var at construction
    const char* vim_env = std::getenv("BATBOX_VIM_MODE");
    if (vim_env && std::string_view(vim_env) == "true") {
        vim_mode_.set_enabled(true);
    }

    // TUI-FLOW-T3: check BATBOX_PERF_HUD once at construction — zero overhead
    // when the env var is unset (no per-frame getenv calls).
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
    placeholder_frame_counter_ = 0;   // reset rotation on new template list
}

// =============================================================================
// Public API — footer hint chips (TUI-FLOW-T6)
// =============================================================================

void InputBar::set_stream_active(bool active) {
    stream_active_ = active;
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
    // Initialise the mode label from the gate's current mode so the footer chip
    // shows the correct state even before the user presses Shift+Tab.
    if (perm_gate_) {
        status_.mode_label = mode_chip_label(perm_gate_->current_mode());
    }
}

// =============================================================================
// Public API — buffer
// =============================================================================

std::string InputBar::buffer() const {
    return buf_;
}

std::size_t InputBar::cursor() const noexcept {
    return cursor_;
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
    buf_.clear();
    cursor_ = 0;
    history_.reset_cursor();
    palette_close();
    autocomplete_reset();
    vim_mode_.reset();
}

void InputBar::set_buffer(std::string text) {
    buf_    = std::move(text);
    cursor_ = buf_.size();
    autocomplete_reset();
}

// =============================================================================
// Render
// =============================================================================

ftxui::Element InputBar::OnRender() {
    Elements rows;

    // Palette overlay renders above the prompt when open
    if (palette_open_) {
        rows.push_back(render_palette_overlay());
    }

    rows.push_back(render_prompt_row());
    rows.push_back(render_status_row());
    rows.push_back(render_footer_chips_row());

    return vbox(std::move(rows));
}

ftxui::Element InputBar::render_prompt_row() const {
    auto prefix_color = color_for(theme_, ThemeRole::PromptPrefix);
    auto fg_color     = color_for(theme_, ThemeRole::Fg);
    auto muted_color  = color_for(theme_, ThemeRole::Muted);

    // TUI-FLOW-T4 + TUI-FLOW-T9: show contextual placeholder when splash is
    // visible and the input buffer is empty.  The placeholder is chosen from the
    // rotating template list (set via set_placeholder_templates).  It vanishes
    // the moment the user types anything (buf_ becomes non-empty).
    if (splash_showing_ && buf_.empty()) {
        // TUI-FLOW-T9: select template by throttled frame counter.
        // Advance the counter every call (once per render frame).
        // Divide by kPlaceholderFrameThrottle to slow rotation to ~2s per step.
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
            text("> ")           | ftxui::color(prefix_color) | bold,
            text(placeholder_text) | ftxui::color(muted_color),
        });
    }

    // Normal rendering: split buffer at cursor for visual cursor highlight.
    std::string before_cursor = buf_.substr(0, cursor_);
    std::string at_cursor;
    std::string after_cursor;

    if (cursor_ < buf_.size()) {
        unsigned char c = static_cast<unsigned char>(buf_[cursor_]);
        std::size_t char_len = 1;
        if (c >= 0xF0)      char_len = 4;
        else if (c >= 0xE0) char_len = 3;
        else if (c >= 0xC0) char_len = 2;
        char_len = std::min(char_len, buf_.size() - cursor_);
        at_cursor    = buf_.substr(cursor_, char_len);
        after_cursor = buf_.substr(cursor_ + char_len);
    } else {
        at_cursor = " ";
    }

    Element text_element = hbox({
        text(before_cursor) | ftxui::color(fg_color),
        text(at_cursor)     | ftxui::color(color_for(theme_, ThemeRole::Bg))
                            | bgcolor(fg_color),
        text(after_cursor)  | ftxui::color(fg_color),
    });

    std::string vim_indicator;
    if (vim_mode_.is_enabled()) {
        vim_indicator = "  " + vim_mode_.mode_indicator();
    }

    return hbox({
        text("> ")  | ftxui::color(prefix_color) | bold,
        text_element,
        text(vim_indicator) | ftxui::color(muted_color),
    });
}

ftxui::Element InputBar::render_status_row() const {
    auto muted_color   = color_for(theme_, ThemeRole::Muted);
    auto accent_color  = color_for(theme_, ThemeRole::AccentCyan);
    auto magenta_color = color_for(theme_, ThemeRole::AccentMagenta);
    std::string model  = status_.model_name.empty() ? "no model" : status_.model_name;
    std::string tokens = format_tokens(status_.token_count);
    std::string cost   = format_cost(status_.cost_usd);
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

    // Append running tool indicator when a tool is in progress.
    // Priority: tool-running wins over thinking (both could theoretically be
    // active simultaneously; show the more specific indicator).
    if (running_tool_.has_value() && !running_tool_->empty()) {
        parts.push_back(text(" · ") | ftxui::color(muted_color));
        parts.push_back(text("running: ") | ftxui::color(magenta_color));
        parts.push_back(text(*running_tool_) | ftxui::color(magenta_color) | bold);
    } else if (thinking_) {
        // TUI-T15: reasoning phase active — show muted "thinking..." indicator
        // so the user knows inference is alive during Magistral's long reasoning.
        parts.push_back(text(" · ") | ftxui::color(muted_color));
        parts.push_back(text("thinking...") | ftxui::color(muted_color));
    }

    // TUI-FLOW-T3: perf HUD chip — zero overhead when env var is unset.
    // Appended as a right-side chip: "⚡ first=Xms · paint=Yms · frame=Zms"
    if (perf_hud_enabled_) {
        auto snap = batbox::perf::g_perf.snapshot();
        std::string hud = " â¡ first="
                        + std::to_string(snap.first_token_ms)
                        + "ms Â· paint="
                        + std::to_string(snap.stream_to_paint_ms)
                        + "ms Â· frame="
                        + std::to_string(snap.frame_ms)
                        + "ms";
        parts.push_back(text(hud) | ftxui::color(magenta_color));
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
// Footer hint chips (TUI-FLOW-T6)
// =============================================================================

std::pair<std::string, std::string> InputBar::compute_footer_chips() const {
    // Splash-showing mode: helper discovery chips replace the regular chips.
    if (splash_showing_) {
        return {"? for shortcuts", "@ for agents"};
    }

    // Left chip: "esc to interrupt" only while a stream is in flight.
    std::string left = stream_active_ ? "esc to interrupt" : "";

    // Right chip: MCP failure takes priority over effort level.
    std::string right;
    const int mcp_n = mcp_failed_.load(std::memory_order_relaxed);
    if (mcp_n > 0) {
        right = std::to_string(mcp_n) + " MCP server failed Â· /mcp";
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

    // --- Left chip (stream interrupt or splash hints) ---
    if (!left.empty()) {
        parts.push_back(text("  ") | ftxui::color(muted_color));
        parts.push_back(text(left) | ftxui::color(muted_color));
    }

    // --- Mode chip (TUI-PERM-T1) ---
    // The mode chip is rendered to the right of the left chip (or at the
    // left margin when there is no left chip).  It uses color hinting so the
    // user can glance at the color to know which mode is active:
    //   Default     → Muted (neutral; normal operation)
    //   Plan        → AccentMagenta (caution; write tools blocked)
    //   AcceptEdits → AccentCyan (info; edit tools auto-approved)
    //   Nuclear     → Error + bold (danger; ALL tools auto-approved)
    {
        const std::string& mode_str = status_.mode_label.empty()
                                      ? "default"
                                      : status_.mode_label;

        // Determine mode from label for color selection (avoids storing the
        // PermissionMode enum separately; label is already the canonical text).
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
        // Default stays muted — no special color.

        // Separator before mode chip.
        if (!left.empty()) {
            parts.push_back(text(" Â· ") | ftxui::color(muted_color));
        } else {
            parts.push_back(text("  ") | ftxui::color(muted_color));
        }

        auto mode_elem = text("mode: " + mode_str) | ftxui::color(mode_color);
        if (mode_bold) {
            mode_elem = mode_elem | bold;
        }
        parts.push_back(mode_elem);
    }

    // --- Right chip (effort / MCP failures) ---
    if (!right.empty()) {
        parts.push_back(text(" Â· ") | ftxui::color(muted_color));
        parts.push_back(text(right) | ftxui::color(muted_color));
    }

    return hbox(std::move(parts));
}

// =============================================================================
// OnEvent
// =============================================================================

bool InputBar::OnEvent(ftxui::Event event) {
    BATBOX_LOG_TRACE("InputBar::OnEvent: input_size={} input=[{}]",
                     event.input().size(), event.input());

    // --- Tool running / done events: update the status row indicator ---
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

    // --- Palette overlay intercepts most keys when open ---
    if (palette_open_) {
        const auto& inp = event.input();

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

    // --- ReplAction dispatch via Keybindings ---
    auto action = keybindings_.event_to_action(event);
    if (action != batbox::repl::ReplAction::None) {
        return handle_action(action);
    }

    // --- Vim mode ---
    if (vim_mode_.is_enabled()) {
        const auto& inp = event.input();
        if (!inp.empty() && event != ftxui::Event::Custom) {
            auto vim_action = vim_mode_.handle_key(inp, buf_, cursor_);
            using K = batbox::repl::VimActionKind;
            if (vim_action.kind == K::SendLine) {
                if (on_submit_) {
                    auto submitted = buf_;
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

    // --- Arrow keys for history (when vim mode off or in passthrough) ---
    const auto& inp = event.input();

    if (inp == kInputArrowUp) {
        if (auto prev = history_.previous()) {
            set_buffer(*prev);
            return true;
        }
        return false;
    }
    if (inp == kInputArrowDown) {
        if (auto nxt = history_.next()) {
            set_buffer(*nxt);
        } else {
            clear();
        }
        return true;
    }

    // --- Cursor movement ---
    if (inp == kInputArrowLeft) {
        if (cursor_ > 0) --cursor_;
        return true;
    }
    if (inp == kInputArrowRight) {
        if (cursor_ < buf_.size()) ++cursor_;
        return true;
    }
    if (inp == kInputHome) {
        cursor_ = 0;
        return true;
    }
    if (inp == kInputEnd) {
        cursor_ = buf_.size();
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

    // --- Plain Enter / Ctrl+M: submit (when not vim mode) ---
    if (inp == kInputReturn || inp == kInputCtrlM) {
        if (!buf_.empty()) {
            auto submitted = buf_;
            history_.push(submitted);
            clear();
            if (on_submit_) on_submit_(std::move(submitted));
            return true;
        }
        return false;
    }

    // --- Tab: autocomplete ---
    if (inp == kInputTab) {
        autocomplete_next();
        return true;
    }

    // --- '/' as first character: open palette ---
    if (inp == "/" && buf_.empty() && cursor_ == 0) {
        buf_    = "/";
        cursor_ = 1;
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
    if (cursor_ == 0 || buf_.empty()) return false;

    std::size_t byte_pos = cursor_;
    --byte_pos;
    while (byte_pos > 0 &&
           (static_cast<unsigned char>(buf_[byte_pos]) & 0xC0) == 0x80) {
        --byte_pos;
    }
    buf_.erase(byte_pos, cursor_ - byte_pos);
    cursor_ = byte_pos;
    autocomplete_reset();
    return true;
}

bool InputBar::handle_delete() {
    if (cursor_ >= buf_.size()) return false;

    std::size_t char_len = 1;
    unsigned char c = static_cast<unsigned char>(buf_[cursor_]);
    if (c >= 0xF0)      char_len = 4;
    else if (c >= 0xE0) char_len = 3;
    else if (c >= 0xC0) char_len = 2;
    char_len = std::min(char_len, buf_.size() - cursor_);
    buf_.erase(cursor_, char_len);
    autocomplete_reset();
    return true;
}

bool InputBar::handle_action(batbox::repl::ReplAction action) {
    using RA = batbox::repl::ReplAction;
    switch (action) {
        case RA::Send: {
            if (buf_.empty()) return false;
            auto submitted = buf_;
            history_.push(submitted);
            clear();
            if (on_submit_) on_submit_(std::move(submitted));
            return true;
        }
        case RA::Newline: {
            insert_at_cursor("\n");
            return true;
        }
        case RA::Cancel: {
            if (palette_open_) {
                palette_close();
                return true;
            }
            if (!buf_.empty()) {
                clear();
                return true;
            }
            return false;
        }
        case RA::HistoryUp: {
            if (auto prev = history_.previous()) {
                set_buffer(*prev);
                return true;
            }
            return false;
        }
        case RA::HistoryDown: {
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
            // TUI-PERM-T1: cycle the active permission mode through
            // Default → Plan → AcceptEdits → Nuclear → Default.
            // No-op when no gate is wired (e.g. unit tests without a gate).
            if (perm_gate_) {
                const auto next = batbox::permissions::cycle_next(
                    perm_gate_->current_mode());
                perm_gate_->set_mode(next);
                // Keep status_.mode_label in sync so the status row reflects
                // the new mode immediately (same frame as the key press).
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
    buf_    = std::move(chosen);
    cursor_ = buf_.size();
}

// =============================================================================
// Autocomplete helpers
// =============================================================================

void InputBar::autocomplete_next() {
    if (!ac_provider_) return;

    if (ac_candidates_.empty()) {
        ac_prefix_      = buf_;
        ac_candidates_  = ac_provider_(ac_prefix_);
        ac_index_       = -1;
        if (ac_candidates_.empty()) return;
    }

    ac_index_ = (ac_index_ + 1) % static_cast<int>(ac_candidates_.size());
    buf_    = ac_candidates_[static_cast<std::size_t>(ac_index_)];
    cursor_ = buf_.size();
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
    switch (action.kind) {
        case K::InsertChar:
            if (action.ch != '\0') {
                buf_.insert(cursor_, 1, action.ch);
                ++cursor_;
            }
            break;
        case K::DeleteRange:
            if (action.start <= action.end && action.end <= buf_.size()) {
                buf_.erase(action.start, action.end - action.start);
                cursor_ = action.start;
            }
            break;
        case K::ReplaceRange:
            if (action.start <= action.end && action.end <= buf_.size()) {
                buf_.replace(action.start, action.end - action.start, action.text);
                cursor_ = action.start + action.text.size();
            }
            break;
        case K::MoveCursor:
            cursor_ = std::min(action.cursor_pos, buf_.size());
            break;
        case K::SetBuffer:
            buf_    = action.text;
            cursor_ = std::min(action.cursor_pos, buf_.size());
            break;
        case K::ChangeMode:
            break;
        case K::ClearLine:
            buf_.clear();
            cursor_ = 0;
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
// Internal helpers
// =============================================================================

void InputBar::insert_at_cursor(std::string_view text) {
    buf_.insert(cursor_, text);
    cursor_ += text.size();
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
