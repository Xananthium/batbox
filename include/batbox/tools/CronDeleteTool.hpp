// include/batbox/tools/CronDeleteTool.hpp
//
// batbox::tools::CronDeleteTool — remove a cron entry by id.
//
// Contract (CPP 5.17 blueprint, id 16693):
//
//   Tool name   : "CronDelete"
//   class       : class CronDeleteTool : public ITool
//   Arguments   :
//     id  (string, required) — the UUID of the entry to remove
//
//   Behaviour:
//     1. Validate id (non-empty string).
//     2. Delegate to CronScheduler::delete_entry().
//     3. Return ToolResult::ok if found + deleted; error if not found.
//
//   Permission flags:
//     is_read_only()          == false  (mutates cron.json)
//     requires_confirmation() == false  (user-local store)
//
// Blueprint contract: batbox::tools::CronDeleteTool (CPP 5.17, bp 16693)

#pragma once

#include <batbox/tools/ITool.hpp>
#include <batbox/tools/CronScheduler.hpp>
#include <batbox/core/Json.hpp>

#include <memory>

namespace batbox::tools {

// =============================================================================
// CronDeleteTool
// =============================================================================

class CronDeleteTool final : public ITool {
public:
    /// Construct with an injected CronScheduler.
    explicit CronDeleteTool(std::shared_ptr<CronScheduler> scheduler);

    // -------------------------------------------------------------------------
    // ITool identity
    // -------------------------------------------------------------------------

    /// Returns "CronDelete".
    [[nodiscard]] std::string_view name() const override;

    /// One-sentence description for the model schema.
    [[nodiscard]] std::string_view description() const override;

    // -------------------------------------------------------------------------
    // OpenAI tool schema
    // -------------------------------------------------------------------------

    [[nodiscard]] Json schema_json() const override;

    // -------------------------------------------------------------------------
    // Execution
    // -------------------------------------------------------------------------

    /// Delete a cron entry.
    ///
    /// args["id"] — required UUID string of the entry to remove
    ///
    /// @returns ToolResult::ok(body)   when deleted.
    ///          ToolResult::error(msg) when not found or id invalid.
    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override;

    // -------------------------------------------------------------------------
    // Permission gate hooks
    // -------------------------------------------------------------------------

    /// false — mutates cron.json.
    [[nodiscard]] bool is_read_only() const override { return false; }

    /// false — user-local store; no confirmation prompt needed.
    [[nodiscard]] bool requires_confirmation() const override { return false; }

private:
    std::shared_ptr<CronScheduler> scheduler_;
};

} // namespace batbox::tools
