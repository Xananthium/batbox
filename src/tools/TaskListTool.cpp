// src/tools/TaskListTool.cpp
//
// Implementation of batbox::tools::TaskListTool.
//
// Blueprint contract: CPP 5.16
//   - name()="TaskList"
//   - args["status"] optional filter; args["tag"] optional filter
//   - is_read_only()=true
//   - Returns JSON array of matching tasks

#include <batbox/tools/TaskListTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>

#include <string>
#include <string_view>

namespace batbox::tools {

// =============================================================================
// Construction
// =============================================================================

TaskListTool::TaskListTool(std::shared_ptr<TaskStore> store)
    : store_(std::move(store))
{}

// =============================================================================
// ITool identity
// =============================================================================

std::string_view TaskListTool::name() const {
    return "TaskList";
}

std::string_view TaskListTool::description() const {
    return "List persistent tasks from ~/.batbox/tasks.json. "
           "Optionally filter by status or tag. "
           "Returns a JSON array of task objects.";
}

// =============================================================================
// schema_json()
// =============================================================================

Json TaskListTool::schema_json() const {
    return Json{
        {"name",        "TaskList"},
        {"description", std::string(description())},
        {"parameters", Json{
            {"type",       "object"},
            {"properties", Json{
                {"status", Json{
                    {"type",        "string"},
                    {"enum",        Json::array({"pending", "in_progress",
                                                 "completed", "failed"})},
                    {"description", "Optional status filter. "
                                    "When omitted all tasks are returned."}
                }},
                {"tag", Json{
                    {"type",        "string"},
                    {"description", "Optional tag filter. "
                                    "When provided only tasks containing this tag are returned."}
                }}
            }},
            {"required",             Json::array()},
            {"additionalProperties", false}
        }}
    };
}

// =============================================================================
// run()
// =============================================================================

ToolResult TaskListTool::run(const Json& args, ToolContext& ctx) {
    try {
        // ------------------------------------------------------------------
        // 0. Cancellation check.
        // ------------------------------------------------------------------
        if (ctx.is_cancelled()) {
            return ToolResult::error("cancelled");
        }

        // ------------------------------------------------------------------
        // 1. Extract optional filters.
        // ------------------------------------------------------------------
        std::string status_filter;
        const auto status_it = args.find("status");
        if (status_it != args.end() && status_it->is_string()) {
            status_filter = status_it->get<std::string>();
        }

        std::string tag_filter;
        const auto tag_it = args.find("tag");
        if (tag_it != args.end() && tag_it->is_string()) {
            tag_filter = tag_it->get<std::string>();
        }

        // ------------------------------------------------------------------
        // 2. Delegate to TaskStore.
        // ------------------------------------------------------------------
        const std::vector<Task> tasks = store_->list_tasks(status_filter, tag_filter);

        // ------------------------------------------------------------------
        // 3. Serialise and return.
        // ------------------------------------------------------------------
        Json arr = Json::array();
        for (const auto& t : tasks) {
            arr.push_back(t.to_json());
        }
        return ToolResult::ok(arr.dump(), arr);

    } catch (const std::exception& ex) {
        return ToolResult::error(
            std::string("TaskList: unexpected error: ") + ex.what());
    } catch (...) {
        return ToolResult::error("TaskList: unknown error.");
    }
}

} // namespace batbox::tools
