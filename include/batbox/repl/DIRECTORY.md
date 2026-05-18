# include/batbox/repl

REPL subsystem headers: autocomplete engine, command context, input history, fuzzy history search, keybindings, the main Repl dispatch loop, and vim mode state machine.

## Files

### Autocomplete.hpp
Context-aware multi-source completion engine for the InputBar.

- `Autocomplete::Autocomplete(slash_registry, plugin_registry, cwd_fn, extra_dirs)` — constructs; registers non-owning references to live registries
- `Autocomplete::complete(prefix, ctx) -> vector<string>` — dispatches to slash, at-mention, bash, or filesystem source; returns up to 20 ranked plain-string candidates
- `Autocomplete::complete_candidates(prefix, ctx) -> vector<Candidate>` — same as complete() but returns Candidate structs with description and score fields for TUI popup rendering
- `Autocomplete::set_extra_dirs(dirs)` — updates the list of extra filesystem search directories
- `detect_context(line) -> ContextDetectResult` — free function; inspects input line up to cursor; returns AutocompleteContext and prefix fragment

### CommandContext.hpp
Lightweight context bag passed to every ISlashCommand::execute() call.

- `ConversationHandle::reset_messages()` — pure virtual; discards all conversation messages; implemented by the live App::Conversation
- `ConversationHandle::inject_user_message(text)` — pure virtual; enqueues text as the next user turn without blocking
- `ConversationHandle::last_assistant_message(n=1) -> string` — returns the Nth-from-last assistant message; used by /copy
- `ConversationHandle::get_session_id() -> string` — returns active session UUID; empty when no session active
- `ConversationHandle::get_session_file_path() -> fs::path` — returns absolute path to the session JSON file
- `ConversationHandle::get_turn_count() -> size_t` — returns number of completed conversational turns
- `ConversationHandle::get_model_name() -> string` — returns model string from session start
- `ConversationHandle::get_messages_json() -> Json` — returns full message history as a JSON array
- `ConversationHandle::set_messages_json(messages)` — replaces the live message list; used by /resume and /compact

### History.hpp
Ring-buffer input history with optional on-disk persistence.

- `History::push(line)` — appends line; ignores blank/whitespace-only and consecutive duplicates; evicts oldest when at cap
- `History::at(age) -> optional<string>` — returns entry at age offset from most recent (0=newest); nullopt when age >= size
- `History::previous() -> optional<string>` — moves cursor one step older; returns entry or nullopt at boundary
- `History::next() -> optional<string>` — moves cursor one step newer; returns entry or nullopt when past end
- `History::reset_cursor()` — resets cursor to past-the-end; call after push or on Escape
- `History::size() -> size_t` — count of stored entries
- `History::cap() -> size_t` — maximum entries before eviction
- `History::clear()` — removes all in-memory entries; does not truncate disk file
- `History::save()` — writes entries to disk atomically via tmp+rename
- `History::load()` — reads entries from disk; replaces in-memory content; no-op when file absent

### HistorySearch.hpp
Ctrl+R fuzzy history search with recency-weighted scoring.

- `HistorySearch::filter_matches(query) -> vector<MatchResult>&` — scans all history entries; scores each by quality*recency; returns ranked vector; resets cycling index
- `HistorySearch::next_match() -> optional<string>` — advances cycling index; wraps around; returns text of selected match
- `HistorySearch::selected() -> optional<string>` — returns text at current cycling index; nullopt when no matches
- `HistorySearch::reset()` — clears match list and sets active() to false
- `HistorySearch::active() -> bool` — true after filter_matches() without subsequent reset()
- `HistorySearch::matches() -> vector<MatchResult>&` — read-only view of current match list

### Keybindings.hpp
REPL action-to-key map with ftxui::Event dispatch.

- `Keybindings::Keybindings()` — constructs with built-in defaults; no I/O
- `Keybindings::default_keybindings() -> unordered_map<ReplAction, string>` — static; returns the built-in action→descriptor snapshot
- `Keybindings::apply_override(map)` — overlays user-supplied action→descriptor map; parses descriptors to ftxui::Events; logs WARN on conflicts
- `Keybindings::event_to_action(ev) -> ReplAction` — maps an ftxui::Event to a ReplAction in O(1); returns ReplAction::None on no match
- `Keybindings::key_for(action) -> optional<string>` — returns current key-descriptor for action; nullopt when unbound
- `Keybindings::descriptor_map() -> unordered_map<ReplAction, string>` — returns snapshot of current action→descriptor map

### Repl.hpp
Main REPL input loop: classifies input by prefix and dispatches to conversation, slash commands, bash, or notes.

- `Repl::Repl(conversation, registry, bash_tool, history, autocomplete, cwd, ctx_extras)` — constructs with injected non-owning references
- `Repl::handle_input(line)` — classifies line by first char ('/'→slash, '!'→bash, '#'→note, '@'→mention, else→chat); handles backslash-continuation multi-line; pushes to history after dispatch
- `Repl::feed_line(line)` — headless/test API; delegates to handle_input()
- `Repl::cancel()` — fires the active CancelSource from any thread; aborts in-flight inference or bash
- `Repl::request_exit()` — sets exit flag; exit_requested() returns true
- `Repl::exit_requested() -> bool` — returns true after request_exit() or /exit
- `Repl::on_submit_callback() -> function<void(string)>` — returns an InputBar submit callback wrapping handle_input()
- `Repl::set_output_stream(os)` — overrides command output stream (default: stdout)
- `Repl::set_input_stream(is)` — overrides input stream for confirmations (default: stdin)

### VimMode.hpp
Vim-mode state machine for the REPL input buffer.

- `VimMode::set_enabled(enabled)` — enables or disables vim mode; when disabled process_key returns Passthrough
- `VimMode::is_enabled() -> bool` — returns current enabled state
- `VimMode::toggle()` — flips enabled state; called by /vim slash command
- `VimMode::mode() -> VimModeState` — returns current mode (Insert/Normal/Visual)
- `VimMode::mode_indicator() -> string` — returns "-- INSERT --", "-- NORMAL --", etc. for the status line
- `VimMode::process_key(key, buf, cursor) -> VimAction` — primary entry point; returns VimAction describing what the caller must apply to its buffer (InsertChar, DeleteRange, MoveCursor, SetBuffer, ChangeMode, SendLine, etc.)
- `VimMode::handle_key(key, buf, cursor) -> VimAction` — blueprint contract alias; delegates to process_key with mutable reference overload
- `VimMode::reset()` — resets to Insert mode and clears pending operator state
