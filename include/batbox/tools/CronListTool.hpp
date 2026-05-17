// include/batbox/tools/CronListTool.hpp
//
// batbox::tools::CronListTool — list active cron entries.
//
// Contract (CPP 5.17 blueprint, id 16696):
//
//   Tool name   : "CronList"
//   class       : class CronListTool : public ITool
//   Arguments   : (none required)
//
//   Behaviour:
//     1. Delegate to CronScheduler::list_entries().
//     2. Return ToolResult::ok(body) with a human-readable table +
//        a structured_payload JSON array of entry objects.
//     3. Returns "No cron entries" when the list is empty.
//
//   Permission flags:
//     is_read_only()          == true   (reads cron.json only)
//     requires_confirmation() == false  (read-only; no prompt)
//
// Blueprint contract: batbox::tools::CronListTool (CPP 5.17, bp 16696)

#pragma once

#include <batbox/tools/ITool.hpp>
#include <batbox/tools/CronScheduler.hpp>
#include <batbox/core/Json.hpp>

#include <memory>

namespace batbox::tools {

// =============================================================================
// CronListTool
// =============================================================================

class CronListTool final : public ITool {
public:
    /// Construct with an injected CronScheduler.
    explicit CronListTool(std::shared_ptr<CronScheduler> scheduler);

    // -------------------------------------------------------------------------
    // ITool identity
    // -------------------------------------------------------------------------

    /// Returns "CronList".
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

    /// List all cron entries.
    ///
    /// No required arguments.
    ///
    /// @returns ToolResult::ok(body, payload)  always (empty list = ok message).
    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override;

    // -------------------------------------------------------------------------
    // Permission gate hooks
    // -------------------------------------------------------------------------

    /// true — reads cron.json only; no mutations.
    [[nodiscard]] bool is_read_only() const override { return true; }

    /// false — read-only; never requires a prompt.
    [[nodiscard]] bool requires_confirmation() const override { return false; }

private:
    std::shared_ptr<CronScheduler> scheduler_;
};

} // namespace batbox::tools
