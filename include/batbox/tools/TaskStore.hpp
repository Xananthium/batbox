// include/batbox/tools/TaskStore.hpp
//
// batbox::tools::TaskStore — shared persistent task storage for the task
// CRUD tools (TaskCreate/TaskList/TaskGet/TaskUpdate).
//
// Contract (task CPP 5.16):
//
//   Storage file : ~/.batbox/tasks.json  (JSON array of task objects)
//
//   Task object fields:
//     id          (string)  — UUIDv4, assigned on create
//     title       (string)  — non-empty human-readable label
//     description (string)  — optional, may be empty
//     status      (string)  — "pending" | "in_progress" | "completed" | "failed"
//     parent_id   (string)  — optional parent task id, empty when absent
//     tags        (array)   — string array, may be empty
//     created_at  (string)  — ISO 8601 UTC timestamp
//     updated_at  (string)  — ISO 8601 UTC timestamp
//
//   Atomic write:
//     All mutations follow the write-to-tmp / fs::rename pattern used by
//     SnipTool and WriteTool: load, mutate in memory, write to
//     <tasks_path>.tmp, then rename over the target in one syscall.
//     This ensures the file is never partially written and is compatible
//     with concurrent processes (last writer wins; good enough for typical
//     single-user / multi-agent use).
//
//   Thread-safety:
//     A std::mutex guards all load/save cycles so concurrent tool calls
//     within the same process are safe.
//
// Blueprint contract: task CPP 5.16

#pragma once

#include <batbox/core/Json.hpp>

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace batbox::tools {

// =============================================================================
// Task — plain data structure representing one persistent task entry.
// =============================================================================

struct Task {
    std::string id;           ///< UUIDv4 string
    std::string title;        ///< Human-readable label (non-empty)
    std::string description;  ///< Optional detail text
    std::string status;       ///< "pending" | "in_progress" | "completed" | "failed"
    std::string parent_id;    ///< Optional parent task id
    std::vector<std::string> tags;  ///< Optional tag list
    std::string created_at;   ///< ISO 8601 UTC
    std::string updated_at;   ///< ISO 8601 UTC

    /// Serialise to a JSON object.
    [[nodiscard]] Json to_json() const;

    /// Deserialise from a JSON object.
    /// Returns false when required fields are missing or have wrong types.
    [[nodiscard]] static bool from_json(const Json& j, Task& out);
};

// =============================================================================
// TaskCreateParams — input for TaskStore::create_task()
// =============================================================================

struct TaskCreateParams {
    std::string title;                   ///< Required; non-empty
    std::string description;             ///< Optional
    std::string status  = "pending";     ///< Default "pending"
    std::string parent_id;               ///< Optional
    std::vector<std::string> tags;       ///< Optional
};

// =============================================================================
// TaskUpdateParams — partial update for TaskStore::update_task()
// =============================================================================

struct TaskUpdateParams {
    std::optional<std::string> title;
    std::optional<std::string> description;
    std::optional<std::string> status;
    std::optional<std::string> parent_id;
    std::optional<std::vector<std::string>> tags;
};

// =============================================================================
// TaskStore — persistent task storage backed by a JSON file.
// =============================================================================

class TaskStore {
public:
    /// Construct with the path to the tasks JSON file.
    /// The file and its parent directories are created on first write.
    explicit TaskStore(std::filesystem::path tasks_path);

    /// Returns the default tasks file path: ~/.batbox/tasks.json
    [[nodiscard]] static std::filesystem::path default_path();

    // -------------------------------------------------------------------------
    // CRUD operations — all hold the internal mutex for the full load/mutate/save
    // cycle to guarantee atomicity within a single process.
    // -------------------------------------------------------------------------

    /// Create a new task.  Generates a UUIDv4 id and sets created_at/updated_at
    /// to the current UTC time.  Returns the created Task on success or an empty
    /// optional when the title is empty or the save fails.
    [[nodiscard]] std::optional<Task> create_task(const TaskCreateParams& params);

    /// Return all tasks matching the optional filters.
    /// status_filter — if non-empty, only tasks with that status are returned.
    /// tag_filter    — if non-empty, only tasks containing that tag are returned.
    [[nodiscard]] std::vector<Task> list_tasks(
        const std::string& status_filter = {},
        const std::string& tag_filter    = {}) const;

    /// Return the task with the given id, or std::nullopt if not found.
    [[nodiscard]] std::optional<Task> get_task(const std::string& id) const;

    /// Apply a partial update to the task with the given id.
    /// Sets updated_at to the current time.
    /// Returns true when the task was found and saved; false otherwise.
    [[nodiscard]] bool update_task(const std::string& id,
                                   const TaskUpdateParams& params);

    // -------------------------------------------------------------------------
    // Low-level helpers — exposed for testing.
    // -------------------------------------------------------------------------

    /// Load all tasks from disk.  Returns an empty vector if the file does not
    /// exist yet or cannot be parsed.
    [[nodiscard]] std::vector<Task> load() const;

    /// Atomically persist the task list to disk.
    /// Returns true on success; false on any filesystem error.
    [[nodiscard]] bool save(const std::vector<Task>& tasks) const;

    /// Returns the path this store was constructed with.
    [[nodiscard]] const std::filesystem::path& path() const noexcept;

private:
    std::filesystem::path  tasks_path_;
    mutable std::mutex     mutex_;

    // -------------------------------------------------------------------------
    // Internal helpers
    // -------------------------------------------------------------------------

    /// Ensure the parent directory of tasks_path_ exists.
    /// Returns true on success.
    [[nodiscard]] bool ensure_dir() const;

    /// Returns the current UTC time as an ISO 8601 string.
    [[nodiscard]] static std::string now_iso8601();

    /// Validate that `status` is one of the four allowed values.
    [[nodiscard]] static bool is_valid_status(const std::string& status);
};

} // namespace batbox::tools
