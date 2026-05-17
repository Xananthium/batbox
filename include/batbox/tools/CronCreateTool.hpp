// include/batbox/tools/CronCreateTool.hpp
//
// batbox::tools::CronCreateTool — schedule a repeating cron job.
//
// Contract (CPP 5.17 blueprint, id 16690):
//
//   Tool name   : "CronCreate"
//   class       : class CronCreateTool : public ITool
//   Arguments   :
//     expression  (string, required) — standard 5-field cron expression
//     prompt      (string, required) — prompt/command text to fire
//     enabled     (bool,   optional) — default true
//
//   Behaviour:
//     1. Validate expression via CronExpr::parse().
//     2. Delegate to CronScheduler::create_entry().
//     3. Return ToolResult::ok(body) where body summarises the created entry.
//     4. Return ToolResult::error(msg) on validation or persistence failure.
//
//   Permission flags:
//     is_read_only()          == false  (persists to cron.json)
//     requires_confirmation() == false  (user-local store)
//
// Blueprint contract: batbox::tools::CronCreateTool (CPP 5.17, bp 16690)

#pragma once

#include <batbox/tools/ITool.hpp>
#include <batbox/tools/CronScheduler.hpp>
#include <batbox/core/Json.hpp>

#include <memory>

namespace batbox::tools {

// =============================================================================
// CronCreateTool
// =============================================================================

class CronCreateTool final : public ITool {
public:
    /// Construct with an injected CronScheduler.
    explicit CronCreateTool(std::shared_ptr<CronScheduler> scheduler);

    // -------------------------------------------------------------------------
    // ITool identity
    // -------------------------------------------------------------------------

    /// Returns "CronCreate".
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

    /// Create a cron entry.
    ///
    /// args["expression"] — required 5-field cron string
    /// args["prompt"]     — required prompt/command text
    /// args["enabled"]    — optional bool (default true)
    ///
    /// @returns ToolResult::ok(body)   on success.
    ///          ToolResult::error(msg) on validation or store failure.
    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override;

    // -------------------------------------------------------------------------
    // Permission gate hooks
    // -------------------------------------------------------------------------

    /// false — persists to cron.json.
    [[nodiscard]] bool is_read_only() const override { return false; }

    /// false — user-local store; no confirmation prompt needed.
    [[nodiscard]] bool requires_confirmation() const override { return false; }

private:
    std::shared_ptr<CronScheduler> scheduler_;
};

} // namespace batbox::tools
