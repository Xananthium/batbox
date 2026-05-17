// src/tools/CronListTool.cpp
//
// Implementation of batbox::tools::CronListTool.
//
// Blueprint contract: CPP 5.17, blueprint id 16697

#include <batbox/tools/CronListTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>

#include <sstream>
#include <string>

namespace batbox::tools {

namespace {

constexpr std::string_view kToolName = "CronList";

} // namespace

// =============================================================================
// CronListTool — constructor
// =============================================================================

CronListTool::CronListTool(std::shared_ptr<CronScheduler> scheduler)
    : scheduler_(std::move(scheduler)) {}

// =============================================================================
// ITool identity
// =============================================================================

std::string_view CronListTool::name() const {
    return kToolName;
}

std::string_view CronListTool::description() const {
    return "List all cron entries stored in ~/.batbox/cron.json, "
           "showing each entry's id, expression, enabled state, and prompt.";
}

// =============================================================================
// schema_json
// =============================================================================

Json CronListTool::schema_json() const {
    return Json{
        {"name",        "CronList"},
        {"description", "List all cron entries stored in ~/.batbox/cron.json, "
                        "showing each entry's id, expression, enabled state, and prompt."},
        {"parameters",  Json{
            {"type",       "object"},
            {"properties", Json::object()},
            {"required",   Json::array()}
        }}
    };
}

// =============================================================================
// run
// =============================================================================

ToolResult CronListTool::run(const Json& /*args*/, ToolContext& ctx) {
    try {
        // ------------------------------------------------------------------
        // 1. Cancellation check.
        // ------------------------------------------------------------------
        if (ctx.is_cancelled()) {
            return ToolResult::error("cancelled");
        }

        // ------------------------------------------------------------------
        // 2. Load entries from scheduler.
        // ------------------------------------------------------------------
        const auto entries = scheduler_->list_entries();

        if (ctx.is_cancelled()) {
            return ToolResult::error("cancelled");
        }

        // ------------------------------------------------------------------
        // 3. Build response.
        // ------------------------------------------------------------------
        if (entries.empty()) {
            Json payload{{"entries", Json::array()}, {"count", 0}};
            return ToolResult::ok(
                "No cron entries. Use CronCreate to schedule repeating tasks.",
                std::move(payload));
        }

        std::ostringstream body;
        body << entries.size() << " cron "
             << (entries.size() == 1 ? "entry" : "entries") << ":\n\n";

        Json entries_json = Json::array();
        for (const auto& e : entries) {
            body << "id:         " << e.id << "\n"
                 << "expression: " << e.expression << "\n"
                 << "enabled:    " << (e.enabled ? "true" : "false") << "\n"
                 << "created_at: " << e.created_at << "\n"
                 << "prompt:     " << e.prompt << "\n"
                 << "\n";
            entries_json.push_back(e.to_json());
        }

        Json payload{{"entries", std::move(entries_json)},
                     {"count",   static_cast<int>(entries.size())}};
        return ToolResult::ok(body.str(), std::move(payload));

    } catch (const std::exception& ex) {
        return ToolResult::error(
            std::string("CronList: unexpected error: ") + ex.what());
    } catch (...) {
        return ToolResult::error("CronList: unknown error.");
    }
}

} // namespace batbox::tools
