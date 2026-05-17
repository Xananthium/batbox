// include/batbox/tools/TaskCreateTool.hpp
//
// batbox::tools::TaskCreateTool — create a new persistent task entry.
//
// Contract (blueprints table task CPP 5.16):
//
//   Tool name   : "TaskCreate"
//   Arguments   :
//     title       (string,  required) — task label; non-empty
//     description (string,  optional) — detail text
//     status      (string,  optional) — "pending"(default)|"in_progress"|
//                                       "completed"|"failed"
//     parent_id   (string,  optional) — parent task id
//     tags        (array,   optional) — string array of labels
//
//   Behaviour:
//     1. Validate title (non-empty) and status (if provided).
//     2. Delegate to TaskStore::create_task() — assigns UUID, timestamps.
//     3. Return ToolResult::ok(body) where body is the task JSON.
//     4. Return ToolResult::error(msg) on validation or storage failure.
//
//   Permission flags:
//     is_read_only()          == false  (creates a file entry)
//     requires_confirmation() == false  (user-local task store)
//
// Blueprint contract: batbox::tools::TaskCreateTool (CPP 5.16)

#pragma once

#include <batbox/tools/ITool.hpp>
#include <batbox/tools/TaskStore.hpp>
#include <batbox/core/Json.hpp>

#include <memory>

namespace batbox::tools {

// =============================================================================
// TaskCreateTool
// =============================================================================

/// Implements the "TaskCreate" tool: create a new persistent task stored in
/// ~/.batbox/tasks.json via the shared TaskStore.
class TaskCreateTool final : public ITool {
public:
    /// Construct with an injected TaskStore.
    explicit TaskCreateTool(std::shared_ptr<TaskStore> store);

    // -------------------------------------------------------------------------
    // ITool identity
    // -------------------------------------------------------------------------

    /// Returns "TaskCreate".
    [[nodiscard]] std::string_view name() const override;

    /// Returns a one-sentence description for the model schema.
    [[nodiscard]] std::string_view description() const override;

    // -------------------------------------------------------------------------
    // OpenAI tool schema
    // -------------------------------------------------------------------------

    [[nodiscard]] Json schema_json() const override;

    // -------------------------------------------------------------------------
    // Execution
    // -------------------------------------------------------------------------

    /// Create a task.
    ///
    /// args["title"]       — required non-empty string
    /// args["description"] — optional string
    /// args["status"]      — optional enum string (default "pending")
    /// args["parent_id"]   — optional string
    /// args["tags"]        — optional string array
    ///
    /// @returns ToolResult::ok(task_json)   on success.
    ///          ToolResult::error(msg)       on validation or store failure.
    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override;

    // -------------------------------------------------------------------------
    // Permission gate hooks
    // -------------------------------------------------------------------------

    /// false — creates a persistent file entry.
    [[nodiscard]] bool is_read_only() const override { return false; }

    /// false — user-local task store; no confirmation prompt needed.
    [[nodiscard]] bool requires_confirmation() const override { return false; }

private:
    std::shared_ptr<TaskStore> store_;
};

} // namespace batbox::tools
