# include/batbox/tui

FTXUI terminal UI component headers: chat view, input bar, permission/plan/question cards, syntax highlighting, theme application, and layout infrastructure.

## Files

### Changelog.hpp
Changelog dialog component showing release notes to the user.

- `ChangelogEntry` — struct: version, date, headline, bullet_points; parsed from the embedded changelog data at startup

### ChatView.hpp
Scrollable chat message list component.

- `ChatView::message_count() -> size_t` — returns count of rendered message rows
- `ChatView::spinner_active() -> bool` — returns true when the inference spinner is showing
- `ChatView::spinner_elapsed_s() -> int` — returns seconds elapsed since the spinner started
- `ChatView::spinner_token_count() -> int` — returns tokens received so far during streaming

### DemonPanel.hpp
Sliding panel showing the Demon advisor agent comments and taglines.

- `DemonPanel::visible() -> bool` — returns true when the panel is expanded
- `DemonPanel::get_demon_comment() -> string` — returns the current advisor comment text
- `DemonPanel::current_tagline_idx() -> size_t` — returns the index of the currently displayed rotating tagline

### DiffCard.hpp
Floating diff review card that blocks the TUI until the user accepts or rejects a proposed file change.

- `DiffCard::await(payload) -> Decision` — renders the diff; blocks until user presses accept/reject/edit; returns Decision::Allow or Decision::Deny
- `DiffCard::scroll_offset() -> int` — returns current vertical scroll position
- `DiffCard::row_count() -> int` — returns total number of diff rows

### Events.hpp
Custom FTXUI event payloads for cross-thread TUI communication.

- `Events` — struct of static ftxui::Event constants: token, status_update, agents_dirty, modal_show, modal_hide; sent via ScreenInteractive::PostEvent
- `TokenPayload` — carries partial token text and agent_id for streaming display
- `StatusUpdatePayload` — carries sidecar state, MCP server counts, model name for the status line
- `AgentsDirtyPayload` — signals the SubAgentPanel to refresh its snapshot
- `ModalShowPayload` — carries modal type and question spec for QuestionCard display
- `ModalHidePayload` — signals QuestionCard to dismiss

### InputBar.hpp
Multi-line input component with autocomplete popup, history navigation, and segment-aware paste.

- `StatusLine` — struct carrying vim mode indicator, permission mode label, and model name for the input bar bottom row
- `InputQueue::depth() -> size_t` — returns count of pending input entries
- `InputQueue::empty() -> bool` — returns true when queue is empty
- `InputQueue::flatten_entry(i) -> string` — reconstructs the i-th entry as a plain string by concatenating all its segments

### InputSegment.hpp
Typed input segment discriminated union for paste-vs-text handling.

- `TextSegment` — variant: characters typed normally; rendered inline
- `PasteSegment` — variant: content pasted via clipboard; shown with paste indicator in the input bar
- `SegCursor` — cursor position tracking struct used by InputBar's segment vector

### MarkdownRender.hpp
Incremental Markdown-to-FTXUI-Element renderer with block-level caching.

- `MarkdownRenderer::MarkdownRenderer(theme)` — constructs with a ThemeRef for colour roles
- `MarkdownRenderer::cached_block_count() -> size_t` — returns count of cached rendered blocks
- `MarkdownRenderer::in_code_fence() -> bool` — returns true when the renderer is inside an open fenced code block (for streaming partial renders)

### ModalPicker.hpp
Floating bordered modal for multi-option selection (used by PermissionCard and QuestionCard).

- `ModalPicker` — FTXUI ComponentBase; renders a bordered modal with a list of options; fires callback on selection or cancellation

### PermissionBanner.hpp
Single-line top banner showing the active permission mode.

- `PermissionBanner::current_mode() -> PermissionMode` — returns the mode the banner is currently displaying
- `PermissionBanner::render_nuclear_banner() -> Element` — renders the ☢️ NUCLEAR MODE warning element
- `PermissionBanner::render_mode_label() -> Element` — renders the mode name label (PLAN / ACCEPT-EDITS)
- `PermissionBanner::render_confirm_prompt() -> Element` — renders the "press Tab to cycle mode" hint

### PermissionCard.hpp
Floating tool-permission modal that prompts the user to allow/deny/always-allow/always-deny a tool invocation.

- `PermissionCard::tool_name() -> string&` — returns name of the tool awaiting permission
- `PermissionCard::args_preview() -> string&` — returns the truncated JSON args preview shown in the card
- `PermissionCard::pending() -> bool` — returns true when a permission decision is being awaited

### PlanApprovalCard.hpp
Floating modal for the user to approve, reject, or edit a proposed plan.

- `PlanApprovalCard::pending() -> bool` — returns true when awaiting a plan approval decision
- `PlanApprovalCard::plan_text() -> string` — returns the plan text currently displayed in the card

### QuestionCard.hpp
Floating question card for AskUserQuestionTool multiple-choice prompts.

- `QuestionCard::is_visible() -> bool` — returns true when a question is being displayed
- `QuestionCard::cursor_index() -> int` — returns the currently focused option index
- `QuestionCard::checked() -> vector<bool>&` — returns the selection state of each option (for multi-select)
- `QuestionCard::total_rows() -> int` — returns total option row count

### RenderStats.hpp
Frame render timestamp accumulator for performance diagnostics.

- `RenderStats::global() -> RenderStats&` — returns the process-global singleton
- `RenderStats::drain() -> vector<TimePoint>` — atomically drains and returns all recorded frame timestamps
- `RenderStats::count() -> size_t` — returns count of timestamps currently in the buffer
- `RenderStats::is_enabled() -> bool` — static; returns true when BATBOX_PERF env var is set

### Screen.hpp
FTXUI ScreenInteractive wrapper with start/stop lifecycle management.

- `ScreenManager::quit_closure() -> Closure` — returns an ftxui::Closure that calls screen_interactive().Exit() for clean shutdown
- `ScreenManager::screen_interactive() -> ScreenInteractive&` — returns reference to the underlying FTXUI screen

### Splash.hpp
Startup splash banner showing bat ASCII art, version, account label, and rotating taglines.

- `Splash` — FTXUI ComponentBase; full-screen animated intro; calls on_done callback after display duration or on any keypress
- `SplashBanner::is_collapsed() -> bool` — returns true when the splash has been dismissed and collapsed to the compact header mode

### StreamingMessageView.hpp
In-flight streaming message component showing token-by-token output during inference.

- `StreamingMessageView::current_text() -> string` — returns the full accumulated text received so far in this streaming turn
- `StreamingMessageView::is_streaming() -> bool` — returns true while a stream is in progress

### SubAgentPanel.hpp
Collapsible side panel showing all live sub-agents with status, step, and token count.

- `TuiAgentTickerThread` — background jthread that polls AgentEventQueue and posts AgentsDirty events to the FTXUI screen at 10 Hz
- `SubAgentPanel::render_agent_row(snapshot) -> Element` — renders one agent row with status glyph, name, step text, and token count
- `SubAgentPanel::status_glyph(status) -> string` — static; returns "⚙" / "✓" / "✗" / "⊘" / "⏳" for queued/done/failed/cancelled/running

### SyntaxHighlight.hpp
Token-based syntax highlighting for fenced code blocks using tree-sitter or manual fallback lexers.

- Provides language-specific highlight functions consumed by MarkdownRenderer for code fence coloring; no public API beyond the MarkdownRenderer integration point

### ThemeApply.hpp
Mapping from ThemeRole enum to concrete ftxui::Color from a Theme struct.

- `ThemeRole` — enum: Background, Foreground, AccentMagenta, AccentCyan, Muted, Success, Error, DiffAddFg, DiffAddBg, DiffRemoveFg, DiffRemoveBg, PromptPrefix, CodeBg
