# src

Top-level source files: application entry point, App lifecycle, clean shutdown, database migration, and sidecar setup subcommand.

## Files

### main.cpp
Program entry point: parses CLI flags and delegates to App::run().

- `main(argc, argv) -> int` — parses flags via CLI11 into AppArgs; calls App::run(args); maps exceptions to exit codes 1/2

### App.cpp
Top-level application lifecycle: initialises all subsystems and runs the TUI or headless loop.

- `App::run(args) -> int` — initialises logging, loads config, loads plugins, wires MCP servers, wires tools, wires commands, wires TUI, starts inference loop; returns 0/1/2/130
- `App::shutdown(ctx)` — reverse-of-init teardown: stops TUI → disconnects MCP → shuts down sidecar → stops supervisor → persists session → unloads plugins → flushes spdlog; idempotent via atomic flag
- `App::reset_shutdown_flag()` — clears shutdown_called_ for unit test double-shutdown scenarios

### App.hpp
App class declaration, AppArgs struct, ShutdownContext struct.

### AppShutdown.cpp
Shutdown sequence implementation detail extracted from App.cpp.

### migrate.cpp
Database migration runner for the agentic task database.

- `run_migrations(db_path) -> Result<void>` — applies any pending schema migrations to agentic/db/agentic.db; idempotent via schema_version table

### migrate.hpp
Migration function declarations.

### setup_sidecar.cpp
`setup-sidecar` subcommand: installs the Python Scrapling sidecar virtual environment.

- `run_setup_sidecar(cfg) -> int` — creates ~/.batbox/sidecar/.venv; runs pip install -r requirements.txt; verifies installation by importing scrapling; prints progress; returns 0 on success

### setup_sidecar.hpp
setup_sidecar function declaration.

### CMakeLists.txt
Build configuration for the batbox executable target.
