# src/repl

REPL subsystem implementations: autocomplete, history, history search, keybindings, the main dispatch loop, and vim mode.

## Files

### Autocomplete.cpp
`complete()` and `complete_candidates()` implementations: dispatches to source_slash_commands(), source_at_mentions(), and source_filesystem(); `score_match()` exact-substring and fuzzy gap-penalty scoring; sorts by descending score; truncates to kMaxResults=20. `detect_context()` implementation: scans input line for '/', '@', '!' sigils.

### History.cpp
`push()`, `at()`, `previous()`, `next()`, `save()`, `load()` implementations; deque-based ring buffer; cap eviction on push; atomic cursor management; atomic tmp+rename write.

### HistorySearch.cpp
`filter_matches()` implementation: iterates deque; scores each entry as quality*recency_weight; sorts descending; resets cycle index. `next_match()` wraps index with modulo. `score_match()` static: substring returns 1.0; fuzzy gap-penalty formula for partial matches.

### Keybindings.cpp
`apply_override()` implementation: maps action name strings to ReplAction enum; calls parse_descriptor() for each; rebuilds event_map_ after all overrides applied; logs conflicts. `parse_descriptor()` implementation: splits on '+' for modifiers; maps base key strings to ftxui::Event values; supports both kitty CSI-u and traditional encodings.

### Repl.cpp
`handle_input()` implementation: prefix dispatch to dispatch_slash/bash/note/mention/chat; multi-line accumulation buffer; calls history.push() after dispatch. `cancel()`: calls cancel_src_->request_stop(). `on_submit_callback()`: returns lambda wrapping handle_input().

### VimMode.cpp
`process_key()` implementation: normal_key/insert_key/visual_key dispatch; resolve_motion() for h/j/k/l/w/b/e/0/$/^; word_forward_start/end/backward implementations; inner_word/a_word/inner_quoted/outer_quoted text-object helpers; toggle_case for ~; pending operator accumulation for d/c/y; pending count digit accumulation for numeric prefixes.

### CMakeLists.txt
Build rules for the repl static library.
