# include/batbox/util

Small utility helpers: account label resolver and editor launch utilities.

## Files

### AccountLabel.hpp
Resolves a display account label for the splash banner.

- `resolve_account_label(configured_account) -> string` — returns configured_account when non-empty; otherwise builds "$USER@<hostname>" using getpwuid_r and gethostname; falls back to "localhost" on hostname failure

### EditorLaunch.hpp
Editor binary resolution and file-in-editor launch helpers.

- `resolve_editor() -> string` — returns the editor binary to exec: $EDITOR (unless "vi"/"vim"), else "nano", "pico", or "vi" in fallback order; uses access(2) for PATH scanning
- `binary_accessible(binary) -> bool` — returns true when binary is found as an executable in any $PATH directory
- `edit_string_in_editor(text, screen) -> string` — writes text to mkstemp temp file; suspends FTXUI via screen->WithRestoredIO() or execs directly; waits for exit; reads back edited content; returns original text on any error
