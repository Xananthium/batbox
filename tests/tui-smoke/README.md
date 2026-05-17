# BatBox TUI Smoke Tests

Tmux-based end-to-end smoke tests for the BatBox terminal UI.
These tests drive the live binary through a real pty and assert screen content.

## Quick Start

### 1. Install tmux (required)

```bash
brew install tmux
tmux -V   # should print >= 3.4
```

### 2. Build BatBox

```bash
cd /path/to/claude-code
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
# Binary: build/src/batbox
```

### 3. Run a single case (no live LLM needed)

```bash
cd tests/tui-smoke
bin/harness mock-llm start
bin/harness run cases/00_smoke_first_message.sh
bin/harness mock-llm stop
```

### 4. Run all cases via harness

```bash
tests/tui-smoke/bin/harness smoke
```

### 5. Run via CTest (requires opting in)

```bash
# Enable tui-smoke in cmake (once per build dir):
cmake -DBATBOX_TUI_SMOKE=ON build/

# Run only tui-smoke tests:
ctest --test-dir build -L tui-smoke
ctest --test-dir build -L tui-smoke -V   # verbose

# Normal ctest -j8 does NOT include these tests.
```

## Harness subcommands

| Command | Description |
|---|---|
| `up [--name N] [--api-base URL]` | Start BatBox in a tmux session |
| `down [--name N]` | Kill the session |
| `send [--name N] "<text>"` | Type text + Enter |
| `key [--name N] <key>` | Send a special key (Tab, Escape, C-c, ...) |
| `screen/capture [--name N]` | Dump plain-text screen to stdout |
| `wait_for [--name N] [--timeout S] "<pattern>"` | Poll until pattern appears |
| `mock-llm start [--port P]` | Start offline mock LLM server |
| `mock-llm stop` | Stop mock LLM server |
| `run <case-file>` | Run a single case script |
| `smoke` | Run all cases in order |

## Environment variables

| Variable | Default | Description |
|---|---|---|
| `BATBOX_API_BASE_URL` | `http://127.0.0.1:8824/v1` | Override for live LLM |
| `BATBOX_NO_SPLASH` | `true` (set by harness) | Skip 1.5s splash animation |
| `BATBOX_MOCK_PORT` | `8824` | Mock LLM listening port |

## Adding new cases

Copy `cases/00_smoke_first_message.sh` as a template.
Name files `NN_description.sh` (two-digit prefix for ordering).
Include skip guards at the top for any missing infrastructure.
Register in `CMakeLists.txt` by calling `_add_tui_smoke_test(...)`.
