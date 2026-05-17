// src/tools/ExitPlanModeTool.cpp
//
// Implementation of batbox::tools::ExitPlanModeTool.
//
// User approval flow:
//   1. Validate that PlanMode is in Planning state.
//   2. Extract the "plan" string argument.
//   3. Present the plan to the user (TUI callback or stdin fallback).
//   4a. Approved → plan_mode_.approve(plan_text) → Approved state.
//   4b. Rejected → plan_mode_.reject()           → Inactive state.
//
// Blueprint contract: batbox::tools::ExitPlanModeTool (task CPP 5.18)

#include <batbox/tools/ExitPlanModeTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>
#include <batbox/conversation/PlanMode.hpp>
#include <batbox/core/Json.hpp>

#include <iostream>
#include <string>

// POSIX isatty
#include <unistd.h>

namespace batbox::tools {

// =============================================================================
// Construction
// =============================================================================

ExitPlanModeTool::ExitPlanModeTool(batbox::conversation::PlanMode& plan_mode)
    : plan_mode_(plan_mode)
    , confirm_fn_(nullptr)
{}

ExitPlanModeTool::ExitPlanModeTool(batbox::conversation::PlanMode& plan_mode,
                                   ConfirmFn confirm_fn)
    : plan_mode_(plan_mode)
    , confirm_fn_(std::move(confirm_fn))
{}

// =============================================================================
// ITool identity
// =============================================================================

std::string_view ExitPlanModeTool::name() const {
    return "ExitPlanMode";
}

std::string_view ExitPlanModeTool::description() const {
    return "Presents the assembled plan to the user for approval; on approval "
           "transitions to Approved state enabling write-side tools for the next "
           "turn, on rejection returns to Inactive so the plan can be revised.";
}

// =============================================================================
// OpenAI tool schema
// =============================================================================

Json ExitPlanModeTool::schema_json() const {
    return Json{
        {"name",        "ExitPlanMode"},
        {"description", "Presents the assembled plan to the user for approval; on approval "
                        "transitions to Approved state enabling write-side tools for the next "
                        "turn, on rejection returns to Inactive so the plan can be revised."},
        {"parameters",  Json{
            {"type",       "object"},
            {"properties", Json{
                {"plan", Json{
                    {"type",        "string"},
                    {"description", "The full plan text to present to the user for review. "
                                    "Markdown formatting is accepted and displayed as-is."}
                }}
            }},
            {"required",   Json::array({"plan"})}
        }}
    };
}

// =============================================================================
// Private helpers
// =============================================================================

// static
bool ExitPlanModeTool::prompt_via_stdin(const std::string& plan_text) {
    std::cout << "\n========== PLAN FOR REVIEW ==========\n"
              << plan_text
              << "\n=====================================\n"
              << "Approve this plan? [y/N] " << std::flush;

    std::string line;
    if (!std::getline(std::cin, line)) {
        // EOF / stream error → treat as rejection
        return false;
    }

    // Trim leading whitespace
    std::size_t start = line.find_first_not_of(" \t\r\n");
    if (start != std::string::npos) {
        line = line.substr(start);
    } else {
        line.clear();
    }

    // Accept 'y' or 'Y' as approval; anything else is rejection.
    return !line.empty() && (line[0] == 'y' || line[0] == 'Y');
}

// =============================================================================
// Execution
// =============================================================================

ToolResult ExitPlanModeTool::run(const Json& args, ToolContext& ctx) {
    // --- Cancellation check ---
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    // --- State validation ---
    const auto current_state = plan_mode_.state();

    if (current_state == batbox::conversation::PlanState::Inactive) {
        return ToolResult::error(
            "ExitPlanMode: not in Planning state (currently Inactive). "
            "Call EnterPlanMode first.");
    }

    if (current_state == batbox::conversation::PlanState::Approved) {
        return ToolResult::error(
            "ExitPlanMode: not in Planning state (currently Approved). "
            "The plan is already approved; advance the turn to proceed.");
    }

    // --- Validate arguments ---
    if (!args.contains("plan") || !args["plan"].is_string()) {
        return ToolResult::error("ExitPlanMode: missing required argument 'plan' (string).");
    }

    const std::string plan_text = args["plan"].get<std::string>();

    if (plan_text.empty()) {
        return ToolResult::error("ExitPlanMode: 'plan' argument must not be empty.");
    }

    // --- Headless mode guard (only applies if no TUI callback is wired) ---
    if (!confirm_fn_ && !::isatty(STDIN_FILENO)) {
        return ToolResult::error(
            "ExitPlanMode: not available in headless mode without a wired confirm callback.");
    }

    // --- Poll cancellation before blocking user interaction ---
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    // --- Obtain user decision ---
    bool approved = false;
    if (confirm_fn_) {
        // TUI / test path
        approved = confirm_fn_(plan_text);
    } else {
        // Terminal stdin path
        approved = prompt_via_stdin(plan_text);
    }

    // --- Apply state transition ---
    if (approved) {
        std::uint32_t plan_id = 0;
        try {
            plan_id = plan_mode_.approve(plan_text);
        } catch (const batbox::conversation::PlanModeError& e) {
            return ToolResult::error(
                std::string("ExitPlanMode: approve transition failed: ") + e.what());
        } catch (...) {
            return ToolResult::error("ExitPlanMode: unexpected error during approve transition.");
        }

        Json payload{
            {"plan_id",   plan_id},
            {"plan_text", plan_text}
        };

        const std::string body =
            "Plan approved (plan_id=" + std::to_string(plan_id) +
            "). Write-side tools are re-enabled for the next execution turn.";

        return ToolResult::ok(body, std::move(payload));

    } else {
        // User rejected the plan.
        try {
            plan_mode_.reject();
        } catch (const batbox::conversation::PlanModeError& e) {
            return ToolResult::error(
                std::string("ExitPlanMode: reject transition failed: ") + e.what());
        } catch (...) {
            return ToolResult::error("ExitPlanMode: unexpected error during reject transition.");
        }

        return ToolResult::ok(
            "Plan rejected by user. Revise the plan and call ExitPlanMode again "
            "with an updated plan.");
    }
}

} // namespace batbox::tools
