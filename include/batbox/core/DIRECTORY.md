# include/batbox/core

Foundational utilities: cooperative cancellation, JSON parsing, logging, filesystem paths, the Result<T,E> type, and UUID generation.

## Files

### CancelToken.hpp
Cooperative cancellation primitives built on std::stop_token.

- `CancelSource::token() -> CancelToken` — returns a child token that fires when this source is stopped
- `CancelSource::request_stop()` — fires the stop signal; all tokens derived from this source become cancelled
- `CancelSource::stop_requested() -> bool` — returns true after request_stop() has been called
- `CancelSource::native() -> stop_source&` — returns the underlying std::stop_source for jthread integration
- `CancelToken::make_root() -> pair<CancelSource, CancelToken>` — creates a root source+token pair
- `CancelToken::child() -> pair<CancelSource, CancelToken>` — creates a child that fires when either itself or its parent fires
- `CancelToken::is_cancelled() -> bool` — returns true when the underlying stop_token has fired
- `CancelToken::stop_requested() -> bool` — alias for is_cancelled()
- `CancelToken::throw_if_cancelled()` — throws CancelledException if cancelled; call at checkpoints in long operations
- `CancelToken::on_cancel(fn)` — registers a callback invoked when cancellation fires; returns a stop_callback handle
- `combine_tokens(a, b) -> CancelToken` — returns a token that fires when either a or b fires

### Json.hpp
Dual-backend JSON interface: nlohmann for ergonomics, simdjson for bulk parsing.

- `parse(sv) -> expected<Json, string>` — parses JSON from string_view using nlohmann; returns Err with message on syntax error
- `parse_fast(sv) -> expected<Json, string>` — parses using simdjson on-demand parser; faster for large payloads
- `parse_simdjson_doc(bytes) -> expected<Json, string>` — parses raw bytes via simdjson; returns nlohmann::json for uniform downstream use
- `dump(j) -> string` — serialises Json to compact string (no whitespace)
- `pretty(j) -> string` — serialises Json with 2-space indentation
- `get_or<T>(j, key, default) -> T` — extracts j[key] as type T; returns default on missing key or type mismatch
- `path_get(j, dotted_path) -> optional<Json>` — navigates nested objects via dot-separated key path (e.g. "a.b.c"); returns nullopt on missing segment

### Logging.hpp
spdlog initialisation and module logger factory.

- `init_logging(cfg={})` — creates file + stderr sinks; sets log level from cfg.general.log_level; registers spdlog to flush on WARN+
- `get(module_name) -> shared_ptr<spdlog::logger>` — returns a named child logger; creates it if absent; child inherits root sink chain
- `redact_secret(value) -> string` — returns "***" for strings longer than 8 chars (API keys); passes short strings through unchanged
- `BATBOX_LOG_TRACE/DEBUG/INFO/WARN/ERROR/CRITICAL` — module-aware logging macros; each resolves to spdlog::get(MODULE) and logs with source location

### Paths.hpp
Standard filesystem path helpers.

- `home_dir() -> fs::path` — returns $HOME or getpwuid fallback; throws on failure
- `config_dir() -> fs::path` — returns $BATBOX_CONFIG_DIR if set, else ~/.batbox; creates the directory on first call
- `expand_tilde(path) -> fs::path` — replaces leading ~ with home_dir(); leaves paths without ~ unchanged
- `project_root() -> fs::path` — walks up from cwd looking for BATBOX.md; returns cwd when not found

### Result.hpp
Rust-style Result<T,E> type alias.

- `Result<T,E>` — alias for std::expected<T,E> on C++23; polyfill on C++20; value-semantics, no exception overhead
- `Unexpected<E>` — wrapper for error values; constructible from any E
- `make_unexpected(e) -> Unexpected<E>` — wraps e in an Unexpected; analogous to std::make_unexpected
- `Err(e)` — convenience alias for make_unexpected; used as `return Err("message")`

### Uuid.hpp
UUIDv4 generation and parsing.

- `Uuid::v4() -> Uuid` — generates a random UUIDv4 using arc4random_buf (macOS) or getrandom (Linux)
- `Uuid::parse(sv) -> optional<Uuid>` — parses a UUID string in xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx format; returns nullopt on malformed input
- `Uuid::to_string() -> string` — serialises to lowercase hyphenated string
- `Uuid::is_nil() -> bool` — returns true when all 16 bytes are zero
- `Uuid::nil() -> Uuid` — returns the all-zero nil UUID
- `uuid_v4() -> string` — free function; generates and returns a new UUIDv4 string directly
