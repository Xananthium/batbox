// include/batbox/tools/BashTool.hpp
//
// batbox::tools::BashTool — ITool wrapper around batbox::tools::bash::BashRunner.
//
// Contract (blueprints table rows 16645-16647, task CPP 5.10):
//
//   Tool name            : "Bash"
//   is_read_only()       : false  (may execute arbitrary commands)
//   requires_confirmation(): true  (always prompts in Default/AcceptEdits modes)
//
// JSON arguments accepted by run():
//
//   command    (string, required)  — shell command string passed to /bin/sh -c.
//   timeout    (integer, optional) — per-invocation timeout in seconds; capped at
//                                    the configured BATBOX_BASH_TIMEOUT_SEC maximum.
//                                    0 or absent = use the configured default.
//   description (string, optional) — short human-readable label for the
//                                    PermissionCard shown in the TUI before the
//                                    user grants permission. Surfaced in the
//                                    structured_payload so the dispatch layer can
//                                    display it without parsing the command string.
//
// Execution behaviour:
//   1. Refuses in Plan mode (ctx.is_plan_mode()) — returns ToolResult::error.
//   2. Extracts command / timeout / description from args.
//   3. Clamps the effective timeout to [0, max_timeout_sec_].
//   4. Delegates to BashRunner::run() which handles pty/pipes selection, env
//      scrubbing, output capping, SIGTERM/SIGKILL watchdog, and cancel_token
//      propagation.
//   5. BashRunner already ANSI-strips its output body; BashTool does not need
//      to strip again.
//   6. Wraps BashResult into ToolResult (ok/error based on is_error).
//   7. Attaches a structured_payload JSON object carrying:
//        { "exit_code": N, "duration_ms": N, "description": "..." }
//
// Constructor:
//   BashTool accepts optional capacity constants that mirror Config::tools:
//     max_timeout_sec       — BATBOX_BASH_TIMEOUT_SEC    (default 120)
//     max_output_bytes      — BATBOX_BASH_MAX_OUTPUT_BYTES (default 1 MiB)
//   Both are provided by the App layer when constructing the tool registry so
//   that live config values flow in without BashTool depending on Config directly.
//
// Blueprint contract: batbox::tools::BashTool (blueprints table rows 16645-16647)

#pragma once

#include <batbox/tools/ITool.hpp>
#include <batbox/tools/bash/BashRunner.hpp>

#include <cstddef>

namespace batbox::tools {

// =============================================================================
// BashTool
// =============================================================================

/// ITool implementation for the "Bash" tool.
///
/// Wraps BashRunner (forkpty/pipes backend) with ITool identity, JSON schema,
/// permission gates, and ToolResult packaging.
class BashTool final : public ITool {
public:
    // -------------------------------------------------------------------------
    // Construction
    //
    // max_timeout_sec    — hard ceiling on args["timeout"]; clamps any larger
    //                      value. Defaults to BATBOX_BASH_TIMEOUT_SEC (120 s).
    // max_output_bytes   — byte cap passed to BashRunner; defaults to
    //                      BATBOX_BASH_MAX_OUTPUT_BYTES (1 MiB).
    // -------------------------------------------------------------------------
    explicit BashTool(int         max_timeout_sec   = kDefaultTimeoutSec,
                      std::size_t max_output_bytes  = kDefaultMaxOutputBytes);

    // -------------------------------------------------------------------------
    // ITool identity
    // -------------------------------------------------------------------------

    /// Returns "Bash" — the stable tool name registered in ToolRegistry.
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

    /// Execute args["command"] via BashRunner.
    ///
    /// Steps:
    ///   1. Refuse if ctx.is_plan_mode().
    ///   2. Validate args["command"] (required string).
    ///   3. Read optional args["timeout"] and clamp to [0, max_timeout_sec_].
    ///   4. Read optional args["description"] for the PermissionCard payload.
    ///   5. Delegate to BashRunner::run().
    ///   6. Wrap BashResult → ToolResult with structured_payload.
    ///
    /// @returns ToolResult::ok(body)   when exit_code == 0.
    ///          ToolResult::error(body) when exit_code != 0 or cancelled.
    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override;

    // -------------------------------------------------------------------------
    // Permission gates
    // -------------------------------------------------------------------------

    /// false — BashTool executes arbitrary shell commands.
    [[nodiscard]] bool is_read_only() const override { return false; }

    /// true — a confirmation prompt is shown before execution in Default /
    ///        AcceptEdits modes.
    [[nodiscard]] bool requires_confirmation() const override { return true; }

    // -------------------------------------------------------------------------
    // Public constants — exposed for tests to reference.
    // -------------------------------------------------------------------------

    /// Default per-invocation timeout (seconds).  Mirrors BATBOX_BASH_TIMEOUT_SEC.
    static constexpr int kDefaultTimeoutSec = 120;

    /// Default maximum output bytes (1 MiB).  Mirrors BATBOX_BASH_MAX_OUTPUT_BYTES.
    static constexpr std::size_t kDefaultMaxOutputBytes = 1'048'576;

private:
    int         max_timeout_sec_;    ///< ceiling for args["timeout"]
    std::size_t max_output_bytes_;   ///< ceiling for BashRunner output
    bash::BashRunner runner_;        ///< stateless executor (no movable state)
};

} // namespace batbox::tools
