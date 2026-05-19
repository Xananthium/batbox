// include/batbox/app/PlanConfirmFn.hpp
// =============================================================================
// make_plan_confirm_fn — factory for ExitPlanModeTool::ConfirmFn (PEXT3 1.5)
//
// Design (data abstraction over OO, Karla K4):
//   Free function returning a std::function.  No class, no manager, no new
//   abstraction layers.  Closure captures only what it needs by value.
//
// Nuclear short-circuit:
//   When nuclear == true, the returned closure auto-approves immediately and
//   posts NO Events::PlanApprovalShow event (the modal is never rendered).
//   This is the user-visible fix for the /nuclear + ExitPlanMode bug.
//
// Normal path:
//   Posts Events::PlanApprovalShow, blocks on PlanApprovalCard::await_user_decision,
//   posts Events::ModalHide, returns true iff Kind::Approved.
//
// DRY note (PEXT3 1.5/1.6):
//   is_nuclear() is trivially inlined here since it is just `return nuclear;`.
//   If additional callers proliferate, factor it out.
// =============================================================================

#pragma once

#include <batbox/tools/ExitPlanModeTool.hpp>
#include <batbox/tui/PlanApprovalCard.hpp>
#include <batbox/tui/Screen.hpp>

#include <memory>

namespace batbox::app {

/// Returns an ExitPlanModeTool::ConfirmFn that:
///   - nuclear == true  → returns true immediately, no modal posted.
///   - nuclear == false → posts PlanApprovalShow, blocks on card, returns result.
///
/// @param nuclear            True when --nuclear was passed on the command line.
/// @param plan_approval_card Shared card component; captured by value (shared_ptr copy).
/// @param screen_mgr         ScreenManager reference; captured by reference — must
///                           outlive the returned closure (same lifetime as App::run()).
[[nodiscard]]
batbox::tools::ExitPlanModeTool::ConfirmFn
make_plan_confirm_fn(bool nuclear,
                     std::shared_ptr<batbox::tui::PlanApprovalCard> plan_approval_card,
                     batbox::tui::ScreenManager& screen_mgr);

} // namespace batbox::app
