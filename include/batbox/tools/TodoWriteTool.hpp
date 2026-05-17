// include/batbox/tools/TodoWriteTool.hpp
//
// batbox::tools::TodoWriteTool — manages a per-session todo checklist.
//
// Contract (blueprints table task CPP 5.15):
//
//   Tool name   : "TodoWrite"
//   Arguments   : args["todos"] — JSON array of todo objects:
//                   { "content":    <string>,                   // non-empty
//                     "status":     "pending"|"in_progress"|"completed",
//                     "activeForm": <string> }                  // non-empty
//
//   Behaviour:
//     1. Validate each item: content and activeForm non-empty, status is one
//        of the three allowed values.
//     2. Enforce the invariant: at most one item may have status "in_progress"
//        at a time.
//     3. Replace the full todo list for this session (not a merge/patch).
//        If every item is "completed" the stored list becomes empty.
//     4. Persist the list in an in-process session-keyed store (no disk I/O).
//        Key: ctx.session_id when non-empty; otherwise ctx.agent_id.
//     5. Return ToolResult::ok(body) where body is a compact JSON string
//        containing the previous list and the new list for the caller.
//     6. Return ToolResult::error(msg) on validation failure.
//     7. is_read_only()          == false (modifies session state)
//     8. requires_confirmation() == false (no user prompt needed)
//
// Thread-safety:
//   The static session store is protected by a std::mutex so concurrent
//   calls from different threads with the same or different session keys
//   are safe.
//
// Blueprint contract: batbox::tools::TodoWriteTool (blueprints table CPP 5.15)

#pragma once

#include <batbox/tools/ITool.hpp>
#include <batbox/core/Json.hpp>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace batbox::tools {

// =============================================================================
// TodoItem — a single entry in the todo list.
// =============================================================================

struct TodoItem {
    std::string content;
    std::string status;     // "pending" | "in_progress" | "completed"
    std::string activeForm;

    /// Serialise to a JSON object.
    [[nodiscard]] Json to_json() const;

    /// Deserialise from a JSON object.  Returns false when fields are absent,
    /// non-string, or status is not one of the three allowed values.
    [[nodiscard]] static bool from_json(const Json& j, TodoItem& out);
};

// =============================================================================
// TodoWriteTool
// =============================================================================

/// Implements the "TodoWrite" tool: atomically replaces the caller's session
/// todo list, validates status values, enforces the single-in_progress
/// invariant, and returns the old and new lists in the result body.
class TodoWriteTool final : public ITool {
public:
    TodoWriteTool() = default;

    // -------------------------------------------------------------------------
    // ITool identity
    // -------------------------------------------------------------------------

    /// Returns "TodoWrite".
    [[nodiscard]] std::string_view name() const override;

    /// Returns a one-sentence description surfaced to the model in the schema.
    [[nodiscard]] std::string_view description() const override;

    // -------------------------------------------------------------------------
    // OpenAI tool schema
    // -------------------------------------------------------------------------

    /// Returns the full OpenAI tools[*].function JSON object for this tool.
    [[nodiscard]] Json schema_json() const override;

    // -------------------------------------------------------------------------
    // Execution
    // -------------------------------------------------------------------------

    /// Replace the session todo list.
    ///
    /// Steps:
    ///   1. Extract and validate args["todos"] array.
    ///   2. Validate each item: non-empty content and activeForm, valid status.
    ///   3. Enforce: at most one item with status "in_progress".
    ///   4. Atomically replace the stored list for the session key derived
    ///      from ctx (session_id if non-empty, otherwise agent_id).
    ///   5. If all items are "completed", store an empty list.
    ///   6. Return ToolResult::ok(body) where body is a JSON string:
    ///        { "previous": [...], "current": [...] }
    ///      "current" reflects the stored list after step 5.
    ///
    /// @returns ToolResult::ok(body)    on success.
    ///          ToolResult::error(msg)  on any validation error.
    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override;

    // -------------------------------------------------------------------------
    // Permission gate hooks
    // -------------------------------------------------------------------------

    /// false — TodoWriteTool mutates session state.
    [[nodiscard]] bool is_read_only() const override { return false; }

    /// false — no confirmation prompt needed for todo list management.
    [[nodiscard]] bool requires_confirmation() const override { return false; }

    // -------------------------------------------------------------------------
    // Session store access — exposed for testing.
    // -------------------------------------------------------------------------

    /// Returns a copy of the current todo list for the given session key.
    /// Thread-safe; returns an empty vector when the key is absent.
    [[nodiscard]] static std::vector<TodoItem> get_todos(const std::string& session_key);

    /// Clears the todo list for the given session key (test teardown / reset).
    static void clear_todos(const std::string& session_key);

private:
    // -------------------------------------------------------------------------
    // Static session store — shared across all TodoWriteTool instances.
    // -------------------------------------------------------------------------

    /// Map from session_key → current todo list.
    static std::unordered_map<std::string, std::vector<TodoItem>> s_store_;

    /// Guards s_store_ for concurrent access.
    static std::mutex s_mutex_;

    // -------------------------------------------------------------------------
    // Helpers
    // -------------------------------------------------------------------------

    /// Returns the session key to use for the given context.
    [[nodiscard]] static std::string session_key(const ToolContext& ctx);
};

} // namespace batbox::tools
