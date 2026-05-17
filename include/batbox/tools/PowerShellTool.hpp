// include/batbox/tools/PowerShellTool.hpp
//
// batbox::tools::PowerShellTool — PowerShell tool, no-op on macOS/Linux.
//
// Contract (blueprints table rows 16653–16654, task CPP 5.11):
//
//   Tool name     : "PowerShell"
//   Schema        : mirrors Bash schema (command, timeout, description)
//
//   Behaviour on macOS/Linux (all non-Windows builds):
//     run() immediately returns:
//       ToolResult::error("PowerShell is not supported on this platform. Use Bash.")
//     No process is spawned.  The schema is still registered in the ToolRegistry
//     so the model is aware the tool exists and receives the error as corrective
//     feedback rather than a missing-tool confusion.
//
//   Behaviour on Windows (future target, #ifdef _WIN32):
//     Returns the same platform-error today.  A future task will wire up actual
//     PowerShell execution via WinAPI; until then the same error body is returned
//     so the model behaves identically on all current target platforms.
//
//   Permission gates (same as BashTool, conservative defaults):
//     is_read_only()          == false  (execution tool)
//     requires_confirmation() == true   (shell execution — always prompt)
//
// Blueprint contract: batbox::tools::PowerShellTool (blueprints table row 16654)

#pragma once

#include <batbox/tools/ITool.hpp>
#include <batbox/core/Json.hpp>

namespace batbox::tools {

// =============================================================================
// PowerShellTool
// =============================================================================

/// Implements the "PowerShell" tool.
///
/// On macOS and Linux (all currently supported platforms) run() returns an
/// informative error directing the model to use Bash instead.  The schema is
/// fully declared so the model always knows the tool is in the registry; the
/// error body is the model's cue to switch tools.
///
/// On Windows (future target) the implementation is gated by #ifdef _WIN32;
/// until actual Windows execution is wired up it returns the same error.
class PowerShellTool final : public ITool {
public:
    PowerShellTool() = default;

    // -------------------------------------------------------------------------
    // ITool identity
    // -------------------------------------------------------------------------

    /// Returns "PowerShell" — the stable tool name registered in the ToolRegistry.
    [[nodiscard]] std::string_view name() const override;

    /// Returns a one-sentence description surfaced to the model in the schema.
    [[nodiscard]] std::string_view description() const override;

    // -------------------------------------------------------------------------
    // OpenAI tool schema
    // -------------------------------------------------------------------------

    /// Returns the full OpenAI tools[*].function JSON object for this tool.
    /// Schema mirrors BashTool: command (required), timeout (optional, ms),
    /// description (optional, human-readable summary).
    [[nodiscard]] Json schema_json() const override;

    // -------------------------------------------------------------------------
    // Execution
    // -------------------------------------------------------------------------

    /// Execute a PowerShell command.
    ///
    /// On macOS / Linux: immediately returns
    ///   ToolResult::error("PowerShell is not supported on this platform. Use Bash.")
    ///
    /// No process is spawned on any currently supported platform.
    ///
    /// @param args  Parsed JSON object from the model's tool_call (not inspected
    ///              on unsupported platforms).
    /// @param ctx   Per-dispatch context (not used on unsupported platforms).
    ///
    /// @returns ToolResult::error with a clear platform-not-supported message.
    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override;

    // -------------------------------------------------------------------------
    // Permission gate hooks
    // -------------------------------------------------------------------------

    /// false — PowerShellTool is an execution tool that may have side effects.
    [[nodiscard]] bool is_read_only() const override { return false; }

    /// true — shell execution always requires a confirmation prompt.
    [[nodiscard]] bool requires_confirmation() const override { return true; }
};

} // namespace batbox::tools
