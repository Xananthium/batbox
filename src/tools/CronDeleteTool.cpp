// src/tools/CronDeleteTool.cpp
//
// Implementation of batbox::tools::CronDeleteTool.
//
// Blueprint contract: CPP 5.17, blueprint id 16694

#include <batbox/tools/CronDeleteTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>

#include <string>

namespace batbox::tools {

namespace {

constexpr std::string_view kToolName = "CronDelete";

} // namespace

// =============================================================================
// CronDeleteTool — constructor
// =============================================================================

CronDeleteTool::CronDeleteTool(std::shared_ptr<CronScheduler> scheduler)
    : scheduler_(std::move(scheduler)) {}

// =============================================================================
// ITool identity
// =============================================================================

std::string_view CronDeleteTool::name() const {
    return kToolName;
}

std::string_view CronDeleteTool::description() const {
    return "Remove a cron entry by its id; the entry is deleted from "
           "~/.batbox/cron.json and will no longer be scheduled.";
}

// =============================================================================
// schema_json
// =============================================================================

Json CronDeleteTool::schema_json() const {
    return Json{
        {"name",        "CronDelete"},
        {"description", "Remove a cron entry by its id; the entry is deleted from "
                        "~/.batbox/cron.json and will no longer be scheduled."},
        {"parameters",  Json{
            {"type",       "object"},
            {"properties", Json{
                {"id", Json{
                    {"type",        "string"},
                    {"description", "The UUID of the cron entry to remove. "
                                    "Use CronList to find the id of an existing entry."}
                }}
            }},
            {"required", Json::array({"id"})}
        }}
    };
}

// =============================================================================
// run
// =============================================================================

ToolResult CronDeleteTool::run(const Json& args, ToolContext& ctx) {
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
                "CronDelete: not allowed in Plan mode (mutates cron.json).");
        }

        // ------------------------------------------------------------------
        // 3. Extract and validate 'id'.
        // ------------------------------------------------------------------
        const auto id_it = args.find("id");
        if (id_it == args.end() || !id_it->is_string()) {
            return ToolResult::error(
                "CronDelete: required argument 'id' is missing or not a string.");
        }
        const std::string id = id_it->get<std::string>();

        if (id.empty()) {
            return ToolResult::error(
                "CronDelete: 'id' must not be empty.");
        }

        // ------------------------------------------------------------------
        // 4. Check cancellation before I/O.
        // ------------------------------------------------------------------
        if (ctx.is_cancelled()) {
            return ToolResult::error("cancelled");
        }

        // ------------------------------------------------------------------
        // 5. Delegate to scheduler.
        // ------------------------------------------------------------------
        const bool removed = scheduler_->delete_entry(id);
        if (!removed) {
            return ToolResult::error(
                "CronDelete: no cron entry with id '" + id + "' was found.");
        }

        Json payload{{"id", id}, {"deleted", true}};
        return ToolResult::ok(
            "Deleted cron entry with id '" + id + "'.",
            std::move(payload));

    } catch (const std::exception& ex) {
        return ToolResult::error(
            std::string("CronDelete: unexpected error: ") + ex.what());
    } catch (...) {
        return ToolResult::error("CronDelete: unknown error.");
    }
}

} // namespace batbox::tools
