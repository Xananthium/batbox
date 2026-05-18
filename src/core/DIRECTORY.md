# src/core

Core utility implementations: cooperative cancellation, JSON parsing, logging setup, path resolution, and UUID generation.

## Files

### CancelToken.cpp
`CancelSource` and `CancelToken` implementations; child() creates a stop_callback that fires the child source; combine_tokens() uses a shared callback connecting two stop_tokens to a third source.

### Json.cpp
`parse()`, `parse_fast()`, `parse_simdjson_doc()`, `dump()`, `pretty()`, `get_or()`, `path_get()` implementations; simdjson on-demand parser with nlohmann as output type for uniform downstream use.

### Logging.cpp
`init_logging()` implementation: creates rotating file sink (batbox.log) and color stderr sink; `get()` uses spdlog registry; `redact_secret()` length-based redaction.

### Paths.cpp
`home_dir()`, `config_dir()`, `expand_tilde()`, `project_root()` implementations; config_dir() creates the ~/.batbox directory on first call; project_root() walks up from cwd looking for BATBOX.md.

### Uuid.cpp
`Uuid::v4()` implementation: calls arc4random_buf on macOS, getrandom on Linux, falls back to /dev/urandom; sets version (4) and variant bits per RFC 4122; `uuid_v4()` free function wraps v4().to_string().

### CMakeLists.txt
Build rules for the core static library.
