// include/batbox/tools/TaskUpdateTool.hpp
//
// batbox::tools::TaskUpdateTool — partially update a persistent task by id.
//
// Contract (blueprints table task CPP 5.16):
//
//   Tool name   : "TaskUpdate"
//   Arguments   :
//     id          (string,  required) — UUID of the task to update
//     title       (string,  optional) — new title value
//     description (string,  optional) — new description value
//     status      (string,  optional) — new status value (validated)
//     parent_id   (string,  optional) — new parent_id (empty string clears it)
//     tags        (array,   optional) — new tags array (replaces existing)
//
//   Behaviour:
//     1. Validate args["id"] (required, non-empty).
//     2. Validate args["status"] if present (must be allowed enum value).
//     3. Build a TaskUpdateParams with only the fields present in args.
//     4. Delegate to TaskStore::update_task() — sets updated_at to now.
//     5. Return ToolResult::ok(updated_task_json) on success.
//        Return ToolResult::error(msg) when not found or validation fails.
//
//   Permission flags:
//     is_read_only()          == false  (mutates persistent storage)
//     requires_confirmation() == false  (user-local task store)
//
// Blueprint contract: batbox::tools::TaskUpdateTool (CPP 5.16)

#pragma once

#include <batbox/tools/ITool.hpp>
#include <batbox/tools/TaskStore.hpp>
#include <batbox/core/Json.hpp>

#include <memory>

namespace batbox::tools {

// =============================================================================
// TaskUpdateTool
// =============================================================================

/// Implements the "TaskUpdate" tool: atomically update one or more fields of
/// a persistent task in ~/.batbox/tasks.json via the shared TaskStore.
class TaskUpdateTool final : public ITool {
public:
    explicit TaskUpdateTool(std::shared_ptr<TaskStore> store);

    // -------------------------------------------------------------------------
    // ITool identity
    // -------------------------------------------------------------------------

    [[nodiscard]] std::string_view name() const override;
    [[nodiscard]] std::string_view description() const override;

    // -------------------------------------------------------------------------
    // OpenAI tool schema
    // -------------------------------------------------------------------------

    [[nodiscard]] Json schema_json() const override;

    // -------------------------------------------------------------------------
    // Execution
    // -------------------------------------------------------------------------

    /// Update task fields.
    ///
    /// args["id"]          — required non-empty string (UUID)
    /// args["title"]       — optional string
    /// args["description"] — optional string
    /// args["status"]      — optional enum string
    /// args["parent_id"]   — optional string
    /// args["tags"]        — optional string array
    ///
    /// Only supplied fields are updated; absent fields keep their current values.
    ///
    /// @returns ToolResult::ok(updated_task_json) on success.
    ///          ToolResult::error(msg)              on not-found or validation error.
    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override;

    // -------------------------------------------------------------------------
    // Permission gate hooks
    // -------------------------------------------------------------------------

    /// false — mutates the persistent task store.
    [[nodiscard]] bool is_read_only() const override { return false; }

    /// false — user-local task store; no confirmation prompt needed.
    [[nodiscard]] bool requires_confirmation() const override { return false; }

private:
    std::shared_ptr<TaskStore> store_;
};

} // namespace batbox::tools
