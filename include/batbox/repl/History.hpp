// include/batbox/repl/History.hpp
// ---------------------------------------------------------------------------
// In-memory command history with optional on-disk persistence.
//
// Design
// ------
//  • Entries are stored newest-last in a std::deque<std::string>.
//  • A ring-buffer cap (default 10 000) evicts the oldest entry when
//    exceeded, preserving the N most-recent inputs.
//  • Consecutive duplicate suppression: push() silently ignores an entry
//    that is identical to the most-recently stored one.
//  • Empty / whitespace-only entries are never stored.
//  • Persistence: one entry per line in a plain-text file; the path
//    defaults to Paths::config_dir() / "history" but is overridable at
//    construction time (or via the BATBOX_HISTORY_FILE env var).
//  • Atomic write: save() writes to <file>.tmp then renames for crash safety.
//  • Navigation: previous() / next() implement the up/down arrow walk that
//    the InputBar drives; reset_cursor() resets to the "beyond the end"
//    position (call after a push or on Escape).
//
// Thread safety
// -------------
//  All methods must be called from one thread (the REPL input thread).
//  No internal synchronisation is provided.
//
// Capacity environment override
// ------------------------------
//  BATBOX_HISTORY_SIZE=<n>   — maximum number of entries stored in memory
//                              and written to disk (default 10 000, min 1).
// ---------------------------------------------------------------------------
#pragma once

#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <cstddef>

namespace batbox::repl {

class History {
public:
    // -----------------------------------------------------------------------
    // Default cap: matches common shell history sizes (bash HISTSIZE default).
    // -----------------------------------------------------------------------
    static constexpr std::size_t kDefaultCap = 10'000;

    // -----------------------------------------------------------------------
    // Constructor
    //
    // persist_file — path to the on-disk history file.
    //   std::nullopt (default): use Paths::config_dir() / "history"
    //                           (~/.batbox/history), unless BATBOX_HISTORY_FILE
    //                           env var overrides it.
    //   empty fs::path         : disable persistence entirely (no file I/O).
    //   non-empty fs::path     : use the given path.
    //
    // cap — maximum number of in-memory entries (default kDefaultCap = 10 000).
    //       Overridden by BATBOX_HISTORY_SIZE env var if set to a valid integer.
    //
    // At construction the file is loaded (if it exists and persistence is
    // enabled) so previous sessions are immediately available.
    // -----------------------------------------------------------------------
    explicit History(std::optional<std::filesystem::path> persist_file = std::nullopt,
                     std::size_t cap = kDefaultCap);

    // Non-copyable — history owns its cursor state.
    History(const History&)            = delete;
    History& operator=(const History&) = delete;

    // Movable.
    History(History&&)            = default;
    History& operator=(History&&) = default;

    ~History() = default;

    // -----------------------------------------------------------------------
    // push(line)
    //
    // Appends line to the in-memory deque and resets the navigation cursor.
    //
    // Silently ignores:
    //   • empty strings
    //   • strings that contain only whitespace
    //   • strings identical to the most-recent stored entry (consecutive dup)
    //
    // When the deque exceeds cap(), the oldest entry is evicted.
    // -----------------------------------------------------------------------
    void push(std::string_view line);

    // -----------------------------------------------------------------------
    // at(age)
    //
    // Returns the entry at the given age (0 = most recent, 1 = one before,
    // …). Returns std::nullopt when age >= size().
    // -----------------------------------------------------------------------
    [[nodiscard]] std::optional<std::string> at(std::size_t age) const;

    // -----------------------------------------------------------------------
    // previous() / next()
    //
    // Walk the history backwards / forwards for up/down arrow navigation.
    //
    // previous() — moves the cursor one step older; returns the entry at the
    //              new cursor position, or std::nullopt when already at the
    //              oldest entry.
    //
    // next()     — moves the cursor one step newer; returns the entry at the
    //              new cursor position, or std::nullopt when the cursor is
    //              already past the end (i.e. at the "live input" position).
    //
    // reset_cursor() — resets the cursor to the "past-the-end" position so
    //                  the next previous() call fetches the most-recent entry.
    //                  Call this after push() or on Escape.
    // -----------------------------------------------------------------------
    std::optional<std::string> previous();
    std::optional<std::string> next();
    void reset_cursor();

    // -----------------------------------------------------------------------
    // size() — number of entries currently stored.
    // cap()  — maximum entries allowed before eviction.
    // -----------------------------------------------------------------------
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t cap()  const noexcept;

    // -----------------------------------------------------------------------
    // clear() — removes all in-memory entries and resets the cursor.
    //           Does NOT delete or truncate the on-disk file; call save()
    //           afterwards if disk state should match.
    // -----------------------------------------------------------------------
    void clear() noexcept;

    // -----------------------------------------------------------------------
    // save()
    //
    // Writes the current in-memory entries to the persist file (one per line).
    // Uses an atomic write: writes to <file>.tmp then renames to <file>.
    //
    // No-op when persist_file() is empty (persistence disabled).
    //
    // Throws: std::filesystem::filesystem_error or std::ios_base::failure on
    //         I/O error.
    // -----------------------------------------------------------------------
    void save() const;

    // -----------------------------------------------------------------------
    // load()
    //
    // Reads entries from the persist file into the in-memory deque.
    // Silently no-ops if the file does not exist.
    // Replaces any existing in-memory content.
    //
    // No-op when persist_file() is empty (persistence disabled).
    //
    // Throws: std::ios_base::failure on read error (other than file-not-found).
    // -----------------------------------------------------------------------
    void load();

    // -----------------------------------------------------------------------
    // persist_file() — returns the configured on-disk path (may be empty).
    // -----------------------------------------------------------------------
    [[nodiscard]] const std::filesystem::path& persist_file() const noexcept;

private:
    // entries_[0] is the oldest; entries_.back() is the most recent.
    std::deque<std::string> entries_;

    // Maximum entries retained in memory (and written to disk).
    std::size_t cap_;

    // On-disk persistence path.  Empty → persistence disabled.
    std::filesystem::path persist_file_;

    // Navigation cursor.
    // cursor_ == entries_.size()  → "past the end" (no active traversal).
    // cursor_ == 0                → pointing at the oldest entry.
    // decremented by previous(), incremented by next().
    std::size_t cursor_;

    // Internal helper: trim trailing whitespace from a string view.
    [[nodiscard]] static bool is_blank(std::string_view s) noexcept;
};

} // namespace batbox::repl
