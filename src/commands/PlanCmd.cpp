// src/commands/PlanCmd.cpp
//
// batbox::commands::PlanCmd — implements the /plan slash command.
//
// Behaviour:
//   /plan            — if Inactive: enter Planning state + confirmation message.
//                      if Planning: show banner and current plan state.
//                      if Approved: show banner, plan_id, and plan_text.
//   /plan status     — print current plan state without changing it.
//   /plan exit       — transition Planning → Inactive (reject active plan).
//
// State machine
// -------------
// /plan calls PlanMode::enter_plan() to transition Inactive → Planning.
// The command does NOT call approve() or advance_turn(); those are reserved
// for EnterPlanModeTool / ExitPlanModeTool which the AI uses during inference
// turns.  /plan is the human-facing entry point that starts plan mode.
//
// When no PlanMode is wired in (headless / test mode) the command reports that
// plan mode is not available in the current context.
//
// Registration entry point:
//   void register_plan_cmd(SlashCommandRegistry&, batbox::conversation::PlanMode*);
//   void register_plan_cmd(SlashCommandRegistry&);   // plan_mode = nullptr

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/conversation/PlanMode.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/commands/CommandHelpers.hpp>
#include <batbox/repl/CommandContext.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace batbox::commands {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

/// Print the current plan mode banner and state summary to `out`.
void print_plan_status(std::ostream& out,
                       const batbox::conversation::PlanMode& pm)
{
    using batbox::conversation::PlanState;
    using batbox::conversation::plan_state_name;

    const std::string_view banner = pm.banner();

    out << '\n';
    if (!banner.empty()) {
        out << "  ╔══════════════════════════════╗\n";
        out << "  ║  " << banner
            << std::string(30 - banner.size() > 0 ? 30 - banner.size() : 0, ' ')
            << "  ║\n";
        out << "  ╚══════════════════════════════╝\n";
    }

    out << "  State: " << plan_state_name(pm.state()) << '\n';

    if (pm.state() == PlanState::Approved) {
        out << "  Plan ID: " << pm.plan_id() << '\n';
        if (!pm.plan_text().empty()) {
            out << '\n';
            out << "  Active plan:\n";
            // Indent each line of the plan text.
            const std::string& text = pm.plan_text();
            std::size_t pos = 0;
            while (pos < text.size()) {
                const auto nl = text.find('\n', pos);
                const std::string_view line =
                    (nl == std::string::npos)
                    ? std::string_view(text).substr(pos)
                    : std::string_view(text).substr(pos, nl - pos);
                out << "    " << line << '\n';
                if (nl == std::string::npos) break;
                pos = nl + 1;
            }
        }
    } else if (pm.state() == PlanState::Planning) {
        out << '\n';
        out << "  Plan mode is active. Write-side tools are blocked.\n";
        out << "  Use ExitPlanMode tool (or /plan exit) to approve or reject the plan.\n";
    } else {
        // Inactive
        out << '\n';
        out << "  Plan mode is not active.\n";
    }
    out << '\n';
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// PlanCmd
// ---------------------------------------------------------------------------

class PlanCmd final : public ISlashCommand {
public:
    /// Construct with an optional live PlanMode pointer.
    /// When plan_mode is nullptr the command runs in headless mode and
    /// reports that plan mode is not available.
    explicit PlanCmd(batbox::conversation::PlanMode* plan_mode = nullptr)
        : plan_mode_(plan_mode) {}

    // ---- Identity -----------------------------------------------------------

    [[nodiscard]] std::string_view name() const noexcept override {
        return "plan";
    }

    [[nodiscard]] std::string_view description() const noexcept override {
        return "Enter plan mode â model asks clarifying questions via AskUserQuestion before drafting the plan.";
    }

    [[nodiscard]] std::string_view usage() const noexcept override {
        return "/plan [prompt] | /plan status | /plan exit";
    }

    [[nodiscard]] std::vector<std::string> aliases() const override {
        return {};
    }

    [[nodiscard]] bool requires_args() const noexcept override { return false; }

    // ---- Execute ------------------------------------------------------------

    [[nodiscard]] batbox::Result<void> execute(
        std::string_view   args,
        CommandContext&    ctx) override;

private:
    batbox::conversation::PlanMode* plan_mode_;  ///< Non-owning; may be null.
};

// ---------------------------------------------------------------------------
// execute
// ---------------------------------------------------------------------------

batbox::Result<void> PlanCmd::execute(
    std::string_view args,
    CommandContext&  ctx)
{
    if (plan_mode_ == nullptr) {
        ctx.output << "  /plan: plan mode runtime not available in this context.\n";
        ctx.output << "  Plan mode requires the full REPL runtime (CPP A.3).\n";
        return {};
    }

    using batbox::conversation::PlanMode;
    using batbox::conversation::PlanState;

    const std::string_view sub = trim(args);

    // /plan status — show current state, no transition.
    if (sub == "status") {
        print_plan_status(ctx.output, *plan_mode_);
        return {};
    }

    // /plan exit — transition Planning → Inactive (human-driven rejection).
    if (sub == "exit") {
        if (plan_mode_->state() == PlanState::Planning) {
            try {
                plan_mode_->reject();
            } catch (const batbox::conversation::PlanModeError& e) {
                return batbox::Err(
                    std::string("/plan exit: ") + e.what());
            }
            ctx.output << "  Plan mode exited. Revise and re-enter with /plan.\n";
        } else if (plan_mode_->state() == PlanState::Approved) {
            return batbox::Err(
                std::string("/plan exit: cannot exit while an approved plan is executing.\n"
                            "  Wait for the plan's turn to complete (advance_turn) "
                            "before exiting."));
        } else {
            ctx.output << "  Plan mode is not active.\n";
        }
        return {};
    }

    // /plan [optional initial prompt]
    // Any non-reserved text after /plan is treated as an initial prompt to forward
    // to the model once plan mode is active.  "status" and "exit" are handled above.
    // The legacy "on" alias is still accepted silently (no forwarding).
    const std::string_view initial_prompt = (sub == "on") ? std::string_view{} : sub;

    // /plan [prompt] — enter plan mode or report current state.
    switch (plan_mode_->state()) {
        case PlanState::Inactive:
            try {
                plan_mode_->enter_plan();
            } catch (const batbox::conversation::PlanModeError& e) {
                return batbox::Err(
                    std::string("/plan: ") + e.what());
            }
            // TUI-PLAN-T4: cards-then-plan confirmation banner.
            ctx.output << "  entered plan mode \xe2\x80\x94 ask away\n";
            ctx.output << "  I will pose multi-choice clarifying questions via AskUserQuestion\n";
            ctx.output << "  before drafting the plan. Write-side tools are blocked\n";
            ctx.output << "  until you approve the plan via ExitPlanMode.\n";
            // Forward the initial prompt so the model starts planning immediately.
            if (!initial_prompt.empty()) {
                ctx.conversation.inject_user_message(initial_prompt);
            }
            break;

        case PlanState::Planning:
            ctx.output << "  Already in plan mode \xe2\x80\x94 ask away\n";
            print_plan_status(ctx.output, *plan_mode_);
            // Forward any additional prompt even when already planning.
            if (!initial_prompt.empty()) {
                ctx.conversation.inject_user_message(initial_prompt);
            }
            break;

        case PlanState::Approved:
            ctx.output << "  An approved plan is currently executing.\n";
            print_plan_status(ctx.output, *plan_mode_);
            break;
    }

    return {};
}

// ---------------------------------------------------------------------------
// Registration functions
// ---------------------------------------------------------------------------

void register_plan_cmd(SlashCommandRegistry&           registry,
                        batbox::conversation::PlanMode* plan_mode)
{
    auto res = registry.register_command(
        std::make_shared<PlanCmd>(plan_mode));
    (void)res;
}

void register_plan_cmd(SlashCommandRegistry& registry) {
    register_plan_cmd(registry, nullptr);
}

} // namespace batbox::commands
