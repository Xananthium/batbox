// src/tools/TodoWriteTool.cpp
//
// Implementation of batbox::tools::TodoWriteTool.
//
// Blueprint contract: CPP 5.15
//   - name()="TodoWrite"
//   - args["todos"] is a JSON array of { content, status, activeForm }
//   - Replaces full session todo list (no partial updates)
//   - Status validated: "pending" | "in_progress" | "completed"
//   - At most one "in_progress" item enforced
//   - Persists to static in-process session-keyed store (no disk)
//   - Returns JSON body with previous and current list

#include <batbox/tools/TodoWriteTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>
#include <batbox/core/Json.hpp>

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace batbox::tools {

// =============================================================================
// Static store definition
// =============================================================================

std::unordered_map<std::string, std::vector<TodoItem>> TodoWriteTool::s_store_;
std::mutex                                              TodoWriteTool::s_mutex_;

// =============================================================================
// TodoItem helpers
// =============================================================================

Json TodoItem::to_json() const {
    return Json{
        {"content",    content},
        {"status",     status},
        {"activeForm", activeForm},
    };
}

bool TodoItem::from_json(const Json& j, TodoItem& out) {
    if (!j.is_object()) return false;

    // content
    auto it_c = j.find("content");
    if (it_c == j.end() || !it_c->is_string()) return false;
    const std::string content = it_c->get<std::string>();
    if (content.empty()) return false;

    // status
    auto it_s = j.find("status");
    if (it_s == j.end() || !it_s->is_string()) return false;
    const std::string status = it_s->get<std::string>();
    if (status != "pending" && status != "in_progress" && status != "completed") {
        return false;
    }

    // activeForm
    auto it_af = j.find("activeForm");
    if (it_af == j.end() || !it_af->is_string()) return false;
    const std::string activeForm = it_af->get<std::string>();
    if (activeForm.empty()) return false;

    out.content    = content;
    out.status     = status;
    out.activeForm = activeForm;
    return true;
}

// =============================================================================
// TodoWriteTool — ITool identity
// =============================================================================

std::string_view TodoWriteTool::name() const {
    return "TodoWrite";
}

std::string_view TodoWriteTool::description() const {
    return "Create and manage a structured task list for the current session. "
           "Replaces the full list; validates status values; enforces at most "
           "one in-progress item.";
}

// =============================================================================
// TodoWriteTool — schema_json()
// =============================================================================

Json TodoWriteTool::schema_json() const {
    return Json{
        {"name",        "TodoWrite"},
        {"description", std::string(description())},
        {"parameters", Json{
            {"type",       "object"},
            {"properties", Json{
                {"todos", Json{
                    {"type",        "array"},
                    {"description", "The full updated todo list. "
                                    "Replaces the previous list entirely."},
                    {"items", Json{
                        {"type",       "object"},
                        {"properties", Json{
                            {"content",    Json{
                                {"type",        "string"},
                                {"description", "The text of the todo item. Must not be empty."},
                                {"minLength",   1}
                            }},
                            {"status", Json{
                                {"type",        "string"},
                                {"enum",        Json{"pending", "in_progress", "completed"}},
                                {"description", "Current status of the todo item."}
                            }},
                            {"activeForm", Json{
                                {"type",        "string"},
                                {"description", "Active form identifier for this item. Must not be empty."},
                                {"minLength",   1}
                            }}
                        }},
                        {"required",             Json::array({"content", "status", "activeForm"})},
                        {"additionalProperties", false}
                    }}
                }}
            }},
            {"required",             Json::array({"todos"})},
            {"additionalProperties", false}
        }}
    };
}

// =============================================================================
// TodoWriteTool — run()
// =============================================================================

ToolResult TodoWriteTool::run(const Json& args, ToolContext& ctx) {
    // ------------------------------------------------------------------
    // 0. Cancellation check
    // ------------------------------------------------------------------
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    // ------------------------------------------------------------------
    // 1. Extract args["todos"]
    // ------------------------------------------------------------------
    auto it_todos = args.find("todos");
    if (it_todos == args.end() || !it_todos->is_array()) {
        return ToolResult::error(
            "TodoWrite: 'todos' argument must be a JSON array.");
    }

    // ------------------------------------------------------------------
    // 2. Validate and parse each item
    // ------------------------------------------------------------------
    std::vector<TodoItem> new_list;
    new_list.reserve(it_todos->size());

    int in_progress_count = 0;
    std::size_t idx = 0;

    for (const Json& item : *it_todos) {
        TodoItem parsed;
        if (!TodoItem::from_json(item, parsed)) {
            return ToolResult::error(
                "TodoWrite: item at index " + std::to_string(idx) +
                " is invalid. Each item requires non-empty 'content' and "
                "'activeForm' strings, and 'status' must be one of: "
                "pending, in_progress, completed.");
        }
        if (parsed.status == "in_progress") {
            ++in_progress_count;
        }
        new_list.push_back(std::move(parsed));
        ++idx;
    }

    // ------------------------------------------------------------------
    // 3. Enforce at-most-one in_progress invariant
    // ------------------------------------------------------------------
    if (in_progress_count > 1) {
        return ToolResult::error(
            "TodoWrite: at most one item may have status 'in_progress' "
            "at a time; found " + std::to_string(in_progress_count) + ".");
    }

    // ------------------------------------------------------------------
    // 4. Determine the session key
    // ------------------------------------------------------------------
    const std::string key = session_key(ctx);

    // ------------------------------------------------------------------
    // 5. Atomically swap into the session store
    // ------------------------------------------------------------------
    std::vector<TodoItem> previous;
    bool all_done = true;
    for (const auto& item : new_list) {
        if (item.status != "completed") { all_done = false; break; }
    }
    // If every item is completed, store empty (clear the list).
    const std::vector<TodoItem>& stored = all_done ? (new_list.clear(), new_list) : new_list;

    {
        std::lock_guard<std::mutex> lock(s_mutex_);
        auto it = s_store_.find(key);
        if (it != s_store_.end()) {
            previous = it->second;
        }
        s_store_[key] = stored;
    }

    // ------------------------------------------------------------------
    // 6. Build result body
    // ------------------------------------------------------------------
    Json prev_arr = Json::array();
    for (const auto& item : previous) {
        prev_arr.push_back(item.to_json());
    }

    Json curr_arr = Json::array();
    for (const auto& item : stored) {
        curr_arr.push_back(item.to_json());
    }

    Json result_body = Json{
        {"previous", prev_arr},
        {"current",  curr_arr},
    };

    return ToolResult::ok(result_body.dump(), result_body);
}

// =============================================================================
// TodoWriteTool — static helpers
// =============================================================================

std::string TodoWriteTool::session_key(const ToolContext& ctx) {
    return ctx.session_id.empty() ? ctx.agent_id : ctx.session_id;
}

std::vector<TodoItem> TodoWriteTool::get_todos(const std::string& session_key_val) {
    std::lock_guard<std::mutex> lock(s_mutex_);
    auto it = s_store_.find(session_key_val);
    if (it == s_store_.end()) return {};
    return it->second;
}

void TodoWriteTool::clear_todos(const std::string& session_key_val) {
    std::lock_guard<std::mutex> lock(s_mutex_);
    s_store_.erase(session_key_val);
}

} // namespace batbox::tools
