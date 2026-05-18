# BatBox

A local-sandbox terminal AI assistant. Single binary. C++20. No telemetry,
no auto-update, no marketplace. OpenAI-compatible inference, configurable
via `~/.batbox/.env`.

## Requirements

- macOS arm64 or Linux x64
- CMake 3.24+
- C++20 compiler (clang 15+ or gcc 12+)

## Build

    git clone https://github.com/Xananthium/batbox.git
    cd batbox
    cmake -B build -S .
    cmake --build build -j

The binary lands at `build/src/batbox`.

For syntax highlighting (tree-sitter grammars), additionally:

    git submodule update --init --recursive
    cmake -B build -S . -DBATBOX_SYNTAX=ON
    cmake --build build -j

## Configure

BatBox reads `~/.batbox/.env`. Example (do NOT commit your real values):

    BATBOX_API_BASE_URL=http://localhost:1234/v1
    BATBOX_API_KEY=your-key-here
    BATBOX_DEFAULT_MODEL=your-model-here
    BATBOX_MODELS=your-model-here,another-model-here

Note: env var names are `BATBOX_*`, not the OpenAI convention.

`BATBOX_MODELS` is a comma-separated list used by the `/model` slash command
to switch between models at runtime.

## Run

Interactive TUI:

    ./build/src/batbox

Headless (single-turn, no TUI):

    ./build/src/batbox --print "your prompt here"

In interactive mode, slash commands are available (e.g., `/model`, `/config`,
`/effort`).

## Tests

    cmake -B build -S . -DBATBOX_TESTS=ON
    cmake --build build -j
    ctest --test-dir build

TUI smoke tests via tmux harness:

    cmake -B build -S . -DBATBOX_TUI_SMOKE=ON
    ctest --test-dir build -L tui-smoke

## Layout

- `src/`            implementation
- `include/`        public headers
- `tests/`          unit + smoke tests
- `cmake/`          build helpers + vcpkg toolchain
- `vendor/`         tree-sitter grammar submodules
- `data/`           themes + model registry
- `skills/`         bundled skills
- `python-sidecar/` scrapling HTTP sidecar
- `agentic/`        project memory (planning docs + task DB)

## License

MIT (see LICENSE).
