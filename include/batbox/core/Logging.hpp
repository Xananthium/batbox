// include/batbox/core/Logging.hpp
// =============================================================================
// batbox logging subsystem — spdlog wrapper.
//
// Design:
//   - One canonical "batbox" root logger, plus named per-module loggers that
//     share the same sink set (acquired via batbox::log::get(module_name)).
//   - Sinks are configured once by calling batbox::log::init_logging() early
//     in main().  Before init_logging() is called every logger falls back to
//     a stderr sink so nothing crashes at static-init time.
//   - Sink selection driven by environment variables (all optional):
//
//       BATBOX_LOG_LEVEL   trace|debug|info|warn|error|critical|off
//                          Default: info
//       BATBOX_LOG_FILE    Absolute or ~ path to rotating log file.
//                          Default: ~/.batbox/batbox.log
//                          Set to empty string "" to suppress file logging.
//       BATBOX_LOG_FORMAT  text|json
//                          Default: text
//
//   - When BATBOX_LOG_LEVEL is "trace" or "debug" a stderr colour sink is
//     added alongside the file sink (handy for development).
//   - File sink: rotating_file_sink_mt, 5 MB × 3 rotations.
//   - Never logs secrets: use batbox::log::redact_secret() for API key values.
//
// Usage:
//
//   #include "batbox/core/Logging.hpp"
//
//   int main() {
//       batbox::log::init_logging();         // reads env vars, sets up sinks
//
//       BATBOX_LOG_INFO("Starting batbox v{}", 1);
//       BATBOX_LOG_DEBUG("loaded {} tools", 39);
//
//       auto lg = batbox::log::get("inference");
//       lg->info("response tokens={}", 512);
//   }
//
// Convenience macros (module = "batbox" root logger):
//   BATBOX_LOG_TRACE(fmt, ...)
//   BATBOX_LOG_DEBUG(fmt, ...)
//   BATBOX_LOG_INFO(fmt, ...)
//   BATBOX_LOG_WARN(fmt, ...)
//   BATBOX_LOG_ERROR(fmt, ...)
//   BATBOX_LOG_CRITICAL(fmt, ...)
// =============================================================================

#pragma once

#include <memory>
#include <string>
#include <string_view>

// Forward-declare spdlog::logger so callers that only need the macros or
// redact_secret() don't pull the full spdlog headers.
namespace spdlog { class logger; }

namespace batbox::log {

// ---------------------------------------------------------------------------
// LogConfig — optional struct for programmatic override (unit tests, etc.)
// All fields default to "read from env" sentinel values.
// ---------------------------------------------------------------------------
struct LogConfig {
    /// spdlog level name: trace|debug|info|warn|error|critical|off.
    /// Empty = read BATBOX_LOG_LEVEL (default "info").
    std::string level;

    /// Absolute or ~-prefixed path to rotating log file.
    /// Empty = read BATBOX_LOG_FILE (default ~/.batbox/batbox.log).
    /// Set to "\x00" (null byte string) to suppress file output entirely.
    std::string log_file;

    /// "text" or "json".  Empty = read BATBOX_LOG_FORMAT (default "text").
    std::string format;
};

// ---------------------------------------------------------------------------
// init_logging() — call once near the start of main().
//
// Reads BATBOX_LOG_LEVEL / BATBOX_LOG_FILE / BATBOX_LOG_FORMAT from the
// environment (or uses LogConfig overrides when provided).
// Registers the root "batbox" logger with spdlog's global registry so that
// batbox::log::get() can find and re-use it.
// Thread-safe; subsequent calls are idempotent (re-entrant init is a no-op).
// ---------------------------------------------------------------------------
void init_logging(LogConfig cfg = {});

// ---------------------------------------------------------------------------
// get(module_name) — return (or create) a per-module logger.
//
// If init_logging() has been called, the returned logger shares the same
// sinks as the root "batbox" logger but has its own name for filtering.
// If init_logging() has NOT been called yet, returns a stderr-only logger so
// the caller never receives a null pointer.
//
// Blueprint contract: std::shared_ptr<spdlog::logger> get(std::string_view module_name)
// ---------------------------------------------------------------------------
std::shared_ptr<spdlog::logger> get(std::string_view module_name);

// ---------------------------------------------------------------------------
// redact_secret(value) — sanitise API key / token values before logging.
//
// Preserves the first 3 characters and replaces the remainder with "***".
// Examples:
//   redact_secret("sk-abc123xyz")  → "sk-***"
//   redact_secret("ab")            → "***"   (< 3 chars: fully redacted)
//   redact_secret("")              → "***"
//
// Blueprint contract: std::string redact_secret(std::string_view value)
// ---------------------------------------------------------------------------
std::string redact_secret(std::string_view value);

} // namespace batbox::log

// =============================================================================
// Convenience macros — always log to the root "batbox" logger.
// Using macros (rather than free functions) preserves __FILE__ / __LINE__
// source location info in the spdlog output pattern.
// =============================================================================

#include <spdlog/spdlog.h>

#define BATBOX_LOG_TRACE(...)    SPDLOG_LOGGER_TRACE(::batbox::log::get("batbox"),    __VA_ARGS__)
#define BATBOX_LOG_DEBUG(...)    SPDLOG_LOGGER_DEBUG(::batbox::log::get("batbox"),    __VA_ARGS__)
#define BATBOX_LOG_INFO(...)     SPDLOG_LOGGER_INFO(::batbox::log::get("batbox"),     __VA_ARGS__)
#define BATBOX_LOG_WARN(...)     SPDLOG_LOGGER_WARN(::batbox::log::get("batbox"),     __VA_ARGS__)
#define BATBOX_LOG_ERROR(...)    SPDLOG_LOGGER_ERROR(::batbox::log::get("batbox"),    __VA_ARGS__)
#define BATBOX_LOG_CRITICAL(...) SPDLOG_LOGGER_CRITICAL(::batbox::log::get("batbox"), __VA_ARGS__)
