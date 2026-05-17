// src/tui/PermissionCard.cpp
// ---------------------------------------------------------------------------
// Implementation of batbox::tui::PermissionCard.
//
// PermissionCard is a blocking FTXUI modal that displays a tool-permission
// prompt and waits for the user to press one of five action keys:
//   a — allow once          A — always allow (with rule pattern)
//   n — deny once           N — always deny  (with rule pattern)
//   e — edit args           Esc — cancel (treated as one-shot deny)
//
// Threading model:
//   • await_user_decision() — called from a worker thread; populates state,
//                              then blocks on cv_ until resolve() is called.
//   • OnRender()            — called on the UI (main) thread by the FTXUI loop.
//   • OnEvent()             — called on the UI thread; calls resolve() on keypress.
//
// Editor integration (TUI-FIX-T7):
//   The e handler calls batbox::util::edit_string_in_editor(args_preview_, screen_)
//   which suspends FTXUI via WithRestoredIO(), runs the resolved editor
//   (resolve_editor()), reads back the edited file, sets Decision::edit_text,
//   then resolves so the caller can re-prompt with the modified args.
//
// Blueprint contract: batbox::tui::PermissionCard (blueprints table, task CPP 1.10)
// ---------------------------------------------------------------------------

#include <batbox/tui/PermissionCard.hpp>
#include <batbox/tui/Events.hpp>
#include <batbox/tui/ThemeApply.hpp>
#include <batbox/permissions/PermissionGate.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/util/EditorLaunch.hpp>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <sstream>
#include <string>
#include <string_view>

namespace batbox::tui {

// =============================================================================
// Construction
// =============================================================================

PermissionCard::PermissionCard(const batbox::theme::Theme& theme)
    : theme_(theme)
    , result_(batbox::permissions::Decision::deny())
{}

// =============================================================================
// build_preview() — static helper
// =============================================================================

std::string PermissionCard::build_preview(const batbox::Json& args) {
    try {
        // pretty() with 2-space indent for compact but readable display
        return args.dump(2);
    } catch (...) {
        return args.dump();
    }
}

// =============================================================================
// build_default_rule() — static helper
// =============================================================================

std::string PermissionCard::build_default_rule(std::string_view tool_name,
                                                const batbox::Json& args) {
    // Extract the canonical argument field for a glob pattern.
    // Order of preference: command → file_path → path → url → query → *
    std::string_view field_names[] = {"command", "file_path", "path", "url", "query"};
    for (auto field : field_names) {
        if (args.contains(field) && args.at(field).is_string()) {
            std::string val = args.at(field).get<std::string>();
            // Truncate very long values and replace trailing portion with *
            if (val.size() > 32) {
                val = val.substr(0, 24) + "*";
            }
            return std::string(tool_name) + "(" + val + ")";
        }
    }
    // Fallback: tool-name wildcard
    return std::string(tool_name) + "(*)";
}

// =============================================================================
// await_user_decision() — worker thread entry point
// =============================================================================

batbox::permissions::Decision PermissionCard::await_user_decision(
    std::string_view tool_name,
    const batbox::Json& args)
{
    // -------------------------------------------------------------------------
    // 1. Populate display state before unblocking.
    // -------------------------------------------------------------------------
    {
        std::lock_guard<std::mutex> lock(mtx_);
        tool_name_    = std::string(tool_name);
        args_preview_ = build_preview(args);
        default_rule_ = build_default_rule(tool_name, args);
        result_       = batbox::permissions::Decision::deny();
        pending_      = true;
        resolved_     = false;
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

void PermissionCard::resolve(batbox::permissions::Decision d) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        result_   = std::move(d);
        resolved_ = true;
    }
    cv_.notify_one();
}

// =============================================================================
// pending() accessor
// =============================================================================

bool PermissionCard::pending() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return pending_;
}

// =============================================================================
// OnRender() — UI thread
// =============================================================================

ftxui::Element PermissionCard::OnRender() {
    using namespace ftxui;

    const ftxui::Color bg_c      = color_for(theme_, ThemeRole::Bg);
    const ftxui::Color fg_c      = color_for(theme_, ThemeRole::Fg);
    const ftxui::Color magenta_c = color_for(theme_, ThemeRole::AccentMagenta);
    const ftxui::Color cyan_c    = color_for(theme_, ThemeRole::AccentCyan);
    const ftxui::Color muted_c   = color_for(theme_, ThemeRole::Muted);
    const ftxui::Color code_bg_c = color_for(theme_, ThemeRole::CodeBg);
    const ftxui::Color err_c     = color_for(theme_, ThemeRole::Error);
    const ftxui::Color succ_c    = color_for(theme_, ThemeRole::Success);

    // -------------------------------------------------------------------------
    // Snapshot mutable state under lock.
    // -------------------------------------------------------------------------
    std::string tool_name_snap;
    std::string args_preview_snap;
    std::string default_rule_snap;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        tool_name_snap    = tool_name_;
        args_preview_snap = args_preview_;
        default_rule_snap = default_rule_;
    }

    // -------------------------------------------------------------------------
    // Title bar: "  Permission Request  "
    // -------------------------------------------------------------------------
    Element title_elem = hbox({
        text("  Permission Request  ")
            | bold
            | ftxui::color(cyan_c)
            | ftxui::bgcolor(bg_c),
    });

    // -------------------------------------------------------------------------
    // Tool name line: "  tool: BashTool  " in magenta
    // -------------------------------------------------------------------------
    const std::string tool_label = tool_name_snap.empty() ? "(unknown)" : tool_name_snap;
    Element tool_elem = hbox({
        text("  tool: ") | ftxui::color(muted_c),
        text(tool_label) | bold | ftxui::color(magenta_c),
    });

    // -------------------------------------------------------------------------
    // Arguments preview box — pretty-printed JSON in code_bg.
    // Wrap at 60 chars per line and limit to 12 visible lines.
    // -------------------------------------------------------------------------
    Elements arg_lines;
    {
        std::istringstream ss(args_preview_snap.empty() ? "{}" : args_preview_snap);
        std::string line;
        int line_count = 0;
        while (std::getline(ss, line) && line_count < 12) {
            // Trim long lines to 62 chars to avoid wrapping.
            if (line.size() > 62) {
                line = line.substr(0, 59) + "...";
            }
            arg_lines.push_back(
                text("  " + line)
                    | ftxui::color(fg_c)
                    | ftxui::bgcolor(code_bg_c)
            );
            ++line_count;
        }
        if (arg_lines.empty()) {
            arg_lines.push_back(
                text("  {}")
                    | ftxui::color(muted_c)
                    | ftxui::bgcolor(code_bg_c)
            );
        }
    }
    Element args_box = vbox(std::move(arg_lines)) | ftxui::bgcolor(code_bg_c);

    // -------------------------------------------------------------------------
    // Rule pattern hint line (shown when pending)
    // -------------------------------------------------------------------------
    Element rule_hint = hbox({
        text("  rule: ") | ftxui::color(muted_c),
        text(default_rule_snap.empty() ? "(*)" : default_rule_snap)
            | ftxui::color(muted_c),
    });

    // -------------------------------------------------------------------------
    // Key-hint footer
    // -------------------------------------------------------------------------
    Element hint_line_1 = hbox({
        text("  "),
        text("[a]") | bold | ftxui::color(succ_c),
        text(" allow once    "),
        text("[A]") | bold | ftxui::color(succ_c),
        text(" always allow"),
    }) | ftxui::bgcolor(bg_c);

    Element hint_line_2 = hbox({
        text("  "),
        text("[n]") | bold | ftxui::color(err_c),
        text(" deny           "),
        text("[N]") | bold | ftxui::color(err_c),
        text(" always deny"),
    }) | ftxui::bgcolor(bg_c);

    Element hint_line_3 = hbox({
        text("  "),
        text("[e]") | bold | ftxui::color(cyan_c),
        text(" edit args      "),
        text("[Esc]") | bold | ftxui::color(muted_c),
        text(" cancel"),
    }) | ftxui::bgcolor(bg_c);

    // -------------------------------------------------------------------------
    // Compose modal box
    // -------------------------------------------------------------------------
    Element modal_box = vbox({
        title_elem,
        separator() | ftxui::color(muted_c),
        tool_elem,
        separator() | ftxui::color(muted_c),
        args_box,
        separator() | ftxui::color(muted_c),
        rule_hint,
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

bool PermissionCard::OnEvent(ftxui::Event event) {
    // a — Allow once (one-shot, no rule)
    if (event == ftxui::Event::Character('a')) {
        resolve(batbox::permissions::Decision::allow());
        return true;
    }

    // A — Always allow (persist allow rule with default pattern)
    if (event == ftxui::Event::Character('A')) {
        std::string rule_pattern;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            rule_pattern = default_rule_;
        }
        resolve(batbox::permissions::Decision::allow_with_rule(rule_pattern));
        return true;
    }

    // n — Deny once (one-shot, no rule)
    if (event == ftxui::Event::Character('n')) {
        resolve(batbox::permissions::Decision::deny());
        return true;
    }

    // N — Always deny (persist deny rule with default pattern)
    if (event == ftxui::Event::Character('N')) {
        std::string rule_pattern;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            rule_pattern = default_rule_;
        }
        resolve(batbox::permissions::Decision::deny_with_rule(rule_pattern));
        return true;
    }

    // e — Edit args: open args_preview_ in $EDITOR / nano / pico / vi.
    //
    // TUI-FIX-T7: edit_string_in_editor() suspends FTXUI via WithRestoredIO()
    // (if screen_ is set), runs the editor, reads back the edited content.
    // The edited JSON string is stored in Decision::edit_text so the caller
    // can re-prompt the tool with the modified arguments.
    if (event == ftxui::Event::Character('e')) {
        std::string args_snap;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            args_snap = args_preview_;
        }
        const std::string edited = batbox::util::edit_string_in_editor(args_snap, screen_);
        batbox::permissions::Decision edit_decision = batbox::permissions::Decision::deny();
        edit_decision.edit_text = edited;
        resolve(std::move(edit_decision));
        return true;
    }

    // Esc — Cancel: one-shot deny
    if (event == ftxui::Event::Escape) {
        resolve(batbox::permissions::Decision::deny());
        return true;
    }

    return false;
}

} // namespace batbox::tui
