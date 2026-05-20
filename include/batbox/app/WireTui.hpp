// include/batbox/app/WireTui.hpp
// =============================================================================
// wire_tui() — compose the main TUI root Component from the four top-level
// sub-components (ChatView, SubAgentPanel, DemonPanel, InputBar) and handle
// the Splash → main layout transition.
//
// Blueprint contract (task CPP 1.15):
//   function  batbox::app::wire_tui
//   file      include/batbox/app/WireTui.hpp
//   file      src/app/WireTui.cpp
//
// Layout produced (ned-cpp.md §2.C1):
//
//   ┌──────────────────────────────────────────────────────────┐
//   │  ChatView (flex)               │  SubAgentPanel (30 cols, │
//   │    scrollable message history  │   only when ≥1 active)  │
//   │    StreamingMessageView        │                          │
//   ├──────────────────────────────────────────────────────────┤
//   │  InputBar (4 lines reserved for prompt + status line)    │
//   └──────────────────────────────────────────────────────────┘
//         └─ DemonPanel (floating bottom-right, hidden by default)
//
// Splash → main transition:
//   If Splash::should_skip() returns true (BATBOX_NO_SPLASH=true), the main
//   layout is mounted immediately via ScreenManager::swap_root().
//   Otherwise, Splash is mounted first; its on_done callback swaps to the
//   main layout once the 1.5s auto-advance timer fires or the user presses
//   any key.
//
// Pattern mirrors CPP 5.30 WireTools:
//   App::run() constructs the four component instances and dependency objects,
//   then delegates all layout composition to wire_tui().  wire_tui() does not
//   start the ScreenManager event loop; App::run() calls screen_mgr.run()
//   after wire_tui() returns.
//
// CPP A.3 wiring (fix #29):
//   model_name          — when non-empty, calls InputBar::set_model() so the
//                         status row shows the active model from first render.
//   on_submit_override  — when non-null, replaces the no-op on_submit stub
//                         and routes submitted text to Conversation.
//
// TUI-T4 wiring (UI-D2):
//   permission_card     — when non-null, a PermissionCard overlay is composed
//                         on top of the main layout.  The card is shown (via
//                         dbox + clear_under) whenever permission_card->pending()
//                         returns true and receives all keyboard events while
//                         pending.  When null (default), no modal is shown and
//                         tests that do not need permission prompts are unaffected.
// =============================================================================

#pragma once

#include <batbox/mcp/McpServerRegistry.hpp>
#include <batbox/tui/ChatView.hpp>
#include <batbox/tui/DemonPanel.hpp>
#include <batbox/tui/InputBar.hpp>
#include <batbox/tui/PermissionCard.hpp>
#include <batbox/tui/PlanApprovalCard.hpp>
#include <batbox/tui/QuestionCard.hpp>
#include <batbox/tui/ModalPickerHost.hpp>
#include <batbox/tui/SubAgentPanel.hpp>
#include <batbox/tui/Screen.hpp>
#include <batbox/agents/AgentSupervisor.hpp>
#include <batbox/agents/AgentEvent.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/theme/Theme.hpp>
#include <batbox/repl/History.hpp>
#include <batbox/repl/Keybindings.hpp>

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <functional>
#include <string>

// Forward declaration — callers pass a raw non-owning pointer; the full header
// is included in WireTui.cpp where set_permission_gate() is called.
namespace batbox::permissions { class PermissionGate; }

namespace batbox::app {

/// Construct all four TUI components, compose them into the main layout, and
/// mount the Splash → main layout transition on the provided ScreenManager.
///
/// Constructs:
///   - batbox::tui::ChatView          (scrollable message history)
///   - batbox::tui::SubAgentPanel     (right sidebar, 10Hz change-driven)
///   - batbox::tui::DemonPanel        (floating bottom-right, 5Hz, hidden by default)
///   - batbox::tui::InputBar          (bottom 4 lines, prompt + status)
///
/// After wire_tui() returns, App::run() must call screen_mgr.run() to enter
/// the FTXUI event loop.  The components remain alive for the loop duration
/// via shared_ptr ownership captured in the FTXUI Component tree.
///
/// @param screen_mgr          ScreenManager that owns the ScreenInteractive.
///                            Must outlive the FTXUI event loop.
/// @param supervisor          AgentSupervisor for SubAgentPanel dirty-seq polling.
///                            May be nullptr; panel renders a muted placeholder.
/// @param queue               AgentEventQueue whose dirty_seq() the 10Hz ticker polls.
///                            Must outlive the event loop.
/// @param theme               Active colour palette.  Must outlive the event loop.
/// @param history             REPL history for InputBar up/down navigation.
///                            Must outlive the event loop.
/// @param keybindings         Resolved keybinding map for InputBar ReplAction dispatch.
///                            Must outlive the event loop.
/// @param model_name          When non-empty, passed to InputBar::set_model() so the
///                            status row shows the correct model from first render.
///                            Defaults to "" (status row shows blank model until updated).
/// @param on_submit_override  When non-null, wired as the InputBar on_submit callback.
///                            When null (default), on_submit is a no-op.
///                            The callback is invoked on the FTXUI UI thread; callers
///                            are responsible for dispatching long-running work (e.g.
///                            inference) to a background thread.
/// @param slash_registry      When non-null, the InputBar slash palette is populated
///                            from this registry.  When null (default), the palette
///                            opens but shows no commands (UI-D5 fix — TUI-T3).
/// @param permission_card     When non-null, a PermissionCard modal overlay is composed
///                            on top of the main layout.  The overlay is visible whenever
///                            permission_card->pending() returns true (i.e., the worker
///                            thread is blocked in await_user_decision()).  Keyboard events
///                            are routed to the card while it is pending.
///                            When null (default), no permission overlay is mounted.
///                            The pointer must remain valid for the lifetime of the event
///                            loop; the shared_ptr owner in App::run() satisfies this.
///                            (UI-D2 fix — TUI-T4)
/// @param plan_approval_card  When non-null, a PlanApprovalCard modal overlay is
///                            composed on top of the main layout alongside the
///                            PermissionCard overlay.  Keyboard events are routed
///                            to PlanApprovalCard while it is pending.
///                            When null, no plan-approval overlay is mounted.
///                            (TUI-PLAN-T2)
/// @param question_card       When non-null, a QuestionCard modal overlay is
///                            composed on top of the main layout below the
///                            PlanApprovalCard overlay.  The card is shown
///                            (via dbox + clear_under) whenever
///                            chat_view->show_question_card() returns true.
///                            Keyboard events are routed to the QuestionCard
///                            while it is visible.  Z-order (outermost wins):
///                              PermissionCard > PlanApprovalCard > QuestionCard.
///                            When null, no question overlay is mounted.
///                            (TUI-ASKQ-T4)
/// @param modal_picker        When non-null, a ModalPicker modal overlay is
///                            composed as the outermost layer (Z-order highest).
///                            Shown whenever modal_picker->pending() returns true.
///                            All keyboard events are routed to the picker while
///                            it is pending; the base layout does not receive input.
///                            When null (default), no picker overlay is mounted.
///                            (UX-A)
void wire_tui(
    batbox::tui::ScreenManager&                 screen_mgr,
    batbox::agents::AgentSupervisor*            supervisor,
    const batbox::agents::AgentEventQueue&      queue,
    const batbox::theme::Theme&                 theme,
    batbox::repl::History&                      history,
    batbox::repl::Keybindings&                  keybindings,
    std::string                                 model_name          = {},
    batbox::tui::InputBar::SubmitCallback       on_submit_override  = nullptr,
    batbox::commands::SlashCommandRegistry*     slash_registry      = nullptr,
    batbox::tui::PermissionCard*                permission_card     = nullptr,
    batbox::tui::PlanApprovalCard*              plan_approval_card  = nullptr,
    batbox::tui::QuestionCard*                  question_card       = nullptr,
    batbox::mcp::McpServerRegistry*             mcp_registry        = nullptr,
    batbox::permissions::PermissionGate*        permission_gate     = nullptr,
    batbox::tui::InputBar::InterruptCallback    on_interrupt_cb     = nullptr,
    batbox::tui::ModalPickerHost*               modal_picker        = nullptr);

} // namespace batbox::app
