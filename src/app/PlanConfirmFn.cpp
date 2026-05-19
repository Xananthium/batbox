// src/app/PlanConfirmFn.cpp
// =============================================================================
// make_plan_confirm_fn implementation (PEXT3 1.5)
// =============================================================================

#include <batbox/app/PlanConfirmFn.hpp>
#include <batbox/core/Logging.hpp>
#include <batbox/tui/Events.hpp>

namespace batbox::app {

batbox::tools::ExitPlanModeTool::ConfirmFn
make_plan_confirm_fn(bool nuclear,
                     std::shared_ptr<batbox::tui::PlanApprovalCard> plan_approval_card,
                     batbox::tui::ScreenManager& screen_mgr)
{
    if (nuclear) {
        // Nuclear: auto-approve immediately.  No modal, no event posted.
        return [](const std::string& /*plan_text*/) -> bool {
            BATBOX_LOG_INFO("nuclear: plan auto-approved without modal");
            return true;
        };
    }

    // Normal TUI path: post the show event, block on user decision, hide.
    return [plan_approval_card, &screen_mgr](const std::string& plan_text) -> bool {
        screen_mgr.post_event(batbox::tui::Events::PlanApprovalShow);
        auto result = plan_approval_card->await_user_decision(plan_text);
        screen_mgr.post_event(batbox::tui::Events::ModalHide);
        return result.kind == batbox::tui::PlanApprovalResult::Kind::Approved;
    };
}

} // namespace batbox::app
