// include/batbox/tools/CronScheduler.hpp
//
// batbox::tools::CronScheduler — in-process cron scheduler.
//
// Contract (CPP 5.17 blueprint):
//
//   Storage file : ~/.batbox/cron.json  (JSON array of cron entry objects)
//
//   CronEntry fields:
//     id          (string)  — UUIDv4, assigned on create
//     expression  (string)  — standard 5-field cron: "MIN HOUR DOM MON DOW"
//                             Each field: integer, "*", or comma-separated list.
//                             Ranges (1-5) and step values (*/5) NOT required.
//     prompt      (string)  — the prompt/command to dispatch when the entry fires
//     enabled     (bool)    — when false the entry is persisted but not scheduled
//     created_at  (string)  — ISO 8601 UTC timestamp
//
//   Scheduler:
//     One background std::thread checks every 1s for due entries.
//     Fires entries whose next_fire_time <= now.
//     Recomputes next_fire_time after each fire.
//     Honors a std::stop_token for clean shutdown.
//     Callbacks (fire handlers) are registered by the application layer;
//     the scheduler calls fire_callback_(entry) when an entry is due.
//
//   Thread-safety:
//     A std::mutex guards all load/save/schedule cycles.
//     The background thread holds the mutex only briefly per check.
//
// Blueprint contract: CPP 5.17

#pragma once

#include <batbox/core/Json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace batbox::tools {

// =============================================================================
// CronEntry — persistent cron schedule entry.
// =============================================================================

struct CronEntry {
    std::string id;          ///< UUIDv4 string
    std::string expression;  ///< 5-field cron expression "MIN HOUR DOM MON DOW"
    std::string prompt;      ///< Prompt dispatched on fire
    bool        enabled = true; ///< When false: persisted but not fired
    std::string created_at;  ///< ISO 8601 UTC

    /// Serialise to a JSON object.
    [[nodiscard]] Json to_json() const;

    /// Deserialise from a JSON object.  Returns false on error.
    [[nodiscard]] static bool from_json(const Json& j, CronEntry& out);
};

// =============================================================================
// CronExpr — parsed 5-field cron expression
// =============================================================================

/// Internal parsed form of one cron field: a sorted list of valid values
/// in the field's natural range, or empty meaning "any" (wildcard).
struct CronField {
    bool        is_wildcard = true;
    std::vector<int> values;        ///< non-empty sorted list when !is_wildcard

    /// Returns true if `v` is allowed by this field.
    [[nodiscard]] bool matches(int v) const noexcept;
};

struct CronExpr {
    CronField minute;   ///< [0,59]
    CronField hour;     ///< [0,23]
    CronField dom;      ///< [1,31]
    CronField month;    ///< [1,12]
    CronField dow;      ///< [0,6]  Sunday=0

    /// Parse a 5-field cron expression string.
    /// Returns false (and leaves *this* unmodified) on parse error.
    [[nodiscard]] static bool parse(const std::string& expr, CronExpr& out);

    /// Compute the next fire time after `from_time` (exclusive).
    /// Returns std::nullopt if no fire time exists within the next 4 years.
    [[nodiscard]] std::optional<std::time_t>
        next_fire(std::time_t from_time) const noexcept;
};

// =============================================================================
// CronScheduler — background scheduler + persistence
// =============================================================================

/// Fire callback signature: called (from the background thread) when a cron
/// entry is due.  The entry is passed by value for thread-safety.
using CronFireCallback = std::function<void(const CronEntry&)>;

class CronScheduler {
public:
    /// Construct with path to cron.json storage file.
    explicit CronScheduler(std::filesystem::path cron_path);

    /// Destructor — stops the background thread cleanly.
    ~CronScheduler();

    // Non-copyable, non-movable (owns a thread).
    CronScheduler(const CronScheduler&)            = delete;
    CronScheduler& operator=(const CronScheduler&) = delete;
    CronScheduler(CronScheduler&&)                 = delete;
    CronScheduler& operator=(CronScheduler&&)      = delete;

    // -------------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------------

    /// Start the background scheduler thread.
    /// Set fire_callback before calling start().
    /// Safe to call only once per instance.
    void start();

    /// Stop the background scheduler thread.
    /// Blocks until the thread joins.  Safe to call multiple times.
    void stop() noexcept;

    /// Register the callback invoked when an entry fires.
    /// Must be set before start(); thread-safe otherwise.
    void set_fire_callback(CronFireCallback cb);

    // -------------------------------------------------------------------------
    // Default storage path
    // -------------------------------------------------------------------------

    /// Returns ~/.batbox/cron.json
    [[nodiscard]] static std::filesystem::path default_path();

    // -------------------------------------------------------------------------
    // CRUD — all hold mutex for load/mutate/save cycle
    // -------------------------------------------------------------------------

    /// Add a new cron entry.  Generates UUID + created_at.
    /// Returns the created entry on success, or nullopt on validation/IO error.
    [[nodiscard]] std::optional<CronEntry> create_entry(
        const std::string& expression,
        const std::string& prompt,
        bool               enabled = true);

    /// Return all entries (enabled and disabled).
    [[nodiscard]] std::vector<CronEntry> list_entries() const;

    /// Remove the entry with the given id.
    /// Returns true if found and removed; false if not found.
    [[nodiscard]] bool delete_entry(const std::string& id);

    // -------------------------------------------------------------------------
    // Low-level helpers — exposed for testing
    // -------------------------------------------------------------------------

    /// Load all entries from disk.  Empty vector if file missing/unparseable.
    [[nodiscard]] std::vector<CronEntry> load() const;

    /// Atomically persist entries to disk.  Returns true on success.
    [[nodiscard]] bool save(const std::vector<CronEntry>& entries) const;

    /// Returns the path this scheduler was constructed with.
    [[nodiscard]] const std::filesystem::path& path() const noexcept;

private:
    std::filesystem::path  cron_path_;
    mutable std::mutex     mutex_;
    CronFireCallback       fire_callback_;
    std::jthread           thread_;

    // -------------------------------------------------------------------------
    // Background thread logic
    // -------------------------------------------------------------------------
    void scheduler_loop(std::stop_token stop_tok);

    // -------------------------------------------------------------------------
    // Internal helpers
    // -------------------------------------------------------------------------
    [[nodiscard]] bool ensure_dir() const;
    [[nodiscard]] static std::string now_iso8601();
    [[nodiscard]] static std::time_t now_time_t() noexcept;
};

} // namespace batbox::tools
