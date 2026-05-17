// src/tools/PowerShellTool.cpp
//
// batbox::tools::PowerShellTool — implementation.
//
// Platform behaviour:
//   All platforms currently supported by batbox (macOS, Linux) do not have
//   PowerShell in the standard toolchain.  run() returns an informative error
//   so the model can self-correct by switching to BashTool.
//
//   The #ifdef _WIN32 block is present to mark the future Windows execution
//   path.  The Windows path returns the same platform error today because
//   actual WinAPI-based PowerShell execution is deferred to a later task.
//   When that task lands it will replace the body of the _WIN32 branch only;
//   the non-Windows branch is final.
//
// Blueprint contract: PowerShellTool::run (blueprints table row 16655)

#include <batbox/tools/PowerShellTool.hpp>

namespace batbox::tools {

// ---------------------------------------------------------------------------
// Error message — single source of truth used by both branches.
// ---------------------------------------------------------------------------

static constexpr std::string_view kPlatformError =
    "PowerShell is not supported on this platform. Use Bash.";

// ---------------------------------------------------------------------------
// ITool identity
// ---------------------------------------------------------------------------

std::string_view PowerShellTool::name() const {
    return "PowerShell";
}

std::string_view PowerShellTool::description() const {
    return "Execute a PowerShell command. "
           "Not available on macOS or Linux — use Bash on those platforms.";
}

// ---------------------------------------------------------------------------
// OpenAI tool schema — mirrors BashTool parameter set
// ---------------------------------------------------------------------------

Json PowerShellTool::schema_json() const {
    return Json{
        {"name",        "PowerShell"},
        {"description", "Execute a PowerShell command. "
                        "Not available on macOS or Linux — use Bash on those platforms."},
        {"parameters",  Json{
            {"type",       "object"},
            {"properties", Json{
                {"command", Json{
                    {"type",        "string"},
                    {"description", "The PowerShell command to execute."}
                }},
                {"timeout", Json{
                    {"type",        "integer"},
                    {"description", "Maximum execution time in milliseconds. "
                                    "Defaults to 120 000 (2 min); max 600 000 (10 min)."},
                    {"minimum",     1},
                    {"maximum",     600000}
                }},
                {"description", Json{
                    {"type",        "string"},
                    {"description", "Short human-readable summary shown in the "
                                    "permission card and agent log."}
                }}
            }},
            {"required", Json::array({"command"})}
        }}
    };
}

// ---------------------------------------------------------------------------
// run() — platform dispatch
// ---------------------------------------------------------------------------

ToolResult PowerShellTool::run(const Json& /*args*/, ToolContext& /*ctx*/) {
#ifdef _WIN32
    // Windows execution path is reserved for a future task.
    // When Windows support is added this branch will invoke PowerShell via
    // WinAPI (CreateProcess / pwsh.exe).  Until then, Windows callers receive
    // the same clear error so behaviour is uniform across all platforms.
    return ToolResult::error(std::string(kPlatformError));
#else
    // macOS and Linux: PowerShell is not available.  Return immediately with
    // a descriptive error the model can act on (switch to BashTool).
    return ToolResult::error(std::string(kPlatformError));
#endif
}

} // namespace batbox::tools
