// include/batbox/tui/InputBar.hpp
// ---------------------------------------------------------------------------
// batbox::tui::InputBar — bottom-of-screen FTXUI component.
//
// Layout (4 lines reserved):
//
//   ┌────────────────────────────────────────────┐
//   │ > user input here█                         │
//   │ ◉ claude-sonnet-4 · 1,247tk · $0.012 · ins │
//   └────────────────────────────────────────────┘
//
//   When palette overlay is open ('/') an overlay drops above the input:
//
//   ┌────────────────────────────────────────────┐
//   │ ╔══════════════════════════════════════╗   │
//   │ ║ /  [filter text]                     ║   │
//   │ ║  > /clear                            ║   │
//   │ ║    /compact                          ║   │
//   │ ║    /exit                             ║   │
//   │ ╚══════════════════════════════════════╝   │
//   │ > /                                        │
//   │ ◉ claude-sonnet-4 · 0tk · $0.000 · ins     │
//   └────────────────────────────────────────────┘
//
//   When splash_showing_ is true and buf_ is empty, a muted placeholder is
//   rendered instead of the blank cursor line (TUI-FLOW-T4):
//
//   ┌────────────────────────────────────────────┐
//   │ > Try '/help' or 'plan a feature'          │  (muted, disappears on type)
//   │ ◉ claude-sonnet-4 · 0tk · $0.000 · ins     │
//   └────────────────────────────────────────────┘
//
// Design
// ------
//  • Inherits from ftxui::ComponentBase; constructed via
//    batbox::tui::make_input_bar() factory returning ftxui::Component.
//  • Input buffer + cursor are owned internally; callers receive completed
//    lines via an on_submit callback.
//  • Keybindings wired via batbox::repl::Keybindings for ReplAction dispatch.
//  • VimMode optional — toggled by ReplAction::VimToggle / BATBOX_VIM_MODE.
//  • History navigation via injected batbox::repl::History reference.
//  • Slash palette: '/' as first char shows filterable overlay. Arrow + Enter
//    select; Escape dismisses.
//  • Tab autocomplete: rotates completions from injected provider callback.
//  • Splash placeholder: set_splash_showing(true) before first prompt.
//    Cleared by set_splash_showing(false) after first submit.
//
// Thread safety
// -------------
//  All methods must be called from the FTXUI event loop thread (main thread).
//  StatusLine fields are updated from the same thread via set_status().
//
// Blueprint contract (Non-technical Deb lock):
//   class  batbox::tui::InputBar : public ftxui::ComponentBase
//   Symbol names used below are locked by CPP 1.9 blueprint rows.
// ---------------------------------------------------------------------------
#pragma once

#include <batbox/repl/History.hpp>
#include <batbox/repl/Keybindings.hpp>
#include <batbox/repl/VimMode.hpp>
#include <batbox/theme/Theme.hpp>
#include <batbox/tui/ThemeApply.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

// Forward declaration: PermissionGate is used only via raw pointer (non-owning)
// to avoid pulling the full header into every TUI consumer.  The full header
// is included in InputBar.cpp where the actual call sites live.
namespace batbox::permissions { class PermissionGate; }

namespace batbox::tui {

// =============================================================================
// StatusLine — live display values shown below the input prompt.
// =============================================================================

/// Values shown in the status line below the prompt.
/// Updated by the application layer (Conversation, UsageTracker) via
/// InputBar::set_status().
struct StatusLine {
    std::string model_name;   ///< e.g. "claude-sonnet-4"
    uint32_t    token_count{0}; ///< Cumulative input + output tokens this session
    double      cost_usd{0.0};  ///< Cumulative cost in USD
    std::string mode_label;   ///< e.g. "default", "plan", "nuclear"
};

// =============================================================================
// InputBar — the FTXUI component
// =============================================================================

class InputBar : public ftxui::ComponentBase {
public:
    // ---- Types ---------------------------------------------------------------

    /// Called with the completed input text when the user submits (Ctrl+Enter).
    using SubmitCallback = std::function<void(std::string)>;

    /// Called to obtain autocomplete candidates for the current prefix.
    /// Returns sorted list of candidate strings (may be empty).
    using AutocompleteProvider = std::function<std::vector<std::string>(std::string_view prefix)>;

    /// Called to obtain slash command names for the palette overlay.
    /// Returns sorted list of slash command names without the leading '/'.
    using SlashCommandProvider = std::function<std::vector<std::string>()>;

    // ---- Construction -------------------------------------------------------

    /// Construct an InputBar.
    ///
    /// @param theme            Active colour palette (ThemeRef = const Theme&).
    /// @param history          Injected History for up/down navigation (non-owning ref).
    /// @param keybindings      Injected Keybindings for ReplAction dispatch (non-owning ref).
    /// @param on_submit        Callback invoked when the user submits (Ctrl+Enter / Enter).
    /// @param slash_provider   Callback returning slash command names (may be nullptr for no palette).
    /// @param ac_provider      Callback returning autocomplete candidates (may be nullptr).
    InputBar(batbox::theme::ThemeRef       theme,
             batbox::repl::History&        history,
             batbox::repl::Keybindings&    keybindings,
             SubmitCallback                on_submit,
             SlashCommandProvider          slash_provider,
             AutocompleteProvider          ac_provider);

    ~InputBar() override = default;

    InputBar(const InputBar&)            = delete;
    InputBar& operator=(const InputBar&) = delete;
    InputBar(InputBar&&)                 = delete;
    InputBar& operator=(InputBar&&)      = delete;

    // ---- FTXUI interface ----------------------------------------------------

    /// Render the input bar (prompt row + status row + optional palette overlay).
    ftxui::Element OnRender() override;

    /// Handle keyboard and custom events.
    bool OnEvent(ftxui::Event event) override;

    /// InputBar is always focusable so Container::Vertical routes keyboard
    /// events to it.  Without this override, ComponentBase::Focusable()
    /// recurses into children (none), returns false, and the container
    /// silently drops all keystrokes before OnEvent() is ever called.
    bool Focusable() const override { return true; }

    // ---- Status line update (thread-unsafe: call from UI thread only) -------

    /// Replace all status-line fields at once.
    void set_status(StatusLine status);

    /// Update just the model name (called e.g. after /model changes).
    void set_model(std::string name);

    /// Update token count and cost (called after each streaming response).
    void set_usage(uint32_t tokens, double cost_usd);

    /// Update the mode label (called by PermissionMode/NuclearMode logic).
    void set_mode(std::string label);

    /// Set or clear the running tool indicator shown in the status row.
    ///
    /// When @p tool is non-empty, the status row appends " · running: <tool>"
    /// after the model/mode fields.  When @p tool is empty/nullopt, the indicator
    /// is cleared and the status row reverts to its baseline display.
    ///
    /// Call from the FTXUI main-loop thread only (via OnEvent handler).
    /// The worker thread signals via make_tool_running_event / make_tool_done_event;
    /// InputBar::OnEvent extracts those and calls set_running_tool() here.
    void set_running_tool(std::optional<std::string> tool);

    // ---- Splash placeholder (TUI-FLOW-T4) -----------------------------------

    /// Enable or disable the contextual placeholder text.
    ///
    /// When @p showing is true and the input buffer is empty, the prompt row
    /// renders the placeholder "Try '/help' or 'plan a feature'" in muted color
    /// instead of a blank input with cursor.  Disappears on first keypress.
    ///
    /// Call with false after the first submit to permanently hide the placeholder.
    ///
    /// Must be called from the UI thread.
    void set_splash_showing(bool showing);

    // ---- Contextual placeholder templates (TUI-FLOW-T9) ---------------------

    /// Set the list of rotating placeholder templates.
    ///
    /// Each call replaces the template list.  The first entry is always shown as
    /// the fallback (and should be the T4 default "Try '/help' or 'plan a feature'").
    /// Subsequent entries are chosen by rotating through the list on each render
    /// frame (once per N frames, where N=120 to throttle at ~2s per template).
    ///
    /// If the vector is empty the fallback constant kSplashPlaceholder is used.
    ///
    /// Must be called from the UI thread before the first render.
    void set_placeholder_templates(std::vector<std::string> templates);

    // ---- Footer hint chips (TUI-FLOW-T6) ------------------------------------

    /// Mark whether a model stream is currently in flight.
    ///
    /// When @p active is true the footer row shows "esc to interrupt" on the
    /// left side.  Set to true on UserMessage, false on StreamDone.
    ///
    /// Must be called from the UI thread (FTXUI event-loop thread).
    void set_stream_active(bool active);

    /// Set the current thinking-effort level shown in the footer right chip.
    ///
    /// Defaults to "medium". Accepted values: "low", "medium", "high".
    /// Displayed as "thinking effort: <level>" unless an MCP failure is active.
    ///
    /// Must be called from the UI thread.
    void set_effort_level(std::string level);

    /// Report the count of failed MCP servers for the right-side footer chip.
    ///
    /// When @p n > 0 the right chip shows "N MCP server failed · /mcp".
    /// When @p n == 0 (default) the chip reverts to the effort-level display.
    ///
    /// Called by TUI-FLOW-T11 when MCP server health changes.
    /// Ready for that task to populate; defaults to 0 (hidden) until then.
    ///
    /// Must be called from the UI thread.
    void set_mcp_failed(int n);

    // ---- Permission gate wiring (TUI-PERM-T1) --------------------------------

    /// Wire the PermissionGate so Shift+Tab (ReplAction::CycleMode) can cycle
    /// the active permission mode through Default → Plan → AcceptEdits → Nuclear.
    ///
    /// @param gate  Raw non-owning pointer to the application-owned PermissionGate.
    ///              When non-null, pressing Shift+Tab calls gate->set_mode(cycle_next())
    ///              and updates the footer mode chip accordingly.
    ///              When null (default), Shift+Tab is a no-op.
    ///
    /// The caller (WireTui) must ensure the gate outlives the InputBar component.
    /// Must be called from the UI thread before the event loop starts.
    void set_permission_gate(batbox::permissions::PermissionGate* gate);

    // ---- Buffer access (read-only, from UI thread) --------------------------

    /// Return a copy of the current input buffer.
    [[nodiscard]] std::string buffer() const;

    /// Return the current cursor position (byte offset into buffer).
    [[nodiscard]] std::size_t cursor() const noexcept;

    // ---- Vim mode -----------------------------------------------------------

    /// Enable or disable vim mode for this InputBar.
    void set_vim_enabled(bool enabled);

    /// Toggle vim mode on/off.
    void toggle_vim();

    // ---- Programmatic control -----------------------------------------------

    /// Clear the input buffer and reset state (cursor, history cursor, overlays).
    void clear();

    /// Set the buffer content and move cursor to end (for history navigation / paste).
    void set_buffer(std::string text);

private:
    // ---- Render helpers -----------------------------------------------------

    /// Render the single prompt row:  ">" prefix + text + cursor (or placeholder).
    ftxui::Element render_prompt_row() const;

    /// Render the status line row.
    ftxui::Element render_status_row() const;

    /// Render the slash palette overlay box (above the prompt).
    ftxui::Element render_palette_overlay() const;

    /// Render the footer hint chip row below the prompt row (TUI-FLOW-T6).
    /// Returns an element with left chip (stream/splash) and right chip
    /// (effort level or MCP failure count).  Both chips use Muted colour.
    ftxui::Element render_footer_chips_row() const;

    /// Compute the (left_text, right_text) pair for the footer chip row.
    ///
    /// Priority rules:
    ///   splash_showing_ → {"? for shortcuts", "@ for agents"}
    ///   stream_active_  → left = "esc to interrupt"
    ///   mcp_failed_ > 0 → right = "N MCP server failed · /mcp"
    ///   otherwise       → right = "thinking effort: <level>"
    std::pair<std::string, std::string> compute_footer_chips() const;

    // ---- Event handlers -----------------------------------------------------

    /// Handle a printable character insertion (Insert mode / non-vim).
    bool handle_printable(const ftxui::Event& ev);

    /// Handle Backspace.
    bool handle_backspace();

    /// Handle Delete key.
    bool handle_delete();

    /// Dispatch a ReplAction returned by keybindings.
    bool handle_action(batbox::repl::ReplAction action);

    // ---- Palette helpers ----------------------------------------------------

    /// Open the slash palette overlay; pre-populates list via slash_provider_.
    void palette_open();

    /// Close and dismiss the slash palette.
    void palette_close();

    /// Filter the palette list using current filter string.
    void palette_filter();

    /// Commit the currently selected palette item into the buffer.
    void palette_commit();

    // ---- Autocomplete helpers -----------------------------------------------

    /// Advance to the next autocomplete completion (Tab key).
    void autocomplete_next();

    /// Reset autocomplete state.
    void autocomplete_reset();

    // ---- Internal helpers ---------------------------------------------------

    /// Apply a VimAction returned by vim_mode_ to buf_ / cursor_.
    void apply_vim_action(const batbox::repl::VimAction& action);

    /// Insert a single UTF-8 character sequence at cursor.
    void insert_at_cursor(std::string_view text);

    // ---- State --------------------------------------------------------------

    // Theme reference (non-owning; outlives this component)
    const batbox::theme::Theme& theme_;

    // Injected dependencies (non-owning refs)
    batbox::repl::History&     history_;
    batbox::repl::Keybindings& keybindings_;

    // Callbacks
    SubmitCallback        on_submit_;
    SlashCommandProvider  slash_provider_;
    AutocompleteProvider  ac_provider_;

    // Input buffer
    std::string  buf_;
    std::size_t  cursor_{0};

    // Vim mode state machine
    batbox::repl::VimMode vim_mode_;

    // Status line
    StatusLine status_;

    // Running tool indicator (set during tool dispatch, cleared after).
    // When non-empty, displayed as " · running: <name>" in the status row.
    std::optional<std::string> running_tool_;

    // Thinking indicator (TUI-T15).
    // Set to true when a ThinkingStarted event arrives; cleared on ThinkingStopped.
    // Displayed as " · thinking..." in the status row when running_tool_ is empty.
    bool thinking_{false};

    // --- Splash placeholder (TUI-FLOW-T4) ---
    /// True while the SplashBanner is visible and no text has been typed.
    /// When true and buf_ is empty, the prompt row shows a muted placeholder
    /// instead of a blank cursor. Set to false by set_splash_showing(false)
    /// after the first submit, or by any keypress that puts text in buf_.
    bool splash_showing_{false};

    // --- TUI-FLOW-T9: contextual placeholder rotation ---
    /// Rotating placeholder templates.  Index 0 is the T4 default fallback.
    /// Populated by set_placeholder_templates(); empty = use kSplashPlaceholder.
    std::vector<std::string> placeholder_templates_;

    /// Frame counter incremented on each placeholder render call.
    /// Divided by kPlaceholderFrameThrottle to advance template index slowly.
    mutable int placeholder_frame_counter_{0};

    // --- TUI-FLOW-T6: footer hint chips ---
    /// True between UserMessage and StreamDone: shows "esc to interrupt" chip.
    bool stream_active_{false};

    /// Current thinking-effort level shown in the right footer chip.
    /// Default "medium"; overridden by set_effort_level().
    std::string effort_level_{"medium"};

    /// Count of currently-failed MCP servers (0 = chip hidden, >0 = chip shown).
    /// Populated by set_mcp_failed(); ready for TUI-FLOW-T11 to call.
    /// Changed to std::atomic<int> by TUI-FLOW-T11 so McpStatusPoller can
    /// call set_mcp_failed() from the poller thread without data races.
    std::atomic<int> mcp_failed_{0};

    // --- Slash palette overlay state ---
    bool                     palette_open_{false};
    std::string              palette_filter_str_;
    std::vector<std::string> palette_all_;      ///< full list from slash_provider_
    std::vector<std::string> palette_filtered_; ///< current filtered subset
    int                      palette_selected_{0};

    // --- Autocomplete state ---
    std::vector<std::string> ac_candidates_;
    int                      ac_index_{-1};
    std::string              ac_prefix_;   ///< prefix at time Tab was first pressed

    // --- TUI-FLOW-T3: perf HUD ---
    /// True when BATBOX_PERF_HUD env var is set and non-empty at construction.
    /// Checked once in the constructor — zero per-frame overhead when false.
    bool perf_hud_enabled_{false};

    // --- TUI-PERM-T1: permission gate for Shift+Tab mode cycle ---
    /// Non-owning raw pointer set by set_permission_gate(); null = no cycle.
    /// Null-safe: CycleMode handler checks before calling set_mode/current_mode.
    batbox::permissions::PermissionGate* perm_gate_{nullptr};
};

// =============================================================================
// Factory
// =============================================================================

/// Create a heap-allocated InputBar wrapped in ftxui::Component.
///
/// This is the intended construction path; callers hold an ftxui::Component
/// handle and may also keep a raw InputBar* via std::dynamic_pointer_cast if
/// they need to call set_status() etc.
[[nodiscard]]
ftxui::Component make_input_bar(
    batbox::theme::ThemeRef          theme,
    batbox::repl::History&           history,
    batbox::repl::Keybindings&       keybindings,
    InputBar::SubmitCallback         on_submit,
    InputBar::SlashCommandProvider   slash_provider = nullptr,
    InputBar::AutocompleteProvider   ac_provider    = nullptr);

} // namespace batbox::tui
