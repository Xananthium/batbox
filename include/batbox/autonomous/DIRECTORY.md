# Autonomous runtime interface

`Engine.hpp` declares runtime options and `run(options, config, registry)`. The
caller owns the configured native tool registry and its referenced services for
the lifetime of the engine. State defaults to `~/.batbox/autonomous` and the
human input file defaults to `~/love_letters.txt`.
