// include/batbox/tools/ITool.hpp
//
// batbox::tools::ITool — pure-virtual base class every tool implements.
//
// Contract (per ned-cpp.md §2.C5 and blueprints table row 16616):
//
//   name()                — stable snake_case identifier, e.g. "read_file".
//                           Must be unique across the registry.
//   description()         — one-sentence natural language description surfaced
//                           to the model in the tool schema.
//   schema_json()         — returns the full OpenAI tools[*].function object as
//                           a batbox::Json value:
//                             {
//                               "name": "<name>",
//                               "description": "<description>",
//                               "parameters": { <JSON Schema object> }
//                             }
//                           Implementations own the object and may return a
//                           const reference or a freshly constructed value.
//   run(args, ctx)        — execute the tool; args is the parsed JSON object
//                           from the model's tool_call, ctx carries per-dispatch
//                           state.  Returns a ToolResult (ok or error).
//   is_read_only()        — default false.  Plan-mode gate: when true, the tool
//                           may run even in Plan mode.  Override to true for
//                           non-mutating tools (Read, Glob, Grep, ToolSearch …).
//   requires_confirmation()— default true.  Permission-gate hint: when false,
//                           the tool never triggers an interactive confirmation
//                           prompt regardless of the active PermissionMode.
//                           Typically false for read-only or idempotent ops.
//
// ABI note:
//   All virtual functions have out-of-line destructors defined by the compiler.
//   Concrete tool objects are owned by ToolRegistry and stored as
//   std::unique_ptr<ITool>; virtual dispatch via the interface pointer is the
//   only dispatch mechanism — no std::function, no type erasure beyond vtable.
//
// Blueprint contract: batbox::tools::ITool (blueprints table row 16616)

#pragma once

#include <batbox/core/Json.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>

#include <string_view>

namespace batbox::tools {

// =============================================================================
// ITool
// =============================================================================

class ITool {
public:
    // -------------------------------------------------------------------------
    // Destructor
    // -------------------------------------------------------------------------
    virtual ~ITool() = default;

    // -------------------------------------------------------------------------
    // Identity — stable across the lifetime of the registry.
    // -------------------------------------------------------------------------

    /// Returns the tool's canonical name (e.g. "read_file", "bash", "task").
    /// The name MUST match the "name" field emitted by schema_json() and the
    /// name the model uses in tool_call objects.
    [[nodiscard]] virtual std::string_view name() const = 0;

    /// Returns a one-sentence human-readable description included in the tool
    /// schema sent to the model.
    [[nodiscard]] virtual std::string_view description() const = 0;

    // -------------------------------------------------------------------------
    // OpenAI tool schema
    // -------------------------------------------------------------------------

    /// Returns the complete OpenAI tools[*].function JSON object describing
    /// this tool.  The returned Json MUST be a JSON object with at minimum:
    ///   {
    ///     "name": "<same as name()>",
    ///     "description": "<same as description()>",
    ///     "parameters": { "type": "object", "properties": {...}, ... }
    ///   }
    /// Implementations may cache and return a const reference or construct on
    /// each call — callers treat the returned value as a value type.
    [[nodiscard]] virtual Json schema_json() const = 0;

    // -------------------------------------------------------------------------
    // Execution
    // -------------------------------------------------------------------------

    /// Execute the tool.
    ///
    /// @param args  Parsed JSON object from the model's tool_call.  Validated
    ///              against the tool's parameter schema by the caller before
    ///              dispatch; implementations may still return ToolResult::error
    ///              for semantic validation failures.
    /// @param ctx   Per-dispatch context (cwd, permission mode, cancel token, …).
    ///
    /// @returns ToolResult::ok(...)   on success.
    ///          ToolResult::error(…)  when the tool encountered a recoverable
    ///                                error (file not found, permission denied,
    ///                                timeout, …).  The error body is fed back
    ///                                to the model so it can self-correct.
    ///
    /// Implementations MUST:
    ///   - Poll ctx.cancel_token.is_cancelled() (or call throw_if_cancelled())
    ///     at every blocking I/O boundary.
    ///   - Return promptly when cancelled (ToolResult::error("cancelled")).
    ///   - Never throw out of run() — catch all exceptions and convert to
    ///     ToolResult::error.
    [[nodiscard]] virtual ToolResult run(const Json& args, ToolContext& ctx) = 0;

    // -------------------------------------------------------------------------
    // Permission gate hooks — both have working defaults; override as needed.
    // -------------------------------------------------------------------------

    /// Returns true for tools that only read state and never mutate it
    /// (Read, Glob, Grep, ToolSearch, CtxInspect, …).
    ///
    /// Plan-mode gate: when the conversation is in Plan mode, the dispatch
    /// layer only allows tools where is_read_only() == true.
    ///
    /// Default: false (conservative — most tools are assumed to have side effects).
    [[nodiscard]] virtual bool is_read_only() const { return false; }

    /// Returns true when a confirmation prompt should be shown before running
    /// this tool (in Default / AcceptEdits modes where prompts are active).
    ///
    /// Permission-gate hint: false means "never show a prompt for this tool"
    /// regardless of the active PermissionMode.  Typical for read-only or
    /// purely informational tools.
    ///
    /// Default: true (conservative — always confirm unless overridden).
    [[nodiscard]] virtual bool requires_confirmation() const { return true; }

    // -------------------------------------------------------------------------
    // Non-copyable, non-movable (tools are owned by ToolRegistry).
    // -------------------------------------------------------------------------
    ITool(const ITool&)            = delete;
    ITool& operator=(const ITool&) = delete;
    ITool(ITool&&)                 = delete;
    ITool& operator=(ITool&&)      = delete;

protected:
    /// Protected default constructor — tools are only constructed by their
    /// concrete implementations.
    ITool() = default;
};

} // namespace batbox::tools
