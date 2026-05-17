// src/tools/TaskCreateTool.cpp
//
// Implementation of batbox::tools::TaskCreateTool.
//
// Blueprint contract: CPP 5.16
//   - name()="TaskCreate"
//   - args["title"] required non-empty string
//   - args["description"], args["status"], args["parent_id"], args["tags"] optional
//   - Delegates to TaskStore::create_task()
//   - Returns task JSON on success

#include <batbox/tools/TaskCreateTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace batbox::tools {

// =============================================================================
// Construction
// =============================================================================

TaskCreateTool::TaskCreateTool(std::shared_ptr<TaskStore> store)
    : store_(std::move(store))
{}

// =============================================================================
// ITool identity
// =============================================================================

std::string_view TaskCreateTool::name() const {
    return "TaskCreate";
}

std::string_view TaskCreateTool::description() const {
    return "Create a new persistent task stored in ~/.batbox/tasks.json. "
           "Assigns a UUID and timestamps automatically. "
           "Supports title, description, status, parent_id, and tags.";
}

// =============================================================================
// schema_json()
// =============================================================================

Json TaskCreateTool::schema_json() const {
    return Json{
        {"name",        "TaskCreate"},
        {"description", std::string(description())},
        {"parameters", Json{
            {"type",       "object"},
            {"properties", Json{
                {"title", Json{
                    {"type",        "string"},
                    {"description", "Task label. Must not be empty."},
                    {"minLength",   1}
                }},
                {"description", Json{
                    {"type",        "string"},
                    {"description", "Optional task detail text."}
                }},
                {"status", Json{
                    {"type",        "string"},
                    {"enum",        Json::array({"pending", "in_progress",
                                                 "completed", "failed"})},
                    {"description", "Initial status. Defaults to 'pending'."}
                }},
                {"parent_id", Json{
                    {"type",        "string"},
                    {"description", "Optional UUID of the parent task."}
                }},
                {"tags", Json{
                    {"type",        "array"},
                    {"items",       Json{{"type", "string"}}},
                    {"description", "Optional list of string labels."}
                }}
            }},
            {"required",             Json::array({"title"})},
            {"additionalProperties", false}
        }}
    };
}

// =============================================================================
// run()
// =============================================================================

ToolResult TaskCreateTool::run(const Json& args, ToolContext& ctx) {
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
                "TaskCreate: not allowed in Plan mode (creates persistent storage).");
        }

        // ------------------------------------------------------------------
        // 2. Extract and validate required "title".
        // ------------------------------------------------------------------
        const auto title_it = args.find("title");
        if (title_it == args.end() || !title_it->is_string()) {
            return ToolResult::error(
                "TaskCreate: required argument 'title' is missing or not a string.");
        }
        const std::string title = title_it->get<std::string>();
        if (title.empty()) {
            return ToolResult::error(
                "TaskCreate: 'title' must not be empty.");
        }

        // ------------------------------------------------------------------
        // 3. Extract optional "status".
        // ------------------------------------------------------------------
        std::string status = "pending";
        const auto status_it = args.find("status");
        if (status_it != args.end()) {
            if (!status_it->is_string()) {
                return ToolResult::error(
                    "TaskCreate: 'status' must be a string.");
            }
            status = status_it->get<std::string>();
            if (status != "pending" && status != "in_progress"
             && status != "completed" && status != "failed") {
                return ToolResult::error(
                    "TaskCreate: invalid 'status' value '" + status
                    + "'. Must be one of: pending, in_progress, completed, failed.");
            }
        }

        // ------------------------------------------------------------------
        // 4. Extract optional fields.
        // ------------------------------------------------------------------
        std::string description;
        const auto desc_it = args.find("description");
        if (desc_it != args.end() && desc_it->is_string()) {
            description = desc_it->get<std::string>();
        }

        std::string parent_id;
        const auto parent_it = args.find("parent_id");
        if (parent_it != args.end() && parent_it->is_string()) {
            parent_id = parent_it->get<std::string>();
        }

        std::vector<std::string> tags;
        const auto tags_it = args.find("tags");
        if (tags_it != args.end() && tags_it->is_array()) {
            for (const auto& tag_item : *tags_it) {
                if (tag_item.is_string()) {
                    tags.push_back(tag_item.get<std::string>());
                }
            }
        }

        // ------------------------------------------------------------------
        // 5. Delegate to TaskStore.
        // ------------------------------------------------------------------
        TaskCreateParams params;
        params.title       = title;
        params.description = description;
        params.status      = status;
        params.parent_id   = parent_id;
        params.tags        = std::move(tags);

        const std::optional<Task> created = store_->create_task(params);
        if (!created.has_value()) {
            return ToolResult::error(
                "TaskCreate: failed to persist task. "
                "Check that ~/.batbox/ is writable.");
        }

        // ------------------------------------------------------------------
        // 6. Return the created task as JSON.
        // ------------------------------------------------------------------
        const Json task_json = created->to_json();
        return ToolResult::ok(task_json.dump(), task_json);

    } catch (const std::exception& ex) {
        return ToolResult::error(
            std::string("TaskCreate: unexpected error: ") + ex.what());
    } catch (...) {
        return ToolResult::error("TaskCreate: unknown error.");
    }
}

} // namespace batbox::tools
