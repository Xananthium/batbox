// src/tools/TaskGetTool.cpp
//
// Implementation of batbox::tools::TaskGetTool.
//
// Blueprint contract: CPP 5.16
//   - name()="TaskGet"
//   - args["id"] required non-empty string
//   - is_read_only()=true
//   - Returns single task JSON or error

#include <batbox/tools/TaskGetTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>

#include <string>
#include <string_view>

namespace batbox::tools {

// =============================================================================
// Construction
// =============================================================================

TaskGetTool::TaskGetTool(std::shared_ptr<TaskStore> store)
    : store_(std::move(store))
{}

// =============================================================================
// ITool identity
// =============================================================================

std::string_view TaskGetTool::name() const {
    return "TaskGet";
}

std::string_view TaskGetTool::description() const {
    return "Retrieve a single persistent task by its UUID from ~/.batbox/tasks.json. "
           "Returns the full task object as JSON.";
}

// =============================================================================
// schema_json()
// =============================================================================

Json TaskGetTool::schema_json() const {
    return Json{
        {"name",        "TaskGet"},
        {"description", std::string(description())},
        {"parameters", Json{
            {"type",       "object"},
            {"properties", Json{
                {"id", Json{
                    {"type",        "string"},
                    {"description", "UUID of the task to retrieve."},
                    {"minLength",   1}
                }}
            }},
            {"required",             Json::array({"id"})},
            {"additionalProperties", false}
        }}
    };
}

// =============================================================================
// run()
// =============================================================================

ToolResult TaskGetTool::run(const Json& args, ToolContext& ctx) {
    try {
        // ------------------------------------------------------------------
        // 0. Cancellation check.
        // ------------------------------------------------------------------
        if (ctx.is_cancelled()) {
            return ToolResult::error("cancelled");
        }

        // ------------------------------------------------------------------
        // 1. Extract and validate required "id".
        // ------------------------------------------------------------------
        const auto id_it = args.find("id");
        if (id_it == args.end() || !id_it->is_string()) {
            return ToolResult::error(
                "TaskGet: required argument 'id' is missing or not a string.");
        }
        const std::string id = id_it->get<std::string>();
        if (id.empty()) {
            return ToolResult::error("TaskGet: 'id' must not be empty.");
        }

        // ------------------------------------------------------------------
        // 2. Delegate to TaskStore.
        // ------------------------------------------------------------------
        const std::optional<Task> task = store_->get_task(id);
        if (!task.has_value()) {
            return ToolResult::error(
                "TaskGet: task with id '" + id + "' not found.");
        }

        // ------------------------------------------------------------------
        // 3. Return task JSON.
        // ------------------------------------------------------------------
        const Json task_json = task->to_json();
        return ToolResult::ok(task_json.dump(), task_json);

    } catch (const std::exception& ex) {
        return ToolResult::error(
            std::string("TaskGet: unexpected error: ") + ex.what());
    } catch (...) {
        return ToolResult::error("TaskGet: unknown error.");
    }
}

} // namespace batbox::tools
