# src/util

Small utility implementations: account label resolution and editor launching.

## Files

### EditorLaunch.cpp
`resolve_editor()` implementation: reads $EDITOR; rejects "vi"/"vim" basename; calls binary_accessible() for "nano" then "pico"; falls back to "vi". `binary_accessible()` implementation: splits $PATH on ':'; calls access(X_OK) on each entry. `edit_string_in_editor()` implementation: mkstemp temp file; suspends FTXUI via screen->WithRestoredIO() or runs via std::system(); reads back edited content; unlinks temp file.
