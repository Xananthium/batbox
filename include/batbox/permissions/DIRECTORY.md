# include/batbox/permissions

Permission system headers: four-mode gate, rule store, pattern matcher, and rule/mode types.

## Files

### PermissionGate.hpp
Central tool-dispatch permission gate implementing the seven-step decision flow.

- `Decision::allow() -> Decision` — static; constructs a one-shot Allow decision
- `Decision::deny() -> Decision` — static; constructs a one-shot Deny decision
- `Decision::deny_with_reason(msg) -> Decision` — static; constructs a Deny with human-readable reason (used by Plan mode)
- `Decision::allow_with_rule(pattern) -> Decision` — static; constructs an Allow that also persists a new allow rule
- `Decision::deny_with_rule(pattern) -> Decision` — static; constructs a Deny that also persists a new deny rule
- `PermissionGate::PermissionGate(store, mode, prompt_fn)` — constructs with a shared PermissionStore, initial PermissionMode, and TUI/test prompt callback
- `PermissionGate::ask(tool_name, args, ctx) -> Decision` — evaluates the seven-step decision flow: Nuclear→Allow, Plan+write→Deny, AcceptEdits+edit→Allow, deny rules, allow rules, AUTO_APPROVE_READS, prompt_fn
- `PermissionGate::set_mode(mode)` — updates the permission mode atomically under mutex
- `PermissionGate::current_mode() -> PermissionMode` — returns current mode atomically under mutex
- `PermissionGate::store() -> PermissionStore&` — returns reference to the underlying rule store

### PermissionMode.hpp
Permission mode enum and string conversion utilities.

- `to_string(mode) -> string_view` — maps PermissionMode to "default"/"plan"/"accept-edits"/"nuclear"
- `mode_from_string(s) -> PermissionMode` — parses mode name string; returns Default on unknown
- `cycle_next(mode) -> PermissionMode` — returns the next mode in the cycling order (Default→Plan→AcceptEdits→Nuclear→Default)
- `requires_banner(mode) -> bool` — returns true for modes that display a status banner (Plan, AcceptEdits, Nuclear)
- `banner_text(mode) -> string_view` — returns the one-line banner label for the given mode

### PermissionStore.hpp
Persistent allow/deny/ask rule store backed by settings.json.

- `PermissionStore::PermissionStore(settings_path)` — constructs and immediately loads rules from settings.json; empty store on missing or malformed file
- `PermissionStore::default_path() -> fs::path` — returns batbox::paths::config_dir() / "settings.json"
- `PermissionStore::allow_rules() -> vector<string>&` — returns raw patterns from permissions.allow
- `PermissionStore::deny_rules() -> vector<string>&` — returns raw patterns from permissions.deny
- `PermissionStore::ask_rules() -> vector<string>&` — returns raw patterns from permissions.ask
- `PermissionStore::rules() -> vector<PermissionRule>` — returns all three lists merged into typed PermissionRule objects (allow→deny→ask order)
- `PermissionStore::last_load_error() -> string&` — returns the error message from the last failed load; empty string on success
- `PermissionStore::add_allow_rule(pattern) -> Result<void>` — adds pattern to allow list; no-op if already present; persists atomically
- `PermissionStore::add_deny_rule(pattern) -> Result<void>` — adds pattern to deny list; persists atomically
- `PermissionStore::add_ask_rule(pattern) -> Result<void>` — adds pattern to ask list; persists atomically
- `PermissionStore::remove_rule(pattern) -> Result<void>` — removes pattern from all three lists; no-op if absent; persists only when a removal occurred

### PatternMatcher.hpp
Glob-style tool permission pattern matching.

- `ToolPattern::parse(rule) -> Result<ToolPattern, string>` — parses a rule string like "Bash(*)" into a ToolPattern; returns Err on invalid syntax
- `glob_match(pattern, text) -> bool` — matches text against a glob pattern (* = any chars, ? = one char); case-sensitive
- `matches(rule, tool_name, args) -> bool` — parses rule and tests tool_name and args JSON against it; returns false on malformed rules
- `parse_pattern_list(raw_rules) -> vector<ToolPattern>` — parses a list of raw rule strings; skips malformed entries with WARN log

### PermissionRule.hpp
Typed rule struct with Kind discriminant.

- `PermissionRule::kind_to_string(kind) -> string_view` — maps PermissionRule::Kind::Allow/Deny/Ask to "allow"/"deny"/"ask"
