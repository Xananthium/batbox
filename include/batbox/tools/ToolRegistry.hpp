// include/batbox/tools/ToolRegistry.hpp
//
// batbox::tools::ToolRegistry — central name-to-ITool dispatch table.
//
// Lifecycle:
//   1. App::init() constructs one ToolRegistry, calls register_tool() for every
//      concrete tool, then passes the registry (by const-ref or pointer) to the
//      inference loop.
//   2. After init, the registry is effectively read-only.  All public query
//      methods (find_by_name, available_tool_schemas, dispatch) are const and
//      thread-safe for concurrent reads.
//   3. register_tool() is NOT thread-safe and MUST only be called from a single
//      thread during the startup phase before any concurrent readers exist.
//
// Blueprint contract: batbox::tools::ToolRegistry (blueprints table, task CPP 5.2)
//   - register_tool(unique_ptr<ITool>)  — add a tool; duplicate name is an error
//   - find_by_name(name)                — returns ITool* (nullptr = not found)
//   - available_tool_schemas(filter)    — returns OpenAI tools[*] JSON array
//   - dispatch(name, args, ctx)         — look up, call run(), wrap errors

#pragma once

#include <batbox/core/Json.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/tools/ITool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace batbox::tools {

// =============================================================================
// ToolRegistry
// =============================================================================

class ToolRegistry {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    /// Default-constructs an empty registry.
    ToolRegistry() = default;

    /// Non-copyable (owns unique_ptr tools).
    ToolRegistry(const ToolRegistry&)            = delete;
    ToolRegistry& operator=(const ToolRegistry&) = delete;

    /// Movable.
    ToolRegistry(ToolRegistry&&)            = default;
    ToolRegistry& operator=(ToolRegistry&&) = default;

    ~ToolRegistry() = default;

    // -------------------------------------------------------------------------
    // Registration — call only during startup, before concurrent access.
    // -------------------------------------------------------------------------

    /// Register a tool.
    ///
    /// @param tool  Owning pointer to the tool.  Must not be null.
    ///
    /// @throws std::invalid_argument if tool is null.
    /// @throws std::runtime_error    if a tool with the same name is already
    ///                               registered (programming error — two tools
    ///                               must not share a name).
    ///
    /// Thread-safety: NOT safe for concurrent calls.  Call only from the
    /// startup thread before any readers exist.
    void register_tool(std::unique_ptr<ITool> tool);

    // -------------------------------------------------------------------------
    // Query — all const, safe for concurrent reads after init.
    // -------------------------------------------------------------------------

    /// Look up a tool by its canonical name.
    ///
    /// @param name  The tool's name (as returned by ITool::name()).
    /// @returns     Non-owning pointer if found, nullptr otherwise.
    ///
    /// The returned pointer is valid for the lifetime of this ToolRegistry
    /// object.  Callers MUST NOT delete or store the pointer beyond that
    /// lifetime.
    [[nodiscard]] const ITool* find_by_name(std::string_view name) const noexcept;

    /// Returns the number of registered tools.
    [[nodiscard]] std::size_t size() const noexcept { return tools_.size(); }

    /// Returns true if no tools are registered.
    [[nodiscard]] bool empty() const noexcept { return tools_.empty(); }

    // -------------------------------------------------------------------------
    // OpenAI schema export
    // -------------------------------------------------------------------------

    /// Returns the OpenAI `tools` array for use in a ChatRequest.
    ///
    /// Each element has the shape:
    ///   { "type": "function", "function": <ITool::schema_json()> }
    ///
    /// @param filter  When absent (std::nullopt), all registered tools are
    ///                included.  When present, only tools whose name appears
    ///                in the vector are included.  Order is not guaranteed.
    ///
    /// @returns  Vector of JSON objects ready for inclusion as the `tools`
    ///           field of an OpenAI-compatible chat request body.
    [[nodiscard]] std::vector<Json> available_tool_schemas(
        const std::optional<std::vector<std::string>>& filter = std::nullopt) const;

    // -------------------------------------------------------------------------
    // Dispatch
    // -------------------------------------------------------------------------

    /// Look up a tool by name and call its run() method.
    ///
    /// @param name  Tool name to dispatch to.
    /// @param args  Parsed JSON arguments from the model's tool_call object.
    /// @param ctx   Per-dispatch context (cwd, permission mode, cancel token).
    ///
    /// @returns  Ok(ToolResult)  — the tool ran (result may itself be is_error).
    ///           Err(std::string) — the named tool is not registered, or
    ///                             the tool is not allowed in the current
    ///                             context (plan mode + non-read-only, or not
    ///                             in allowed_tools list).
    ///
    /// Errors thrown by ITool::run() are caught and converted to
    /// ToolResult::error so the model can self-correct; they do NOT propagate
    /// as C++ exceptions.
    [[nodiscard]] Result<ToolResult, std::string> dispatch(
        std::string_view name,
        const Json&      args,
        ToolContext&     ctx);

private:
    // Insertion-order list for schema export (preserves registration order).
    std::vector<std::string>                              insertion_order_;
    // Name → owned tool.
    std::unordered_map<std::string, std::unique_ptr<ITool>> tools_;
};

} // namespace batbox::tools
