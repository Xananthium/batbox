// include/batbox/tui/ChatView.hpp
// =============================================================================
// batbox::tui::ChatView — append-only scrollable message-history Component
//
// Design
// ------
// ChatView is an FTXUI ComponentBase subclass that maintains an ordered list
// of completed conversation messages, each rendered via MarkdownRenderer.
// A StreamingMessageView placeholder slot at the bottom is reserved for the
// in-progress assistant turn (managed by the caller via token events).
//
// Scrolling model
// ---------------
// The component tracks a scroll_offset_ (lines from the bottom, 0 = pinned to
// bottom).  When scroll_offset_ == 0, a new append_message() call keeps the
// view pinned.  Once the user presses any scroll key the offset is set > 0 and
// auto-scroll is disabled until the user scrolls back to the bottom (offset 0).
//
// Controls:
//   ArrowUp / k        — scroll up 1 line
//   ArrowDown / j      — scroll down 1 line
//   PageUp             — scroll up half the visible height
//   PageDown           — scroll down half the visible height
//   End / G            — snap back to bottom (re-enable auto-scroll)
//
// Event handling (in OnEvent — priority order)
// --------------------------------------------
//   make_token_event     → accumulates into streaming_buffer_, calls
//                          set_streaming_text(streaming_buffer_), returns true.
//   make_user_message_event → appends a User message to history, returns true.
//   make_stream_done_event  → commits streaming_buffer_ as an Assistant message,
//                             clears streaming_buffer_ + streaming tail, returns true.
//   Events::QuestionShow    → extracts payload, calls question_card_->set_spec(),
//                             sets show_question_card_ = true, returns true.
//   Events::QuestionResolved→ sets show_question_card_ = false, returns true.
//   scroll keys (k/j/G/etc) → handled as before.
//
// Modal z-order (outermost = highest priority)
// --------------------------------------------
//   PermissionCard (top)  > PlanApprovalCard > QuestionCard
//   All three layers are wired in WireTui.cpp via nested dbox + CatchEvent.
//   ChatView owns question_card_ and the show_question_card_ visibility flag
//   only; the actual overlay is composed in wire_tui().
//
// Thread safety
// -------------
// ChatView is NOT thread-safe.  All public methods must be called from the
// FTXUI UI thread.  Background threads deliver content via token Events posted
// through ScreenManager::post_token(); the UI thread calls append() on the
// streaming view before the finalized message arrives here.
//
// Blueprint contract: batbox::tui::ChatView (blueprints table, CPP 1.7)
// TUI-ASKQ-T4: QuestionCard modal integration.
// =============================================================================

#pragma once

#include <batbox/conversation/Message.hpp>
#include <batbox/theme/Theme.hpp>
#include <batbox/tui/MarkdownRender.hpp>
#include <batbox/tui/ThemeApply.hpp>
#include <batbox/tui/QuestionCard.hpp>

#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace batbox::tui {

// Forward declaration to avoid circular include (InputBar includes ChatView via Events.hpp).
class InputBar;

// =============================================================================
// ChatView — append-only scrollable Component for conversation history
// =============================================================================

/// FTXUI ComponentBase that renders an ordered list of past messages.
///
/// Usage:
/// @code
///   auto chat = std::make_shared<batbox::tui::ChatView>(theme);
///   // … add a completed message:
///   chat->append_message(msg);
///   // … token events handled by caller; streaming text goes to a
///   //    StreamingMessageView pinned below the history pane.
/// @endcode
class ChatView : public ftxui::ComponentBase {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    /// Construct a ChatView with the given theme reference.
    ///
    /// @param theme  Active colour palette; must outlive this ChatView.
    explicit ChatView(const batbox::theme::Theme& theme);

    // Non-copyable, non-moveable (holds a ThemeRef).
    ChatView(const ChatView&)            = delete;
    ChatView& operator=(const ChatView&) = delete;
    ChatView(ChatView&&)                 = delete;
    ChatView& operator=(ChatView&&)      = delete;

    ~ChatView() override = default;

    // -------------------------------------------------------------------------
    // Message management
    // -------------------------------------------------------------------------

    /// Append a completed message to the history.
    ///
    /// The message content is rendered immediately via MarkdownRenderer.
    /// If scroll_offset_ == 0 the view stays pinned to the bottom so the new
    /// message is visible without further user action.
    ///
    /// Must be called from the UI thread.
    ///
    /// @param msg  A finalized batbox::conversation::Message.
    void append_message(const batbox::conversation::Message& msg);

    /// Replace the content of the in-progress streaming tail.
    ///
    /// Called on each token event so the streaming assistant turn is visually
    /// distinct at the bottom of the list.  An empty string clears the tail.
    ///
    /// Must be called from the UI thread.
    ///
    /// @param text  Accumulated streaming text so far (full content, not delta).
    void set_streaming_text(std::string_view text);

    /// Clear the streaming tail (called when the assistant turn is finalized
    /// and append_message has been called with the completed message).
    void clear_streaming();

    /// Return the number of finalized messages in the history.
    [[nodiscard]] std::size_t message_count() const noexcept;

    /// Register the function used by the spinner timer thread to post events
    /// back to the FTXUI event loop.  Must be called once after construction
    /// and before the first UserMessage event arrives.
    ///
    /// The function is invoked from a background thread; it must be thread-safe
    /// (ScreenManager::post_event() satisfies this requirement).
    ///
    /// @param fn  Callable with signature void(ftxui::Event).
    void set_screen_post_fn(std::function<void(ftxui::Event)> fn);

    /// True when the live spinner is currently running.
    [[nodiscard]] bool spinner_active() const noexcept { return spinner_active_; }

    /// Elapsed seconds on the current (or most recently completed) turn.
    [[nodiscard]] int spinner_elapsed_s() const noexcept { return spinner_elapsed_s_; }

    /// Streamed token count on the current (or most recently completed) turn.
    [[nodiscard]] int spinner_token_count() const noexcept { return spinner_token_count_; }

    /// The tagline word currently shown in the spinner row (e.g. "frank sinatra…").
    [[nodiscard]] const std::string& spinner_tagline() const noexcept { return spinner_tagline_; }

    /// Return the current scroll offset (0 = pinned to bottom).
    [[nodiscard]] int scroll_offset() const noexcept { return scroll_offset_; }

    /// True when auto-scroll is active (offset == 0).
    [[nodiscard]] bool at_bottom() const noexcept { return scroll_offset_ == 0; }

    // -------------------------------------------------------------------------
    // TUI-ASKQ-T4 — QuestionCard modal integration
    // -------------------------------------------------------------------------

    /// Attach the QuestionCard instance whose visibility this ChatView manages.
    ///
    /// Must be called once after construction (from wire_tui) before the first
    /// QuestionShow event can arrive.  The card itself is rendered by the
    /// overlay layer in WireTui.cpp; ChatView owns the show_question_card_
    /// flag that controls visibility.
    ///
    /// @param qc  Shared pointer to the QuestionCard.  Must remain valid for
    ///            the lifetime of the FTXUI event loop.
    void set_question_card(std::shared_ptr<QuestionCard> qc);

    // -------------------------------------------------------------------------
    // TUI-FLOW-T6 — footer hint chips wiring
    // -------------------------------------------------------------------------

    /// Register the InputBar whose set_stream_active() should be mirrored when
    /// UserMessage and StreamDone events arrive.
    ///
    /// Must be called once from wire_tui() after both ChatView and InputBar have
    /// been constructed, before the first UserMessage event can arrive.
    ///
    /// @param bar  Raw pointer to InputBar (non-owning).  Must remain valid for
    ///             the lifetime of the FTXUI event loop.
    void set_input_bar(InputBar* bar);

    /// True when the QuestionCard should be shown as a modal overlay.
    ///
    /// Read by the WireTui overlay renderer on every frame.  Written by
    /// OnEvent() when QuestionShow / QuestionResolved events arrive.
    [[nodiscard]] bool show_question_card() const noexcept {
        return show_question_card_.load(std::memory_order_acquire);
    }

    // -------------------------------------------------------------------------
    // FTXUI overrides
    // -------------------------------------------------------------------------

    /// Render the scrollable message history.
    ///
    /// Composes finalized message rows plus the streaming tail (if present)
    /// into a vertically-scrolled vbox.  Theme colours applied per role:
    ///   User        → PromptPrefix for the "You: " label, Fg for body
    ///   Assistant   → AccentCyan for the "Batbox: " label, Fg for body
    ///   Tool result → Muted header, CodeBg background for the body
    ///   Tool call   → Muted label, Muted body (collapsed by default)
    ///
    /// @returns An ftxui::Element suitable for embedding in the parent layout.
    ftxui::Element OnRender() override;

    /// Handle token events, user-message events, stream-done events, and
    /// keyboard scroll events.
    ///
    /// Priority order:
    ///   1. make_token_event      — accumulate + update streaming tail
    ///   2. make_user_message_event — append User message to history
    ///   3. make_stream_done_event  — commit streaming buffer, clear tail
    ///   4. Events::QuestionShow    — load spec into question_card_, show overlay
    ///   5. Events::QuestionResolved — hide overlay, invoke callback
    ///   6. ctrl+o (\x0f)         — toggle expand/collapse on most-recent collapsible
    ///                               entry (ToolCardEntry or long user prompt).
    ///                               No-op / falls through when a modal is visible.
    ///   7. Scroll keys (ArrowUp/k, ArrowDown/j, PageUp, PageDown, End/G)
    ///
    /// @param event  Any FTXUI event.
    /// @returns true when the event was consumed; false otherwise.
    bool OnEvent(ftxui::Event event) override;

private:
    // -------------------------------------------------------------------------
    // Internal types
    // -------------------------------------------------------------------------

    /// One rendered entry in the history list.
    struct MessageEntry {
        batbox::conversation::Role role;
        std::string                label;        ///< "You: " / "Batbox: " / etc.
        std::string                raw_content;  ///< Original message content
        ftxui::Element             rendered;     ///< Pre-rendered body element
        bool                       is_tool_call   = false;
        bool                       collapsed      = true;  ///< Tool calls start collapsed
        /// TUI-FLOW-T8: true when a long user prompt (>120 chars) or a
        /// tool-call entry has been expanded by ctrl+o.  For user prompts,
        /// render_entry() consults this flag: false = truncated single line,
        /// true = full content rendered verbatim.  For is_tool_call entries
        /// the collapsed flag governs display; expanded is a parallel field
        /// reserved for future full-body expansion.
        bool                       expanded       = false;
    };

    // -------------------------------------------------------------------------
    // TUI-FLOW-T2 — tool-call card state
    // -------------------------------------------------------------------------

    /// One live/completed tool-call card rendered between the user prompt
    /// and the eventual assistant text reply.
    ///
    /// Cards accumulate via ToolRunning events and transition to complete
    /// on ToolDone.  Multiple concurrent tool calls (same assistant turn)
    /// share ONE card; subsequent dispatch batches each get their own.
    struct ToolCardEntry {
        // ---- Identity ----
        /// Canonical tool name of the first/primary tool (e.g. "Read", "Bash").
        std::string tool_name;
        /// One-line preview arg (path, command, pattern, …).
        std::string args_summary;
        /// Total number of tools in this batch (used for the combined summary).
        int tool_count{1};

        // ---- State ----
        /// True while the tool dispatch is still in flight (before ToolDone).
        bool in_flight{true};
        /// True when the card is expanded (user pressed ctrl+o or equivalent).
        bool expanded{false};

        // ---- Accumulated preview lines (for expanded view) ----
        /// List of arg-summary strings for each tool in this batch.
        std::vector<std::string> preview_lines;
    };

    // -------------------------------------------------------------------------
    // Private helpers
    // -------------------------------------------------------------------------

    /// Render a single MessageEntry into an ftxui::Element for display.
    [[nodiscard]] ftxui::Element render_entry(const MessageEntry& entry) const;

    /// Render the label prefix ("You: ", "Batbox: ", etc.) for a role.
    [[nodiscard]] ftxui::Element render_label(const MessageEntry& entry) const;

    /// Render the body content using MarkdownRenderer.
    [[nodiscard]] ftxui::Element render_body(const MessageEntry& entry) const;

    /// Render the streaming tail element (if streaming_text_ is non-empty).
    [[nodiscard]] ftxui::Element render_streaming() const;

    /// Render the live spinner row (visible while spinner_active_ is true)
    /// or the frozen summary row (visible when spinner was active and is now done).
    [[nodiscard]] ftxui::Element render_spinner_row() const;

    // TUI-FLOW-T2 — tool-card helpers

    /// Map a raw tool name to a past-tense verb (e.g. "Read" → "Read",
    /// "read_file" → "Read", "bash" → "Running").
    [[nodiscard]] static std::string verb_past(const std::string& tool_name);

    /// Map a raw tool name to a gerund verb (e.g. "Read" → "Reading",
    /// "bash" → "Running", "grep" → "Searching").
    [[nodiscard]] static std::string verb_gerund(const std::string& tool_name);

    /// Build the one-line summary text for a tool card.
    /// @param card   The card entry to summarise.
    /// @param width  Terminal width (for truncation of the affordance text).
    [[nodiscard]] std::string tool_card_summary(const ToolCardEntry& card,
                                                 int width) const;

    /// Render a single ToolCardEntry into an FTXUI Element.
    [[nodiscard]] ftxui::Element render_tool_card(const ToolCardEntry& card,
                                                   int term_width) const;

    /// Render all tool cards as a vbox column.
    [[nodiscard]] ftxui::Element render_tool_cards(int term_width) const;

    /// Start the 1Hz spinner timer thread.
    /// Safe to call multiple times — no-op if spinner is already running.
    void start_spinner_timer();

    /// Stop the 1Hz spinner timer thread and join it.
    /// Called from OnEvent (UI thread) when StreamDone arrives.
    void stop_spinner_timer();

    /// Clamp scroll_offset_ to valid range [0, max_scroll].
    void clamp_scroll(int visible_height);

    /// Build a label string for the given role.
    static std::string make_label(batbox::conversation::Role role,
                                  const batbox::conversation::Message& msg);

    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------

    const batbox::theme::Theme& theme_;

    /// Finalized message entries.
    std::vector<MessageEntry> entries_;

    /// Streaming assistant tail (empty when no stream is active).
    std::string streaming_text_;

    /// Accumulation buffer for in-progress assistant streaming content.
    /// Token events append to this; stream_done commits it as a message and
    /// clears it.  Separate from streaming_text_ (which is the display string
    /// set via set_streaming_text) so the accumulator survives set_streaming_text
    /// calls without aliasing issues.
    std::string streaming_buffer_;

    /// Scroll position: number of content rows scrolled up from the bottom.
    /// 0 = pinned to bottom (auto-scroll enabled).
    int scroll_offset_ = 0;

    /// Cached height from the last render pass (used for PgUp/PgDn).
    int last_visible_height_ = 24;

    // -------------------------------------------------------------------------
    // TUI-FLOW-T2 — tool-call card state
    // -------------------------------------------------------------------------

    /// Ordered list of tool-call cards rendered between the user prompt and
    /// the assistant reply.  Each ToolRunning event appends or updates the
    /// current in-flight card.  Each ToolDone event marks the current card done.
    std::vector<ToolCardEntry> tool_cards_;

    /// Cached terminal width from the last OnRender() call.
    /// Used by tool-card truncation logic.
    int term_width_{80};

    // -------------------------------------------------------------------------
    // TUI-FLOW-T1 — live spinner state
    // -------------------------------------------------------------------------

    /// Function posted to by the timer thread (ScreenManager::post_event).
    /// Set via set_screen_post_fn() from App.cpp / WireTui.cpp.
    std::function<void(ftxui::Event)> screen_post_fn_;

    /// True while a turn is in flight (UserMessage..StreamDone).
    bool spinner_active_ = false;

    /// True once the first Token event has been received in this turn.
    bool spinner_tokens_started_ = false;

    /// True while a ToolRunning event has been received but ToolDone has not.
    bool spinner_tool_in_flight_ = false;

    /// Wall-clock seconds elapsed since the turn started.
    /// Incremented in OnEvent when a SpinnerTick event is processed.
    int spinner_elapsed_s_ = 0;

    /// Frozen elapsed seconds captured when StreamDone fires (summary row).
    int spinner_frozen_elapsed_s_ = 0;

    /// Running streamed-token count for this turn.
    int spinner_token_count_ = 0;

    /// Frozen token count captured when StreamDone fires (summary row).
    int spinner_frozen_token_count_ = 0;

    /// True when at least one turn has completed and we should show the
    /// frozen summary row.
    bool spinner_show_summary_ = false;

    /// Tagline word shown in the spinner row (e.g. "frank sinatra…").
    /// Picked from kTaglines at turn start.
    std::string spinner_tagline_;

    /// 1Hz background timer thread.
    std::thread spinner_thread_;

    /// Signals the timer thread to stop (set before join()).
    std::atomic<bool> spinner_stop_flag_{false};

    // -------------------------------------------------------------------------
    // TUI-FLOW-T3 — stream-to-paint latency
    // -------------------------------------------------------------------------

    /// Time at which the most recent token event arrived in OnEvent().
    /// Set when a non-empty token arrives; cleared after OnRender() reads it.
    /// Default-constructed time_point has time_since_epoch() == 0 (sentinel).
    std::chrono::steady_clock::time_point pending_token_post_time_{};

    // TUI-FIX-T9: EMA state for frame_ms and stream_to_paint_ms.
    // frame_ema_ms_: exponential moving average of per-frame render duration.
    //   alpha = 1/8 → new_ema = old_ema + (sample - old_ema) / 8
    // paint_ema_ms_: exponential moving average of stream-to-paint latency.
    //   alpha = 1/16 → new_ema = old_ema + (sample - old_ema) / 16
    // Both are stored as integer milliseconds (truncated after EMA update).
    int frame_ema_ms_{0};
    int paint_ema_ms_{0};

    // -------------------------------------------------------------------------
    // TUI-ASKQ-T4 — QuestionCard modal integration
    // -------------------------------------------------------------------------

    /// The QuestionCard modal instance.  Set once via set_question_card().
    /// Null until wire_tui() calls set_question_card().
    std::shared_ptr<QuestionCard> question_card_;

    /// True when the QuestionCard overlay should be shown.
    ///
    /// Written by OnEvent (UI thread) on QuestionShow / QuestionResolved.
    /// Read by the WireTui overlay renderer (also UI thread — no race).
    /// Uses atomic storage for symmetry with PermissionCard::pending() and to
    /// allow safe reads from the renderer lambda without locking.
    std::atomic<bool> show_question_card_{false};

    // -------------------------------------------------------------------------
    // TUI-FLOW-T6 — footer hint chips wiring
    // -------------------------------------------------------------------------

    /// Non-owning pointer to the InputBar; null until set_input_bar() is called.
    /// Used to forward set_stream_active(true/false) on UserMessage/StreamDone.
    InputBar* input_bar_{nullptr};
};


} // namespace batbox::tui
