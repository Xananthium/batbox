# tests/tui-smoke

Tmux-based TUI smoke tests: drive the live terminal UI offline against local mock servers.

## Files

### CMakeLists.txt
Registers tui-smoke test targets with CTest label "tui-smoke"; builds only when BATBOX_TUI_SMOKE=ON.
