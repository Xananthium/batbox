// include/batbox/tools/TaskListTool.hpp
//
// batbox::tools::TaskListTool — list persistent tasks with optional filtering.
//
// Contract (blueprints table task CPP 5.16):
//
//   Tool name   : "TaskList"
//   Arguments   :
//     status  (string, optional) — filter by status value
//     tag     (string, optional) — filter by tag membership
//
//   Behaviour:
//     1. Delegate to TaskStore::list_tasks() with optional filters.
//     2. Return ToolResult::ok(body) where body is a JSON array of task objects.
//        Empty array when no tasks match.
//     3. Return ToolResult::error(msg) on cancellation or unexpected error.
//
//   Permission flags:
//     is_read_only()          == true   (reads only; allowed in Plan mode)
//     requires_confirmation() == false  (no prompt needed)
//
// Blueprint contract: batbox::tools::TaskListTool (CPP 5.16)

#pragma once

#include <batbox/tools/ITool.hpp>
#include <batbox/tools/TaskStore.hpp>
#include <batbox/core/Json.hpp>

#include <memory>

namespace batbox::tools {

// =============================================================================
// TaskListTool
// =============================================================================

/// Implements the "TaskList" tool: return persistent tasks from
/// ~/.batbox/tasks.json, optionally filtered by status and/or tag.
class TaskListTool final : public ITool {
public:
    explicit TaskListTool(std::shared_ptr<TaskStore> store);

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

    /// List tasks.
    ///
    /// args["status"]  — optional status filter string
    /// args["tag"]     — optional tag filter string
    ///
    /// @returns ToolResult::ok(json_array)  always on success (may be empty array).
    ///          ToolResult::error(msg)       on cancellation or unexpected error.
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
