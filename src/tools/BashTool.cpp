// src/tools/BashTool.cpp
//
// Implementation of batbox::tools::BashTool.
//
// BashTool is a thin ITool adapter around BashRunner.  All heavy lifting
// (pty/pipes backend selection, env scrubbing, ANSI stripping, watchdog,
// output capping, cancel_token propagation) lives in BashRunner.
//
// Blueprint contract: batbox::tools::BashTool (CPP 5.10)
//   file  : tools/BashTool.hpp  → include/batbox/tools/BashTool.hpp
//   class : batbox::tools::BashTool (row 16646)
//   file  : tools/BashTool.cpp  → src/tools/BashTool.cpp   (row 16647)

#include <batbox/tools/BashTool.hpp>

#include <batbox/core/Json.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>

#include <algorithm>
#include <string>
#include <string_view>

namespace batbox::tools {

// =============================================================================
// Construction
// =============================================================================

BashTool::BashTool(int max_timeout_sec, std::size_t max_output_bytes)
    : max_timeout_sec_(max_timeout_sec)
    , max_output_bytes_(max_output_bytes)
{}

// =============================================================================
// ITool identity
// =============================================================================

std::string_view BashTool::name() const {
    return "Bash";
}

std::string_view BashTool::description() const {
    return "Execute a shell command and return its combined stdout+stderr output. "
           "Commands run in /bin/sh -c with a scrubbed environment. "
           "Use the description parameter to explain what the command does before it is approved.";
}

Json BashTool::schema_json() const {
    return Json{
        {"name",        "Bash"},
        {"description", description()},
        {"parameters",  Json{
            {"type",       "object"},
            {"properties", Json{
                {"command", Json{
                    {"type",        "string"},
                    {"description", "Shell command string passed to /bin/sh -c."}
                }},
                {"timeout", Json{
                    {"type",        "integer"},
                    {"minimum",     0},
                    {"description",
                     "Per-invocation timeout in seconds. "
                     "Capped at the server-configured maximum (BATBOX_BASH_TIMEOUT_SEC). "
                     "0 or absent = use the configured default timeout."}
                }},
                {"description", Json{
                    {"type",        "string"},
                    {"description",
                     "Short human-readable description of what this command does. "
                     "Surfaced in the permission prompt shown to the user before the command runs."}
                }}
            }},
            {"required", Json::array({"command"})}
        }}
    };
}

// =============================================================================
// Execution
// =============================================================================

ToolResult BashTool::run(const Json& args, ToolContext& ctx) {
    // -------------------------------------------------------------------------
    // 0. Fast cancellation check.
    // -------------------------------------------------------------------------
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    // -------------------------------------------------------------------------
    // 1. Plan-mode guard — Bash is never allowed in Plan mode.
    // -------------------------------------------------------------------------
    if (ctx.is_plan_mode()) {
        return ToolResult::error(
            "Bash: command execution is not permitted in Plan mode. "
            "Use read-only tools (Read, Glob, Grep) to gather information.");
    }

    // -------------------------------------------------------------------------
    // 2. Extract and validate args["command"] (required).
    // -------------------------------------------------------------------------
    if (!args.contains("command") || !args["command"].is_string()) {
        return ToolResult::error(
            "Bash: required argument 'command' is missing or not a string.");
    }
    const std::string command = args["command"].get<std::string>();
    if (command.empty()) {
        return ToolResult::error("Bash: 'command' must not be empty.");
    }

    // -------------------------------------------------------------------------
    // 3. Extract optional args["timeout"] and clamp to [0, max_timeout_sec_].
    //    0 passed to BashRunner means "no timeout" — we use the configured
    //    default (max_timeout_sec_) when the caller doesn't specify one.
    // -------------------------------------------------------------------------
    int effective_timeout = max_timeout_sec_;

    if (args.contains("timeout") && !args["timeout"].is_null()) {
        if (!args["timeout"].is_number_integer()) {
            return ToolResult::error(
                "Bash: 'timeout' must be a non-negative integer (seconds).");
        }
        const auto requested = args["timeout"].get<long long>();
        if (requested < 0) {
            return ToolResult::error("Bash: 'timeout' must be >= 0.");
        }
        // 0 means "use default" — honour up to the configured ceiling.
        if (requested == 0) {
            effective_timeout = max_timeout_sec_;
        } else {
            // Cap the caller's value at our hard ceiling.
            effective_timeout = static_cast<int>(
                std::min(static_cast<long long>(max_timeout_sec_), requested));
        }
    }

    // -------------------------------------------------------------------------
    // 4. Extract optional args["description"] for the PermissionCard.
    // -------------------------------------------------------------------------
    std::string description_text;
    if (args.contains("description") && args["description"].is_string()) {
        description_text = args["description"].get<std::string>();
    }

    // -------------------------------------------------------------------------
    // 5. Cancellation check before blocking call.
    // -------------------------------------------------------------------------
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    // -------------------------------------------------------------------------
    // 6. Delegate to BashRunner.
    //
    //    BashRunner handles:
    //      - Backend selection (pty with forkpty → pipes fallback)
    //      - Environment scrubbing via EnvScrub
    //      - ANSI stripping of output (already done in PtyBackend/PipesBackend)
    //      - Watchdog: SIGTERM at effective_timeout, SIGKILL 2s later
    //      - Output cap at max_output_bytes_
    //      - CancelToken → SIGINT to child process group
    //
    //    We pass an empty env_allowlist so BashRunner uses its default set
    //    (PATH, HOME, USER, LOGNAME, LANG, TERM, SHELL) while stripping
    //    secrets (BATBOX_API_KEY, ANTHROPIC_API_KEY, OPENAI_API_KEY, etc.).
    // -------------------------------------------------------------------------
    const std::vector<std::string> env_allowlist;  // empty = use BashRunner defaults
    bash::BashResult result = runner_.run(
        command,
        ctx.cwd,
        env_allowlist,
        effective_timeout,
        max_output_bytes_,
        ctx.cancel_token,
        /*plan_mode=*/false   // already checked above
    );

    // -------------------------------------------------------------------------
    // 7. Build structured_payload for TUI consumers (PermissionCard, diff cards).
    //    Fields:
    //      exit_code    — raw exit status from the child
    //      duration_ms  — wall-clock duration of the run
    //      description  — caller-provided label (may be empty)
    //      command      — the command that was run (for display)
    // -------------------------------------------------------------------------
    Json payload = Json{
        {"exit_code",   result.exit_code},
        {"duration_ms", result.duration.count()},
        {"description", description_text},
        {"command",     command}
    };

    // -------------------------------------------------------------------------
    // 8. Wrap BashResult → ToolResult.
    //    BashResult.body already contains ANSI-stripped output plus the
    //    "[exit=N, duration=Xms]" trailer appended by BashRunner.
    // -------------------------------------------------------------------------
    if (result.is_error) {
        return ToolResult::error(std::move(result.body), std::move(payload));
    }
    return ToolResult::ok(std::move(result.body), std::move(payload));
}

} // namespace batbox::tools
