// include/batbox/tools/TaskGetTool.hpp
//
// batbox::tools::TaskGetTool — retrieve a single persistent task by id.
//
// Contract (blueprints table task CPP 5.16):
//
//   Tool name   : "TaskGet"
//   Arguments   :
//     id  (string, required) — UUID of the task to retrieve
//
//   Behaviour:
//     1. Validate that args["id"] is a non-empty string.
//     2. Delegate to TaskStore::get_task(id).
//     3. Return ToolResult::ok(task_json)  when found.
//        Return ToolResult::error(msg)      when not found or on error.
//
//   Permission flags:
//     is_read_only()          == true   (reads only; allowed in Plan mode)
//     requires_confirmation() == false  (no prompt needed)
//
// Blueprint contract: batbox::tools::TaskGetTool (CPP 5.16)

#pragma once

#include <batbox/tools/ITool.hpp>
#include <batbox/tools/TaskStore.hpp>
#include <batbox/core/Json.hpp>

#include <memory>

namespace batbox::tools {

// =============================================================================
// TaskGetTool
// =============================================================================

/// Implements the "TaskGet" tool: retrieve a single task by its UUID from
/// ~/.batbox/tasks.json via the shared TaskStore.
class TaskGetTool final : public ITool {
public:
    explicit TaskGetTool(std::shared_ptr<TaskStore> store);

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

    /// Get a task by id.
    ///
    /// args["id"]  — required non-empty string (UUID)
    ///
    /// @returns ToolResult::ok(task_json)  when the task exists.
    ///          ToolResult::error(msg)      when not found or args invalid.
    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override;

    // -------------------------------------------------------------------------
    // Permission gate hooks
    // -------------------------------------------------------------------------

    /// true — reads the task store only; safe in Plan mode.
    [[nodiscard]] bool is_read_only() const override { return true; }

    /// false — no confirmation prompt needed.
    [[nodiscard]] bool requires_confirmation() const override { return false; }

private:
    std::shared_ptr<TaskStore> store_;
};

} // namespace batbox::tools
