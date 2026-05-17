// src/core/Logging.cpp
// =============================================================================
// Implementation of the batbox logging subsystem (spdlog wrapper).
// See include/batbox/core/Logging.hpp for API documentation.
// =============================================================================

#include "batbox/core/Logging.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/ansicolor_sink.h>
#include <spdlog/pattern_formatter.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace batbox::log {

namespace {

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------
// g_init_mutex guards both init_logging() and per-module logger creation.
// g_sinks holds the shared sink set so per-module loggers can share them.
std::mutex                     g_init_mutex;
std::vector<spdlog::sink_ptr>  g_sinks;

// ---------------------------------------------------------------------------
// Helper: expand leading "~" to the user home directory.
// ---------------------------------------------------------------------------
static std::string expand_home(std::string_view path) {
    if (path.empty() || path[0] != '~') {
        return std::string(path);
    }
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    return std::string(home) + std::string(path.substr(1));
}

// ---------------------------------------------------------------------------
// Helper: read env var with fallback.
// ---------------------------------------------------------------------------
static std::string env_or(const char* var, std::string_view fallback) {
    const char* val = std::getenv(var);
    if (val && val[0] != '\0') return std::string(val);
    return std::string(fallback);
}

// ---------------------------------------------------------------------------
// Helper: parse spdlog level from string (case-insensitive).
// Returns spdlog::level::info if the string is unrecognised.
// ---------------------------------------------------------------------------
static spdlog::level::level_enum parse_level(const std::string& s) {
    std::string lc = s;
    std::transform(lc.begin(), lc.end(), lc.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    if (lc == "trace")    return spdlog::level::trace;
    if (lc == "debug")    return spdlog::level::debug;
    if (lc == "info")     return spdlog::level::info;
    if (lc == "warn" || lc == "warning") return spdlog::level::warn;
    if (lc == "error")    return spdlog::level::err;
    if (lc == "critical") return spdlog::level::critical;
    if (lc == "off")      return spdlog::level::off;
    return spdlog::level::info; // safe default
}

// ---------------------------------------------------------------------------
// Helper: build and register the root "batbox" logger with the given sinks
// and level. Called under g_init_mutex.
// ---------------------------------------------------------------------------
static std::shared_ptr<spdlog::logger>
build_root_logger(const std::vector<spdlog::sink_ptr>& sinks,
                  spdlog::level::level_enum              level) {
    // Drop any existing registration so we can re-register.
    spdlog::drop("batbox");
    auto logger = std::make_shared<spdlog::logger>(
        "batbox", sinks.begin(), sinks.end());
    logger->set_level(level);
    logger->flush_on(spdlog::level::err);
    spdlog::register_logger(logger);
    return logger;
}

// ---------------------------------------------------------------------------
// Helper: return a stderr-only fallback logger for pre-init use.
// The fallback logger is NOT registered in spdlog's global registry so it
// doesn't interfere with a later init_logging() call.
// ---------------------------------------------------------------------------
static std::shared_ptr<spdlog::logger> make_fallback_logger(const std::string& name) {
    auto sink = std::make_shared<spdlog::sinks::ansicolor_stderr_sink_mt>();
    sink->set_level(spdlog::level::trace);
    auto logger = std::make_shared<spdlog::logger>(name, sink);
    logger->set_level(spdlog::level::warn); // only warn+ before init
    return logger;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// init_logging()
// ---------------------------------------------------------------------------
void init_logging(LogConfig cfg) {
    std::lock_guard<std::mutex> lock(g_init_mutex);

    // --- Resolve configuration (explicit cfg > env > defaults) ---

    const std::string level_str = cfg.level.empty()
        ? env_or("BATBOX_LOG_LEVEL", "info")
        : cfg.level;

    // Sentinel "\x00" (null byte string) means suppress file logging.
    const bool suppress_file = (!cfg.log_file.empty() && cfg.log_file[0] == '\x00');
    std::string log_file_path;
    if (!suppress_file) {
        log_file_path = cfg.log_file.empty()
            ? expand_home(env_or("BATBOX_LOG_FILE", "~/.batbox/batbox.log"))
            : expand_home(cfg.log_file);
    }

    const std::string format_str = cfg.format.empty()
        ? env_or("BATBOX_LOG_FORMAT", "text")
        : cfg.format;

    const spdlog::level::level_enum level = parse_level(level_str);

    // --- Build sinks ---

    g_sinks.clear();

    // File sink (rotating, 5 MB × 3 files), unless suppressed.
    if (!suppress_file && !log_file_path.empty()) {
        try {
            std::filesystem::path p(log_file_path);
            if (p.has_parent_path()) {
                std::filesystem::create_directories(p.parent_path());
            }

            constexpr std::size_t max_size  = 5ULL * 1024ULL * 1024ULL; // 5 MB
            constexpr std::size_t max_files = 3;
            auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                log_file_path, max_size, max_files);
            file_sink->set_level(spdlog::level::trace);
            g_sinks.push_back(std::move(file_sink));
        } catch (const std::exception&) {
            // Permission denied or other error — fall through to stderr fallback.
        }
    }

    // Stderr colour sink: always present for trace/debug; also when file
    // creation failed and g_sinks is empty (ensures at least one active sink).
    const bool want_stderr = (level <= spdlog::level::debug) || g_sinks.empty();
    if (want_stderr) {
        auto stderr_sink = std::make_shared<spdlog::sinks::ansicolor_stderr_sink_mt>();
        stderr_sink->set_level(spdlog::level::trace);
        g_sinks.push_back(std::move(stderr_sink));
    }

    // --- Apply pattern to all sinks ---
    // text:  [2026-05-15 12:00:00.123] [batbox] [info] message
    // json:  {"ts":"...","logger":"batbox","level":"info","msg":"..."}
    const bool json_format = (format_str == "json");
    for (auto& sink : g_sinks) {
        if (json_format) {
            sink->set_pattern(
                R"({"ts":"%Y-%m-%dT%H:%M:%S.%e","logger":"%n","level":"%l","msg":"%v"})");
        } else {
            sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] %v");
        }
    }

    // Build (or rebuild) the root "batbox" logger.
    build_root_logger(g_sinks, level);
}

// ---------------------------------------------------------------------------
// get(module_name)
// ---------------------------------------------------------------------------
std::shared_ptr<spdlog::logger> get(std::string_view module_name) {
    const std::string name(module_name);

    // Fast path: look up existing logger in the spdlog registry.
    auto existing = spdlog::get(name);
    if (existing) return existing;

    // Not yet initialised (no root "batbox" logger) — return a transient
    // stderr-only fallback. We do NOT register it so init_logging() can
    // cleanly take over later. (The next call to get() after init will find
    // the freshly registered logger via spdlog::get().)
    auto root = spdlog::get("batbox");
    if (!root) {
        return make_fallback_logger(name);
    }

    // Initialised but this named logger not yet created. Create it under the
    // init mutex to avoid races.
    std::lock_guard<std::mutex> lock(g_init_mutex);

    // Re-check under the lock — another thread may have created it already.
    auto existing2 = spdlog::get(name);
    if (existing2) return existing2;

    auto logger = std::make_shared<spdlog::logger>(
        name, g_sinks.begin(), g_sinks.end());
    logger->set_level(root->level());
    logger->flush_on(spdlog::level::err);
    spdlog::register_logger(logger);
    return logger;
}

// ---------------------------------------------------------------------------
// redact_secret(value)
// ---------------------------------------------------------------------------
std::string redact_secret(std::string_view value) {
    constexpr std::size_t keep = 3;
    if (value.size() <= keep) {
        return "***";
    }
    return std::string(value.substr(0, keep)) + "***";
}

} // namespace batbox::log
