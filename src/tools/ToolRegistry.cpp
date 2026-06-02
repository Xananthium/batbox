// src/tools/ToolRegistry.cpp
//
// Implementation of batbox::tools::ToolRegistry.
//
// See include/batbox/tools/ToolRegistry.hpp for the full public API contract
// and thread-safety guarantees.

#include <batbox/tools/ToolRegistry.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace batbox::tools {

// =============================================================================
// register_tool
// =============================================================================

void ToolRegistry::register_tool(std::unique_ptr<ITool> tool) {
    if (!tool) {
        throw std::invalid_argument("ToolRegistry::register_tool: tool must not be null");
    }

    std::string name{tool->name()};

    auto [it, inserted] = tools_.emplace(name, std::move(tool));
    if (!inserted) {
        throw std::runtime_error(
            "ToolRegistry::register_tool: duplicate tool name \"" + name + "\"");
    }

    insertion_order_.push_back(std::move(name));
}

// =============================================================================
// find_by_name
// =============================================================================

const ITool* ToolRegistry::find_by_name(std::string_view name) const noexcept {
    auto it = tools_.find(std::string{name});
    if (it == tools_.end()) {
        return nullptr;
    }
    return it->second.get();
}

// =============================================================================
// available_tool_schemas
// =============================================================================

std::vector<Json> ToolRegistry::available_tool_schemas(
    const std::optional<std::vector<std::string>>& filter) const
{
    std::vector<Json> result;
    result.reserve(insertion_order_.size());

    for (const std::string& name : insertion_order_) {
        // Apply filter: skip tools whose name is not in the allow-list.
        if (filter.has_value()) {
            const auto& allow = *filter;
            bool found = std::find(allow.begin(), allow.end(), name) != allow.end();
            if (!found) {
                continue;
            }
        }

        auto it = tools_.find(name);
        if (it == tools_.end()) {
            // Shouldn't happen (insertion_order_ and tools_ are kept in sync),
            // but be defensive.
            continue;
        }

        // Wrap ITool::schema_json() in the OpenAI tools[*] envelope:
        //   { "type": "function", "function": <schema_json()> }
        result.push_back(Json{
            {"type",     "function"},
            {"function", it->second->schema_json()}
        });
    }

    return result;
}

// =============================================================================
// dispatch
// =============================================================================

Result<ToolResult, std::string> ToolRegistry::dispatch(
    std::string_view name,
    const Json&      args,
    ToolContext&     ctx)
{
    // 1. Look up the tool via non-const map access, because ITool::run() is
    //    non-const (tools may update internal state such as call history).
    ITool* tool = nullptr;
    {
        auto it = tools_.find(std::string{name});
        if (it != tools_.end()) {
            tool = it->second.get();
        }
    }
    if (!tool) {
        return Unexpected<std::string>(
            std::string{"ToolRegistry::dispatch: unknown tool \""} + std::string{name} + "\"");
    }

    // 2. Check allowed_tools list from context.
    if (!ctx.tool_is_allowed(std::string{name})) {
        return Unexpected<std::string>(
            std::string{"ToolRegistry::dispatch: tool \""} + std::string{name}
            + "\" is not in the allowed_tools list for this context");
    }

    // 3. Check plan-mode gate: non-read-only tools are blocked in Plan mode.
    if (ctx.is_plan_mode() && !tool->is_read_only()) {
        return Unexpected<std::string>(
            std::string{"ToolRegistry::dispatch: tool \""} + std::string{name}
            + "\" is not allowed in Plan mode (not read-only)");
    }

    // 4. Call run(); catch all exceptions and convert to ToolResult::error so
    //    the model can self-correct rather than crashing the session.  The
    //    exception-to-error wrapping below is byte-identical to pre-S7.
    ToolResult tr;
    try {
        tr = tool->run(args, ctx);
    } catch (const std::exception& ex) {
        tr = ToolResult::error(
            std::string{"ToolRegistry::dispatch: tool \""} + std::string{name}
            + "\" threw an exception: " + ex.what());
    } catch (...) {
        tr = ToolResult::error(
            std::string{"ToolRegistry::dispatch: tool \""} + std::string{name}
            + "\" threw an unknown exception");
    }

    // 5. Subagent-dispatch seam (S7): route the result of the invoked run()
    //    through the envelope.  This is the single, unbypassable boundary where
    //    every tool result — native or MCP, success or error — becomes a
    //    subagent result.  With the S7 default pass-through hooks this returns
    //    `tr` unchanged (byte-identical to pre-S7); S1+S4 fill the hooks later
    //    without re-touching this function.
    return envelope_.process(name, args, std::move(tr), ctx);
}

} // namespace batbox::tools
