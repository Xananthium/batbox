// src/tools/TaskUpdateTool.cpp
//
// Implementation of batbox::tools::TaskUpdateTool.
//
// Blueprint contract: CPP 5.16
//   - name()="TaskUpdate"
//   - args["id"] required; other fields optional partial update
//   - Atomic write via TaskStore::update_task()
//   - Returns updated task JSON on success

#include <batbox/tools/TaskUpdateTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace batbox::tools {

// =============================================================================
// Construction
// =============================================================================

TaskUpdateTool::TaskUpdateTool(std::shared_ptr<TaskStore> store)
    : store_(std::move(store))
{}

// =============================================================================
// ITool identity
// =============================================================================

std::string_view TaskUpdateTool::name() const {
    return "TaskUpdate";
}

std::string_view TaskUpdateTool::description() const {
    return "Partially update a persistent task by UUID in ~/.batbox/tasks.json. "
           "Only supplied fields are changed; others keep their current values. "
           "Sets updated_at to the current time atomically.";
}

// =============================================================================
// schema_json()
// =============================================================================

Json TaskUpdateTool::schema_json() const {
    return Json{
        {"name",        "TaskUpdate"},
        {"description", std::string(description())},
        {"parameters", Json{
            {"type",       "object"},
            {"properties", Json{
                {"id", Json{
                    {"type",        "string"},
                    {"description", "UUID of the task to update."},
                    {"minLength",   1}
                }},
                {"title", Json{
                    {"type",        "string"},
                    {"description", "New title value. Must not be empty if supplied."}
                }},
                {"description", Json{
                    {"type",        "string"},
                    {"description", "New description value."}
                }},
                {"status", Json{
                    {"type",        "string"},
                    {"enum",        Json::array({"pending", "in_progress",
                                                 "completed", "failed"})},
                    {"description", "New status value."}
                }},
                {"parent_id", Json{
                    {"type",        "string"},
                    {"description", "New parent task UUID. Pass empty string to clear."}
                }},
                {"tags", Json{
                    {"type",        "array"},
                    {"items",       Json{{"type", "string"}}},
                    {"description", "New tags array. Replaces the existing tags entirely."}
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

ToolResult TaskUpdateTool::run(const Json& args, ToolContext& ctx) {
    try {
        // ------------------------------------------------------------------
        // 0. Cancellation check.
        // ------------------------------------------------------------------
        if (ctx.is_cancelled()) {
            return ToolResult::error("cancelled");
        }

        // ------------------------------------------------------------------
        // 1. Plan-mode gate.
        // ------------------------------------------------------------------
        if (ctx.is_plan_mode()) {
            return ToolResult::error(
                "TaskUpdate: not allowed in Plan mode (mutates persistent storage).");
        }

        // ------------------------------------------------------------------
        // 2. Extract and validate required "id".
        // ------------------------------------------------------------------
        const auto id_it = args.find("id");
        if (id_it == args.end() || !id_it->is_string()) {
            return ToolResult::error(
                "TaskUpdate: required argument 'id' is missing or not a string.");
        }
        const std::string id = id_it->get<std::string>();
        if (id.empty()) {
            return ToolResult::error("TaskUpdate: 'id' must not be empty.");
        }

        // ------------------------------------------------------------------
        // 3. Build the partial update params.
        // ------------------------------------------------------------------
        TaskUpdateParams params;

        const auto title_it = args.find("title");
        if (title_it != args.end() && title_it->is_string()) {
            const std::string new_title = title_it->get<std::string>();
            if (new_title.empty()) {
                return ToolResult::error(
                    "TaskUpdate: 'title' must not be empty when supplied.");
            }
            params.title = new_title;
        }

        const auto desc_it = args.find("description");
        if (desc_it != args.end() && desc_it->is_string()) {
            params.description = desc_it->get<std::string>();
        }

        const auto status_it = args.find("status");
        if (status_it != args.end() && status_it->is_string()) {
            const std::string new_status = status_it->get<std::string>();
            if (new_status != "pending" && new_status != "in_progress"
             && new_status != "completed" && new_status != "failed") {
                return ToolResult::error(
                    "TaskUpdate: invalid 'status' value '" + new_status
                    + "'. Must be one of: pending, in_progress, completed, failed.");
            }
            params.status = new_status;
        }

        const auto parent_it = args.find("parent_id");
        if (parent_it != args.end() && parent_it->is_string()) {
            params.parent_id = parent_it->get<std::string>();
        }

        const auto tags_it = args.find("tags");
        if (tags_it != args.end() && tags_it->is_array()) {
            std::vector<std::string> new_tags;
            for (const auto& tag_item : *tags_it) {
                if (tag_item.is_string()) {
                    new_tags.push_back(tag_item.get<std::string>());
                }
            }
            params.tags = std::move(new_tags);
        }

        // ------------------------------------------------------------------
        // 4. Apply the update via TaskStore.
        // ------------------------------------------------------------------
        const bool updated = store_->update_task(id, params);
        if (!updated) {
            return ToolResult::error(
                "TaskUpdate: task with id '" + id + "' not found.");
        }

        // ------------------------------------------------------------------
        // 5. Retrieve and return the updated task.
        // ------------------------------------------------------------------
        const std::optional<Task> task = store_->get_task(id);
        if (!task.has_value()) {
            // Should not happen — but handle defensively.
            return ToolResult::error(
                "TaskUpdate: task was updated but could not be retrieved.");
        }

        const Json task_json = task->to_json();
        return ToolResult::ok(task_json.dump(), task_json);

    } catch (const std::exception& ex) {
        return ToolResult::error(
            std::string("TaskUpdate: unexpected error: ") + ex.what());
    } catch (...) {
        return ToolResult::error("TaskUpdate: unknown error.");
    }
}

} // namespace batbox::tools
