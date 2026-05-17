// src/tools/CronCreateTool.cpp
//
// Implementation of batbox::tools::CronCreateTool.
//
// Blueprint contract: CPP 5.17, blueprint id 16691

#include <batbox/tools/CronCreateTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>

#include <string>

namespace batbox::tools {

namespace {

constexpr std::string_view kToolName = "CronCreate";

} // namespace

// =============================================================================
// CronCreateTool — constructor
// =============================================================================

CronCreateTool::CronCreateTool(std::shared_ptr<CronScheduler> scheduler)
    : scheduler_(std::move(scheduler)) {}

// =============================================================================
// ITool identity
// =============================================================================

std::string_view CronCreateTool::name() const {
    return kToolName;
}

std::string_view CronCreateTool::description() const {
    return "Schedule a repeating cron job: provide a 5-field cron expression "
           "(\"MIN HOUR DOM MON DOW\") and a prompt text to dispatch when due; "
           "entries are persisted to ~/.batbox/cron.json.";
}

// =============================================================================
// schema_json
// =============================================================================

Json CronCreateTool::schema_json() const {
    return Json{
        {"name",        "CronCreate"},
        {"description", "Schedule a repeating cron job: provide a 5-field cron expression "
                        "(\"MIN HOUR DOM MON DOW\") and a prompt text to dispatch when due; "
                        "entries are persisted to ~/.batbox/cron.json."},
        {"parameters",  Json{
            {"type",       "object"},
            {"properties", Json{
                {"expression", Json{
                    {"type",        "string"},
                    {"description", "5-field cron expression: \"MIN HOUR DOM MON DOW\". "
                                    "Each field may be *, an integer, a comma-separated list, "
                                    "a range (A-B), or a step (*/N or A-B/N). "
                                    "Example: \"0 9 * * 1-5\" runs at 09:00 on weekdays."}
                }},
                {"prompt", Json{
                    {"type",        "string"},
                    {"description", "The prompt or command text to dispatch when this entry fires."}
                }},
                {"enabled", Json{
                    {"type",        "boolean"},
                    {"description", "Whether the entry is active. Default true. "
                                    "Set false to persist without scheduling."},
                    {"default",     true}
                }}
            }},
            {"required", Json::array({"expression", "prompt"})}
        }}
    };
}

// =============================================================================
// run
// =============================================================================

ToolResult CronCreateTool::run(const Json& args, ToolContext& ctx) {
    try {
        // ------------------------------------------------------------------
        // 1. Cancellation check.
        // ------------------------------------------------------------------
        if (ctx.is_cancelled()) {
            return ToolResult::error("cancelled");
        }

        // ------------------------------------------------------------------
        // 2. Plan-mode gate.
        // ------------------------------------------------------------------
        if (ctx.is_plan_mode()) {
            return ToolResult::error(
                "CronCreate: not allowed in Plan mode (creates a persistent entry).");
        }

        // ------------------------------------------------------------------
        // 3. Extract and validate 'expression'.
        // ------------------------------------------------------------------
        const auto expr_it = args.find("expression");
        if (expr_it == args.end() || !expr_it->is_string()) {
            return ToolResult::error(
                "CronCreate: required argument 'expression' is missing or not a string.");
        }
        const std::string expression = expr_it->get<std::string>();

        if (expression.empty()) {
            return ToolResult::error(
                "CronCreate: 'expression' must not be empty.");
        }

        // ------------------------------------------------------------------
        // 4. Extract and validate 'prompt'.
        // ------------------------------------------------------------------
        const auto prompt_it = args.find("prompt");
        if (prompt_it == args.end() || !prompt_it->is_string()) {
            return ToolResult::error(
                "CronCreate: required argument 'prompt' is missing or not a string.");
        }
        const std::string prompt = prompt_it->get<std::string>();

        if (prompt.empty()) {
            return ToolResult::error(
                "CronCreate: 'prompt' must not be empty.");
        }

        // ------------------------------------------------------------------
        // 5. Extract optional 'enabled' (default true).
        // ------------------------------------------------------------------
        bool enabled = true;
        {
            const auto en_it = args.find("enabled");
            if (en_it != args.end()) {
                if (!en_it->is_boolean()) {
                    return ToolResult::error(
                        "CronCreate: 'enabled' must be a boolean when provided.");
                }
                enabled = en_it->get<bool>();
            }
        }

        // ------------------------------------------------------------------
        // 6. Check cancellation before I/O.
        // ------------------------------------------------------------------
        if (ctx.is_cancelled()) {
            return ToolResult::error("cancelled");
        }

        // ------------------------------------------------------------------
        // 7. Delegate to scheduler.
        // ------------------------------------------------------------------
        auto opt_entry = scheduler_->create_entry(expression, prompt, enabled);
        if (!opt_entry.has_value()) {
            // Expression may be invalid or I/O failure.
            return ToolResult::error(
                "CronCreate: failed to create entry. "
                "Check that 'expression' is a valid 5-field cron string "
                "(e.g. \"0 9 * * 1-5\") and that ~/.batbox/ is writable.");
        }

        const auto& entry = *opt_entry;
        const std::string body =
            "Created cron entry:\n"
            "  id:         " + entry.id + "\n"
            "  expression: " + entry.expression + "\n"
            "  prompt:     " + entry.prompt + "\n"
            "  enabled:    " + (entry.enabled ? "true" : "false") + "\n"
            "  created_at: " + entry.created_at;

        Json payload = entry.to_json();
        return ToolResult::ok(body, std::move(payload));

    } catch (const std::exception& ex) {
        return ToolResult::error(
            std::string("CronCreate: unexpected error: ") + ex.what());
    } catch (...) {
        return ToolResult::error("CronCreate: unknown error.");
    }
}

} // namespace batbox::tools
