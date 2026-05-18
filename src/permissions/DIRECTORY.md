# src/permissions

Permission system implementations: seven-step gate, rule persistence, glob pattern matching, and mode utilities.

## Files

### PermissionGate.cpp
`ask()` implementation: seven-step decision flow under mutex; `is_accept_edits_tool()` and `is_read_only_tool()` static helper tables; auto-persists returned persist_rule via PermissionStore after prompt_fn returns.

### PermissionStore.cpp
`add_allow_rule()`, `add_deny_rule()`, `add_ask_rule()`, `remove_rule()` implementations: each calls `mutate_and_persist()` which reloads from disk, applies lambda, writes back atomically; deduplication check before write.

### PatternMatcher.cpp
`ToolPattern::parse()` implementation: splits rule on '(' to extract tool name glob and optional JSON args glob; `glob_match()`: iterates characters with * wildcard expansion; `matches()`: applies both globs to tool_name and args JSON string.

### PermissionMode.cpp
`to_string()`, `mode_from_string()`, `cycle_next()`, `requires_banner()`, `banner_text()` implementations; static string tables for mode names and banner text.

### PermissionRule.cpp
`kind_to_string()` implementation; PermissionRule comparison operators.

### CMakeLists.txt
Build rules for the permissions static library.
