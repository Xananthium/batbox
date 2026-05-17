// src/tui/PlanApprovalCard.cpp
// ---------------------------------------------------------------------------
// Implementation of batbox::tui::PlanApprovalCard.
//
// PlanApprovalCard is a blocking FTXUI modal that displays a plan-approval
// prompt and waits for the user to press one of three action keys:
//   a / A / Enter — Approve (confirm the plan; ExitPlanMode transitions to Approved)
//   r / R / Esc   — Reject  (send plan back for revision)
//   e / E         — Edit    (returns plan text so the model receives feedback)
//
// Threading model:
//   • await_user_decision() — called from a worker thread; populates state,
//                              then blocks on cv_ until resolve() is called.
//   • OnRender()            — called on the UI (main) thread by the FTXUI loop.
//   • OnEvent()             — called on the UI thread; calls resolve() on keypress.
//
// Blueprint contract: batbox::tui::PlanApprovalCard (task TUI-PLAN-T2)
// ---------------------------------------------------------------------------

#include <batbox/tui/PlanApprovalCard.hpp>
#include <batbox/tui/Events.hpp>
#include <batbox/tui/ThemeApply.hpp>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <sstream>
#include <string>

namespace batbox::tui {

// =============================================================================
// Construction
// =============================================================================

PlanApprovalCard::PlanApprovalCard(const batbox::theme::Theme& theme)
    : theme_(theme)
    , result_(PlanApprovalResult::rejected())
{}

// =============================================================================
// await_user_decision() — worker thread entry point
// =============================================================================

PlanApprovalResult PlanApprovalCard::await_user_decision(const std::string& plan_text) {
    // -------------------------------------------------------------------------
    // 1. Populate display state before unblocking.
    // -------------------------------------------------------------------------
    {
        std::lock_guard<std::mutex> lock(mtx_);
        plan_text_  = plan_text;
        result_     = PlanApprovalResult::rejected();
        pending_    = true;
        resolved_   = false;
    }

    // -------------------------------------------------------------------------
    // 2. Block until the UI thread calls resolve().
    // -------------------------------------------------------------------------
    std::unique_lock<std::mutex> lock(mtx_);
    cv_.wait(lock, [this] { return resolved_; });
    pending_ = false;
    return result_;
}

// =============================================================================
// resolve() — UI thread, called from OnEvent
// =============================================================================

void PlanApprovalCard::resolve(PlanApprovalResult r) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        result_   = std::move(r);
        resolved_ = true;
    }
    cv_.notify_one();
}

// =============================================================================
// pending() accessor
// =============================================================================

bool PlanApprovalCard::pending() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return pending_;
}

// =============================================================================
// plan_text() accessor
// =============================================================================

std::string PlanApprovalCard::plan_text() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return plan_text_;
}

// =============================================================================
// OnRender() — UI thread
// =============================================================================

ftxui::Element PlanApprovalCard::OnRender() {
    using namespace ftxui;

    const ftxui::Color bg_c      = color_for(theme_, ThemeRole::Bg);
    const ftxui::Color fg_c      = color_for(theme_, ThemeRole::Fg);
    const ftxui::Color cyan_c    = color_for(theme_, ThemeRole::AccentCyan);
    const ftxui::Color muted_c   = color_for(theme_, ThemeRole::Muted);
    const ftxui::Color code_bg_c = color_for(theme_, ThemeRole::CodeBg);
    const ftxui::Color err_c     = color_for(theme_, ThemeRole::Error);
    const ftxui::Color succ_c    = color_for(theme_, ThemeRole::Success);

    // -------------------------------------------------------------------------
    // Snapshot mutable state under lock.
    // -------------------------------------------------------------------------
    std::string plan_snap;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        plan_snap = plan_text_;
    }

    // -------------------------------------------------------------------------
    // Title bar
    // -------------------------------------------------------------------------
    Element title_elem = hbox({
        text("  Plan Review — Approve, Reject, or Edit  ")
            | bold
            | ftxui::color(cyan_c)
            | ftxui::bgcolor(bg_c),
    });

    // -------------------------------------------------------------------------
    // Plan text display — up to 20 lines, truncated to 72 chars per line.
    // -------------------------------------------------------------------------
    Elements plan_lines;
    {
        std::istringstream ss(plan_snap.empty() ? "(no plan text)" : plan_snap);
        std::string line;
        int line_count = 0;
        constexpr int kMaxLines    = 20;
        constexpr int kMaxLineCols = 72;
        while (std::getline(ss, line) && line_count < kMaxLines) {
            if (static_cast<int>(line.size()) > kMaxLineCols) {
                line = line.substr(0, kMaxLineCols - 3) + "...";
            }
            plan_lines.push_back(
                text("  " + line)
                    | ftxui::color(fg_c)
                    | ftxui::bgcolor(code_bg_c)
            );
            ++line_count;
        }
        // If there are more lines, indicate truncation.
        {
            // Count remaining lines by scanning the stringstream.
            std::string dummy;
            int remaining = 0;
            while (std::getline(ss, dummy)) ++remaining;
            if (remaining > 0) {
                plan_lines.push_back(
                    text("  … (" + std::to_string(remaining) + " more lines)")
                        | ftxui::color(muted_c)
                        | ftxui::bgcolor(code_bg_c)
                );
            }
        }
        if (plan_lines.empty()) {
            plan_lines.push_back(
                text("  (empty plan)")
                    | ftxui::color(muted_c)
                    | ftxui::bgcolor(code_bg_c)
            );
        }
    }
    Element plan_box = vbox(std::move(plan_lines)) | ftxui::bgcolor(code_bg_c);

    // -------------------------------------------------------------------------
    // Key-hint footer
    // -------------------------------------------------------------------------
    Element hint_line_1 = hbox({
        text("  "),
        text("[A]") | bold | ftxui::color(succ_c),
        text(" Approve       "),
        text("[R]") | bold | ftxui::color(err_c),
        text(" Reject"),
    }) | ftxui::bgcolor(bg_c);

    Element hint_line_2 = hbox({
        text("  "),
        text("[E]") | bold | ftxui::color(cyan_c),
        text(" Edit / send feedback"),
    }) | ftxui::bgcolor(bg_c);

    Element hint_line_3 = hbox({
        text("  "),
        text("[Enter]") | bold | ftxui::color(succ_c),
        text(" Approve  "),
        text("[Esc]") | bold | ftxui::color(muted_c),
        text(" Reject"),
    }) | ftxui::bgcolor(bg_c);

    // -------------------------------------------------------------------------
    // Compose modal box
    // -------------------------------------------------------------------------
    Element modal_box = vbox({
        title_elem,
        separator() | ftxui::color(muted_c),
        plan_box,
        separator() | ftxui::color(muted_c),
        hint_line_1,
        hint_line_2,
        hint_line_3,
    })
        | border
        | ftxui::bgcolor(bg_c)
        | ftxui::color(fg_c)
        | size(WIDTH, GREATER_THAN, 52)
        | size(WIDTH, LESS_THAN,    80)
        | clear_under;

    return modal_box | center;
}

// =============================================================================
// OnEvent() — UI thread
// =============================================================================

bool PlanApprovalCard::OnEvent(ftxui::Event event) {
    // a / A — Approve (case-insensitive; Enter also approves)
    if (event == ftxui::Event::Character('a') ||
        event == ftxui::Event::Character('A') ||
        event == ftxui::Event::Return) {
        resolve(PlanApprovalResult::approved());
        return true;
    }

    // r / R — Reject (case-insensitive)
    if (event == ftxui::Event::Character('r') ||
        event == ftxui::Event::Character('R')) {
        resolve(PlanApprovalResult::rejected());
        return true;
    }

    // Esc — Reject (cancel = reject)
    if (event == ftxui::Event::Escape) {
        resolve(PlanApprovalResult::rejected());
        return true;
    }

    // e / E — Edit: return the plan text as feedback for the model
    if (event == ftxui::Event::Character('e') ||
        event == ftxui::Event::Character('E')) {
        std::string plan_snap;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            plan_snap = plan_text_;
        }
        resolve(PlanApprovalResult::edited(std::move(plan_snap)));
        return true;
    }

    return false;
}

} // namespace batbox::tui
