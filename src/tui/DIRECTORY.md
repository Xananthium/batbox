# src/tui

FTXUI terminal UI component implementations: chat view, input bar, overlays, markdown renderer, syntax highlighting, and layout infrastructure.

## Files

### AccountLabel.cpp
`resolve_account_label()` implementation: returns configured account when non-empty; falls back to getpwuid_r($UID) for username and gethostname() for hostname; handles both failure modes.

### Changelog.cpp
Parses the embedded changelog data into ChangelogEntry structs; renders the changelog dialog component; compares versions against StateStore::read_last_seen_changelog_version() to suppress already-seen entries.

### ChatView.cpp
FTXUI ComponentBase subclass: maintains ordered list of rendered message elements; append_message() converts Message to FTXUI Elements via MarkdownRenderer; handles spinner state with 10 Hz tick callbacks; scrolls to bottom on new messages.

### DemonPanel.cpp
DemonPanel FTXUI component: sliding collapsible side panel; DemonRateLimiter integration; rotating tagline tick at configurable interval; kDemonTaglines[] constexpr array of Miss Kittin-era electroclash phrases.

### DiffCard.cpp
Blocking diff review modal: parses unified diff payload; renders added/removed/context rows with diff_add/diff_remove theme colors; await() blocks the calling thread via condition variable until the user accepts, rejects, or edits.

### Events.cpp
Event payload serialisation for PostEvent: encodes/decodes TokenPayload, StatusUpdatePayload, AgentsDirtyPayload, ModalShowPayload, ModalHidePayload as FTXUI custom events.

### InputBar.cpp
FTXUI ComponentBase subclass: multi-line input with segment-aware paste handling; Autocomplete popup overlay; history navigation via History/HistorySearch; VimMode integration; StatusLine rendering; submit callback on Ctrl+Enter.

### MarkdownRender.cpp
`MarkdownRenderer` implementation: block-level incremental parser; handles ATX headings, fenced code blocks, unordered/ordered lists, blockquotes, horizontal rules, and inline bold/italic/code; caches completed blocks; calls SyntaxHighlight for code fence coloring.

### ModalPicker.cpp
FTXUI ComponentBase: floating bordered modal with a scrollable option list; fires selection callback on Enter; fires cancel callback on Escape; keyboard navigation with Up/Down arrows.

### PermissionBanner.cpp
FTXUI ComponentBase: top status bar rendering the active PermissionMode; render_nuclear_banner() returns ☢️ element; render_mode_label() returns PLAN/ACCEPT-EDITS colored label; cycles modes on Tab keypress via PermissionGate::set_mode().

### PermissionCard.cpp
Blocking permission modal: renders tool_name, args_preview, and four-button choice (Allow/Deny/Always Allow/Always Deny); await() blocks via condition variable; returns Decision to the dispatch loop.

### PlanApprovalCard.cpp
Blocking plan approval modal: renders full plan text with scrolling; three-button choice (Approve/Reject/Edit); await() blocks until the user responds; returns PlanApprovalResult with Kind and optional edited text.

### QuestionCard.cpp
Blocking multi-choice question modal for AskUserQuestionTool: renders question text and labeled option rows; supports multi-select (checkbox) and single-select (radio) modes; await() blocks until submitted or cancelled.

### RenderStats.cpp
`RenderStats::global()` singleton implementation; `record()` pushes current timestamp atomically; `drain()` swaps the buffer under a mutex; is_enabled() checks BATBOX_PERF at first call.

### Screen.cpp
`ScreenManager` implementation: wraps ftxui::ScreenInteractive::Fullscreen(); `quit_closure()` returns Exit() closure; manages start/stop lifecycle for clean shutdown.

### Splash.cpp
`Splash` component: animates bat ASCII art; rotates taglines from splash_taglines.hpp; collapses to compact header on keypress or timeout. `SplashBanner` component: compact header showing bat glyph, version, model name, and account label.

### splash_taglines.hpp
Constexpr array of rotating taglines shown during startup (internal header, not part of the public API surface).

### StreamingMessageView.cpp
FTXUI ComponentBase: in-flight streaming message renderer; append_token() adds text and schedules repaint; collapses to static ChatView entry on stream end; renders with a trailing cursor indicator during streaming.

### SubAgentPanel.cpp
`TuiAgentTickerThread` implementation: polls AgentEventQueue at 10 Hz; posts AgentsDirty events via screen.PostEvent(). `SubAgentPanel` implementation: renders collapsible agent list; render_agent_row() formats status_glyph + name + step + token count; status_glyph() maps SubAgentStatus to Unicode glyphs.

### SyntaxHighlight.cpp
Dispatches to tree-sitter grammar libraries (when BATBOX_SYNTAX=ON) or manual_lexers fallback; returns a vector of (offset, length, color) highlight spans consumed by MarkdownRenderer for code fence coloring.

### ThemeApply.cpp
`color_for(theme, role) -> ftxui::Color` implementation: switch on ThemeRole enum; returns the corresponding field from the Theme struct.

## Subdirectories

### manual_lexers/
Hand-written fallback syntax lexers for C++, Python, JavaScript, Go, and Rust used when tree-sitter is unavailable.
