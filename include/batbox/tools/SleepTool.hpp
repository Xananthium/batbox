// include/batbox/tools/SleepTool.hpp
//
// batbox::tools::SleepTool — sleep for a requested number of seconds with
// cooperative cancellation support via ToolContext::cancel_token.
//
// Contract (CPP 5.21 blueprint):
//   name()            = "Sleep"
//   is_read_only()    = true
//   requires_confirmation() = false
//   args              = { "seconds": <number> }   — capped at 300
//   on success        — ToolResult::ok("slept N seconds")
//   on cancellation   — ToolResult::ok("(cancelled)")
//   on bad args       — ToolResult::error(...)
//
// Implementation note:
//   The sleep is interruptible via std::condition_variable notified by an
//   on_cancel callback registered on ctx.cancel_token.  The wait_for
//   predicate re-checks cancellation so spurious wake-ups are handled.
//
// Blueprint contract: batbox::tools::SleepTool (CPP 5.21)

#pragma once

#include <batbox/tools/ITool.hpp>
#include <batbox/core/Json.hpp>

namespace batbox::tools {

// =============================================================================
// SleepTool
// =============================================================================

class SleepTool final : public ITool {
public:
    SleepTool() = default;

    // -------------------------------------------------------------------------
    // ITool interface
    // -------------------------------------------------------------------------

    /// Returns "Sleep".
    [[nodiscard]] std::string_view name() const override;

    /// Returns a one-sentence description for the model.
    [[nodiscard]] std::string_view description() const override;

    /// Returns the OpenAI tool schema for the "seconds" parameter.
    [[nodiscard]] Json schema_json() const override;

    /// Sleeps for the requested number of seconds (max 300).
    /// Wakes early and returns "(cancelled)" if ctx.cancel_token fires.
    /// Returns "slept N seconds" on normal completion.
    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override;

    /// Returns true — Sleep never mutates state.
    [[nodiscard]] bool is_read_only() const override { return true; }

    /// Returns false — Sleep never requires a confirmation prompt.
    [[nodiscard]] bool requires_confirmation() const override { return false; }
};

} // namespace batbox::tools
