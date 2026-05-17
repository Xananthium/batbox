// src/repl/History.cpp
// ---------------------------------------------------------------------------
// Implementation of batbox::repl::History.
//
// Persistence format
// ------------------
//   Plain text, one entry per line, UTF-8.  Lines are written with '\n'
//   (Unix line endings). Empty lines and lines that decode to whitespace-only
//   strings are skipped on load.
//
// Atomic write strategy
// ---------------------
//   save() writes to <persist_file>.tmp, then calls std::filesystem::rename()
//   which is atomic on POSIX systems (within the same filesystem).  The .tmp
//   file is removed on failure so stale temporaries do not accumulate.
//
// Cap enforcement on load
// -----------------------
//   If the on-disk file contains more than cap_ entries only the last
//   (newest) cap_ entries are kept.  This keeps memory use bounded even
//   when the file was written with a larger cap.
// ---------------------------------------------------------------------------

#include "batbox/repl/History.hpp"
#include "batbox/core/Paths.hpp"

#include <algorithm>
#include <cstdlib>       // std::getenv
#include <fstream>
#include <stdexcept>
#include <string>

namespace batbox::repl {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

// Read BATBOX_HISTORY_SIZE env-var; returns 0 if unset or invalid.
std::size_t history_size_from_env() {
    const char* env = std::getenv("BATBOX_HISTORY_SIZE");
    if (env == nullptr || env[0] == '\0') {
        return 0;
    }
    try {
        const long v = std::stol(env);
        if (v >= 1) {
            return static_cast<std::size_t>(v);
        }
    } catch (...) {
        // Invalid value — ignore and use default.
    }
    return 0;
}

// Return the default persist file path: config_dir() / "history", or an
// empty path when BATBOX_HISTORY_FILE="" (explicitly disabled by caller).
std::filesystem::path default_persist_file() {
    const char* env = std::getenv("BATBOX_HISTORY_FILE");
    if (env != nullptr) {
        // Empty string means "disable persistence".
        if (env[0] == '\0') {
            return {};
        }
        return std::filesystem::path{env};
    }
    return batbox::paths::config_dir() / "history";
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

History::History(std::optional<std::filesystem::path> persist_file, std::size_t cap)
    : cap_{ [&]() -> std::size_t {
          // Environment variable overrides the constructor argument.
          const std::size_t env_cap = history_size_from_env();
          if (env_cap > 0) return env_cap;
          return (cap > 0) ? cap : kDefaultCap;
      }() }
    , persist_file_{
          !persist_file.has_value()
              // No argument: resolve from env / default location.
              ? default_persist_file()
              // Argument provided: use it as-is (empty path = disabled).
              : std::move(*persist_file)
      }
    , cursor_{ 0 }
{
    // Load history from disk; silently ignores missing file.
    load();
    // Cursor starts past-the-end.
    cursor_ = entries_.size();
}

// ---------------------------------------------------------------------------
// is_blank()
// ---------------------------------------------------------------------------

/*static*/ bool History::is_blank(std::string_view s) noexcept {
    for (const char c : s) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            return false;
        }
    }
    return true;  // empty or all-whitespace
}

// ---------------------------------------------------------------------------
// push()
// ---------------------------------------------------------------------------

void History::push(std::string_view line) {
    // Reject empty / whitespace-only entries.
    if (is_blank(line)) {
        return;
    }

    // Reject consecutive duplicate.
    if (!entries_.empty() && entries_.back() == line) {
        reset_cursor();
        return;
    }

    entries_.emplace_back(line);

    // Evict oldest if we exceed cap.
    while (entries_.size() > cap_) {
        entries_.pop_front();
    }

    // Always reset navigation cursor after a new entry.
    cursor_ = entries_.size();
}

// ---------------------------------------------------------------------------
// at()
// ---------------------------------------------------------------------------

std::optional<std::string> History::at(std::size_t age) const {
    if (age >= entries_.size()) {
        return std::nullopt;
    }
    // age 0 → most recent → entries_.back()
    const std::size_t idx = entries_.size() - 1 - age;
    return entries_[idx];
}

// ---------------------------------------------------------------------------
// previous() / next() / reset_cursor()
// ---------------------------------------------------------------------------

std::optional<std::string> History::previous() {
    if (entries_.empty()) {
        return std::nullopt;
    }
    // Already at the oldest entry — don't go further back.
    if (cursor_ == 0) {
        return entries_.front();
    }
    --cursor_;
    return entries_[cursor_];
}

std::optional<std::string> History::next() {
    if (entries_.empty()) {
        return std::nullopt;
    }
    // Already at or past the end — nothing newer.
    if (cursor_ >= entries_.size()) {
        return std::nullopt;
    }
    ++cursor_;
    // cursor_ == entries_.size() means "past the end" (live-input position).
    if (cursor_ == entries_.size()) {
        return std::nullopt;
    }
    return entries_[cursor_];
}

void History::reset_cursor() {
    cursor_ = entries_.size();
}

// ---------------------------------------------------------------------------
// size() / cap()
// ---------------------------------------------------------------------------

std::size_t History::size() const noexcept {
    return entries_.size();
}

std::size_t History::cap() const noexcept {
    return cap_;
}

// ---------------------------------------------------------------------------
// clear()
// ---------------------------------------------------------------------------

void History::clear() noexcept {
    entries_.clear();
    cursor_ = 0;
}

// ---------------------------------------------------------------------------
// save()
// ---------------------------------------------------------------------------

void History::save() const {
    if (persist_file_.empty()) {
        return;  // Persistence disabled.
    }

    // Ensure parent directory exists.
    const auto parent = persist_file_.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    const std::filesystem::path tmp_path = std::filesystem::path{persist_file_}.concat(".tmp");

    // Write to temporary file.
    {
        std::ofstream out{tmp_path, std::ios::out | std::ios::trunc};
        if (!out) {
            throw std::ios_base::failure{
                "batbox::repl::History::save: cannot open tmp file: " +
                tmp_path.string()};
        }
        for (const auto& entry : entries_) {
            out << entry << '\n';
            if (!out) {
                // Attempt cleanup before throwing.
                out.close();
                std::error_code ec;
                std::filesystem::remove(tmp_path, ec);
                throw std::ios_base::failure{
                    "batbox::repl::History::save: write error to " +
                    tmp_path.string()};
            }
        }
    }  // out flushed and closed by destructor.

    // Atomic rename.
    std::error_code ec;
    std::filesystem::rename(tmp_path, persist_file_, ec);
    if (ec) {
        // Cleanup tmp on failure.
        std::error_code rm_ec;
        std::filesystem::remove(tmp_path, rm_ec);
        throw std::filesystem::filesystem_error{
            "batbox::repl::History::save: rename failed", tmp_path,
            persist_file_, ec};
    }
}

// ---------------------------------------------------------------------------
// load()
// ---------------------------------------------------------------------------

void History::load() {
    if (persist_file_.empty()) {
        return;  // Persistence disabled.
    }

    if (!std::filesystem::exists(persist_file_)) {
        return;  // No history yet — silently succeed.
    }

    std::ifstream in{persist_file_};
    if (!in) {
        throw std::ios_base::failure{
            "batbox::repl::History::load: cannot open " +
            persist_file_.string()};
    }

    std::deque<std::string> loaded;
    std::string line;
    while (std::getline(in, line)) {
        // Strip trailing \r for files written on Windows.
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (is_blank(line)) {
            continue;
        }
        loaded.push_back(std::move(line));
    }

    if (in.bad()) {
        throw std::ios_base::failure{
            "batbox::repl::History::load: read error on " +
            persist_file_.string()};
    }

    // Apply cap: keep only the last (newest) cap_ entries.
    while (loaded.size() > cap_) {
        loaded.pop_front();
    }

    entries_ = std::move(loaded);
    cursor_  = entries_.size();
}

// ---------------------------------------------------------------------------
// persist_file()
// ---------------------------------------------------------------------------

const std::filesystem::path& History::persist_file() const noexcept {
    return persist_file_;
}

} // namespace batbox::repl
