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
//   When the input queue has entries (TUI-FIX-T10), queued rows render above
//   the active input row in muted style:
//
//   ┌────────────────────────────────────────────┐
//   │   [1] what is your favourite colour?       │  ← queue row (muted)
//   │   [2] and your second choice?              │  ← queue row (muted)
//   │ > █                                        │  ← active input row
//   │ ◉ claude-sonnet-4 · 0tk · $0.000 · default │
//   │ [2 queued · Tab: add · Enter: steer · ↑: edit]  │
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
// TUI-FIX-T4: Segment model
// -------------------------
//  Buffer is now std::vector<InputSegment> (tagged-union, kind 0=Text/1=Paste).
//  Cursor is SegCursor {seg_idx, byte_off}.
//  Public API (buffer(), cursor(), set_buffer(), clear()) is preserved; these
//  methods flatten/reconstruct the segment vector transparently so existing
//  callers continue to work without changes.
//
// TUI-FIX-T10: Input queue (Codex hybrid)
// ----------------------------------------
//  InputQueue (SoA) holds up to ~5 queued entries as a flat segment array.
//  Tab mid-turn appends current input to queue; Enter mid-turn interrupts
//  and submits combined message.  cursor_idx_ (-1 = input row, 0..N-1 =
//  queue row) controls which row receives edits.
//  Rendering happens inside OnRender (NOT a new FTXUI Component).
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
#include <batbox/tui/InputSegment.hpp>
#include <batbox/tui/ThemeApply.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <cstddef>

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
// InputQueue — SoA flat segment store for queued follow-up messages.
//
// TUI-FIX-T10: Struct-of-arrays layout.  Entry i occupies:
//   all_segments[entry_starts[i] .. entry_starts[i] + entry_lens[i])
//
// Bounded to kMaxQueueDepth entries (~5).  Append is O(1) amortised.
// Deletion (clear all) zeroes starts/lens and clears all_segments.
// =============================================================================
struct InputQueue {
    static constexpr std::size_t kMaxQueueDepth = 5;

    std::vector<std::size_t>  entry_starts;   ///< index into all_segments[]
    std::vector<std::size_t>  entry_lens;     ///< segment count per entry
    std::vector<InputSegment> all_segments;   ///< flat; all entries concatenated

    /// Number of queued entries.
    [[nodiscard]] std::size_t depth() const noexcept { return entry_starts.size(); }

    /// True when the queue holds no entries.
    [[nodiscard]] bool empty() const noexcept { return entry_starts.empty(); }

    /// Append segments from [begin, end) as a new queue entry.
    void append(const std::vector<InputSegment>& segs) {
        entry_starts.push_back(all_segments.size());
        entry_lens.push_back(segs.size());
        for (const auto& s : segs) all_segments.push_back(s);
    }

    /// Flatten entry at index i into a single string (paste bodies expanded).
    [[nodiscard]] std::string flatten_entry(std::size_t i) const {
        if (i >= entry_starts.size()) return {};
        std::string result;
        std::size_t start = entry_starts[i];
        std::size_t len   = entry_lens[i];
        for (std::size_t j = start; j < start + len && j < all_segments.size(); ++j) {
            result += all_segments[j].body;
        }
        return result;
    }

    /// Flatten all entries joined by '\n'.
    [[nodiscard]] std::string flatten_all() const {
        std::string result;
        for (std::size_t i = 0; i < entry_starts.size(); ++i) {
            if (i > 0) result += '\n';
            result += flatten_entry(i);
        }
        return result;
    }

    /// Clear all queued entries.
    void clear() {
        entry_starts.clear();
        entry_lens.clear();
        all_segments.clear();
    }
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

    /// Called when the user presses Esc while a stream is in flight.
    /// Invoked from the FTXUI event-loop thread.  The callback should call
    /// request_stop() on the active CancelSource via a thread-safe mechanism.
    using InterruptCallback = std::function<void()>;

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

    /// Render the input bar (queue rows + prompt row + status row + optional palette overlay).
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

    /// Register a callback invoked when the user presses Esc while a stream
    /// is in flight (stream_active_ == true).
    ///
    /// Called on the FTXUI event-loop thread.  The canonical implementation
    /// captures a shared_ptr<CancelSource> and calls request_stop() on it so
    /// the SSE worker thread detects cancellation at its next poll point.
    ///
    /// When nullptr (default), Esc while streaming is a no-op for the interrupt
    /// path (existing Cancel semantics still apply when stream is inactive).
    ///
    /// Must be called from the UI thread before the event loop starts.
    void set_on_interrupt(InterruptCallback cb);

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

    /// Return a copy of the current input buffer (flattened from all segments).
    [[nodiscard]] std::string buffer() const;

    /// Return the current cursor position as a flat byte offset into the
    /// flattened buffer string.  This preserves backward compatibility with
    /// callers that use cursor() as a byte index.
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

    // ---- TUI-FIX-T4: segment model public helpers ---------------------------

    /// Insert a bracketed-paste body at the current cursor position.
    ///
    /// If body is shorter than kPasteChipMinChars AND contains no newlines,
    /// it is inserted as plain text in the current Text segment (no chip).
    /// Otherwise a PasteSegment chip is created with the next paste id.
    ///
    /// Called from OnEvent when the \e[200~...\e[201~] envelope is complete.
    void insert_paste(std::string body);

    /// Flatten all segments into a single string (paste bodies expanded).
    /// This is what gets submitted to on_submit_.
    [[nodiscard]] std::string flatten_for_submit() const;

    // ---- TUI-FIX-T10: input queue public accessors --------------------------

    /// Read-only access to the input queue (for testing).
    [[nodiscard]] const InputQueue& input_queue() const noexcept { return queue_; }

    /// Clear the input queue (e.g. after a successful combined submit).
    void clear_queue();

private:
    // ---- Render helpers -----------------------------------------------------

    /// Render the single prompt row:  ">" prefix + text + cursor (or placeholder).
    ftxui::Element render_prompt_row() const;

    /// Render one queue row (muted condensed line above the active input row).
    /// @param entry_idx  Index into queue_.entry_starts (0 = oldest).
    /// @param selected   True when cursor_idx_ == entry_idx (highlight row).
    ftxui::Element render_queue_row(std::size_t entry_idx, bool selected) const;

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
    ///   queue depth > 0 → right includes "[N queued · Tab: add · Enter: steer · ↑: edit]"
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

    /// Apply a VimAction returned by vim_mode_ to the buffer.
    /// For vim operations, works on the flattened string then re-sets buffer.
    void apply_vim_action(const batbox::repl::VimAction& action);

    /// Insert a single UTF-8 character sequence at the current cursor position
    /// in the current (or a new) Text segment.
    void insert_at_cursor(std::string_view text);

    // ---- TUI-FIX-T4: segment model internals --------------------------------

    /// Return the flat byte offset corresponding to the current SegCursor.
    [[nodiscard]] std::size_t flat_cursor_offset() const noexcept;

    /// Set the SegCursor from a flat byte offset into the flattened buffer.
    void set_seg_cursor_from_flat(std::size_t flat_offset);

    /// Ensure the segment at seg_cursor_.seg_idx is a Text segment.
    /// If the cursor is at a Paste segment, split or insert as needed.
    /// Returns a reference to the Text body at the cursor.
    InputSegment& cursor_text_segment();

    /// Remove empty Text segments and merge adjacent Text segments.
    void normalize_segments();

    /// Build the chip label for a Paste segment.
    [[nodiscard]] static std::string paste_chip_label(const InputSegment& ps);

    /// Returns true when the buffer (all segments combined) is logically empty.
    [[nodiscard]] bool is_empty() const noexcept;

    // ---- TUI-FIX-T10: queue helpers -----------------------------------------

    /// Append the current input row to the queue (if non-empty) and clear input.
    /// Returns true if an entry was actually appended.
    bool queue_append_current();

    /// Build the combined submit string: current + '\n' + joined queue, then
    /// clear both.  Used by Enter-mid-turn and Enter-idle-with-queue paths.
    std::string build_combined_submit();

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
    InterruptCallback     on_interrupt_;  ///< Fired when Esc pressed while stream_active_.

    // ---- TUI-FIX-T4: segment model buffer -----------------------------------
    /// Buffer as a list of segments.  Always contains at least one Text segment
    /// (possibly with an empty body) so cursor operations have a valid target.
    std::vector<InputSegment> segments_;

    /// Cursor position within segments_.
    SegCursor seg_cursor_{0, 0};

    /// Per-session paste counter; incremented each time a Paste chip is created.
    int paste_id_counter_{0};

    /// True while accumulating a bracketed paste sequence (\e[200~...\e[201~).
    bool in_paste_seq_{false};

    /// Accumulated paste bytes (between \e[200~ and \e[201~).
    std::string paste_accumulator_;

    // ---- TUI-FIX-T10: input queue -------------------------------------------
    /// SoA queue of follow-up messages.  Populated by Tab mid-turn.
    InputQueue queue_;

    /// Queue/input selection cursor.
    ///   -1           = input row (default)
    ///   0 .. N-1     = queue row index (for edit via ↑/↓)
    int cursor_idx_{-1};

    // ---- Vim mode state machine ---
    batbox::repl::VimMode vim_mode_;

    // Status line
    StatusLine status_;

    // ---- A3: dirty-string cache for formatted status display ----
    // Rebuilt only when status_strs_dirty_ == true (set by set_usage()).
    // Combined with snprintf format_cost / format_tokens: 1 alloc per turn
    // instead of per render frame.
    mutable std::string cached_token_str_;
    mutable std::string cached_cost_str_;
    mutable bool status_strs_dirty_ = true;

    // Running tool indicator (set during tool dispatch, cleared after).
    // When non-empty, displayed as " · running: <name>" in the status row.
    std::optional<std::string> running_tool_;

    // Thinking indicator (TUI-T15).
    // Set to true when a ThinkingStarted event arrives; cleared on ThinkingStopped.
    // Displayed as " · thinking..." in the status row when running_tool_ is empty.
    bool thinking_{false};

    // --- Splash placeholder (TUI-FLOW-T4) ---
    /// True while the SplashBanner is visible and no text has been typed.
    bool splash_showing_{false};

    // --- TUI-FLOW-T9: contextual placeholder rotation ---
    /// Rotating placeholder templates.  Index 0 is the T4 default fallback.
    std::vector<std::string> placeholder_templates_;

    /// Frame counter incremented on each placeholder render call.
    mutable int placeholder_frame_counter_{0};

    // --- TUI-FLOW-T6: footer hint chips ---
    /// True between UserMessage and StreamDone: shows "esc to interrupt" chip.
    bool stream_active_{false};

    /// Current thinking-effort level shown in the right footer chip.
    std::string effort_level_{"medium"};

    /// Count of currently-failed MCP servers (0 = chip hidden, >0 = chip shown).
    std::atomic<int> mcp_failed_{0};

    // --- Slash palette overlay state ---
    bool                     palette_open_{false};
    std::string              palette_filter_str_;
    std::vector<std::string> palette_all_;
    std::vector<std::string> palette_filtered_;
    int                      palette_selected_{0};

    // --- Autocomplete state ---
    std::vector<std::string> ac_candidates_;
    int                      ac_index_{-1};
    std::string              ac_prefix_;

    // --- TUI-FLOW-T3: perf HUD ---
    bool perf_hud_enabled_{false};

    // --- TUI-PERM-T1: permission gate for Shift+Tab mode cycle ---
    batbox::permissions::PermissionGate* perm_gate_{nullptr};
};

// =============================================================================
// Constants
// =============================================================================

/// Minimum paste body length (in bytes) to create a PasteSegment chip.
/// Pastes below this threshold with no newlines are inserted as plain text.
inline constexpr int kPasteChipMinChars = 200;

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
