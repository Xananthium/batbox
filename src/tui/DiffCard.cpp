// src/tui/DiffCard.cpp
// ---------------------------------------------------------------------------
// Implementation of batbox::tui::DiffCard.
//
// DiffCard is a blocking FTXUI modal that renders a unified diff with
// theme-aware +/- coloring and waits for the user to Accept (Enter/y)
// or Reject (Esc/n).
//
// Threading model:
//   • await()    — called from a worker thread; posts ModalShow event,
//                  then blocks on cv_ until resolve() is called.
//   • OnRender() — called on the UI (main) thread by the FTXUI loop.
//   • OnEvent()  — called on the UI thread; calls resolve() on accept/reject.
//
// Blueprint contract: batbox::tui::DiffCard (blueprints table, task CPP 1.11)
// ---------------------------------------------------------------------------

#include <batbox/tui/DiffCard.hpp>
#include <batbox/tui/Events.hpp>
#include <batbox/tui/ThemeApply.hpp>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <string>
#include <string_view>

namespace batbox::tui {

// =============================================================================
// Construction
// =============================================================================

DiffCard::DiffCard(const batbox::theme::Theme& theme)
    : theme_(theme)
{}

// =============================================================================
// await() — worker thread entry point
// =============================================================================

DiffCard::Decision DiffCard::await(const batbox::Json& payload) {
    // -------------------------------------------------------------------------
    // 1. Extract fields from payload.
    // -------------------------------------------------------------------------
    std::string diff_text;
    std::string path;
    std::string operation;

    if (payload.contains("diff") && payload.at("diff").is_string()) {
        diff_text = payload.at("diff").get<std::string>();
    }
    if (payload.contains("path") && payload.at("path").is_string()) {
        path = payload.at("path").get<std::string>();
    }
    if (payload.contains("operation") && payload.at("operation").is_string()) {
        operation = payload.at("operation").get<std::string>();
    }

    // -------------------------------------------------------------------------
    // 2. Populate UI state (safe before posting event — UI thread hasn't
    //    rendered yet, condition_variable ensures ordering).
    // -------------------------------------------------------------------------
    {
        std::lock_guard<std::mutex> lock(mtx_);
        path_        = std::move(path);
        operation_   = std::move(operation);
        scroll_offset_ = 0;
        parse_diff(diff_text);
        pending_  = true;
        resolved_ = false;
        result_   = Decision::Reject;
    }

    // -------------------------------------------------------------------------
    // 3. Post a ModalShow event to wake the UI thread.
    //    The ModalShow event's callback is not used here — DiffCard manages its
    //    own synchronisation.  We post an empty-callback ModalShow so the
    //    application's root Component knows to display the overlay.
    // -------------------------------------------------------------------------
    // Build a ModalShow event.  The callback posts nothing — DiffCard resolves
    // itself via OnEvent.
    auto modal_ev = make_modal_show_event(
        "Diff Review",
        path_,
        operation_,
        [](ModalResult) { /* DiffCard handles this internally */ }
    );
    // Note: the FTXUI ScreenInteractive is not accessible from DiffCard
    // directly.  The caller (dispatch layer) is expected to post this event
    // themselves and set the modal show-flag.  await() therefore only blocks
    // on the condition variable; the event was posted by the caller before
    // calling await(), or this component is used embedded with Modal().
    //
    // For the blocking-wait pattern we just wait on cv_ here; resolve()
    // will signal it when OnEvent fires Accept or Reject.
    // The caller is responsible for toggling the Modal show flag.

    // -------------------------------------------------------------------------
    // 4. Block until UI thread calls resolve().
    // -------------------------------------------------------------------------
    std::unique_lock<std::mutex> lock(mtx_);
    cv_.wait(lock, [this]{ return resolved_; });
    pending_ = false;
    return result_;
}

// =============================================================================
// resolve() — UI thread, called from OnEvent
// =============================================================================

void DiffCard::resolve(Decision d) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        result_   = d;
        resolved_ = true;
    }
    cv_.notify_one();
}

// =============================================================================
// parse_diff()
// =============================================================================

void DiffCard::parse_diff(const std::string& diff_text) {
    rows_.clear();

    if (diff_text.empty() || diff_text == "(no changes)" ||
        diff_text == "(no changes)\n") {
        rows_.push_back({RowKind::Context, "(no changes)"});
        return;
    }

    // Walk lines.
    std::size_t pos   = 0;
    const std::size_t len = diff_text.size();

    while (pos < len) {
        // Find end of current line.
        std::size_t end = diff_text.find('\n', pos);
        std::string_view line;
        if (end == std::string::npos) {
            line = std::string_view{diff_text}.substr(pos);
            pos  = len;
        } else {
            line = std::string_view{diff_text}.substr(pos, end - pos);
            pos  = end + 1;
        }

        if (line.empty()) {
            rows_.push_back({RowKind::Context, ""});
            continue;
        }

        const char first = line[0];
        const std::string_view rest = line.substr(1);

        if (first == '+') {
            if (line.size() >= 3 && line.substr(0, 3) == "+++") {
                rows_.push_back({RowKind::Header, std::string{line}});
            } else {
                rows_.push_back({RowKind::Add, std::string{rest}});
            }
        } else if (first == '-') {
            if (line.size() >= 3 && line.substr(0, 3) == "---") {
                rows_.push_back({RowKind::Header, std::string{line}});
            } else {
                rows_.push_back({RowKind::Remove, std::string{rest}});
            }
        } else if (first == '@') {
            rows_.push_back({RowKind::Header, std::string{line}});
        } else {
            // Context line: ' ' prefix.
            rows_.push_back({RowKind::Context,
                             first == ' ' ? std::string{rest} : std::string{line}});
        }
    }
}

// =============================================================================
// render_row()
// =============================================================================

ftxui::Element DiffCard::render_row(const DiffRow& row) const {
    using namespace ftxui;

    const std::string display = row.text;

    switch (row.kind) {
        case RowKind::Add: {
            const ftxui::Color fg = color_for(theme_, ThemeRole::DiffAddFg);
            const ftxui::Color bg = color_for(theme_, ThemeRole::DiffAddBg);
            return hbox({
                text("+") | ftxui::color(fg) | ftxui::bgcolor(bg),
                text(display) | ftxui::color(fg) | ftxui::bgcolor(bg) | flex,
            });
        }
        case RowKind::Remove: {
            const ftxui::Color fg = color_for(theme_, ThemeRole::DiffRemoveFg);
            const ftxui::Color bg = color_for(theme_, ThemeRole::DiffRemoveBg);
            return hbox({
                text("-") | ftxui::color(fg) | ftxui::bgcolor(bg),
                text(display) | ftxui::color(fg) | ftxui::bgcolor(bg) | flex,
            });
        }
        case RowKind::Header: {
            const ftxui::Color muted_c = color_for(theme_, ThemeRole::Muted);
            return text(display) | ftxui::color(muted_c);
        }
        case RowKind::Context:
        default: {
            const ftxui::Color fg = color_for(theme_, ThemeRole::Fg);
            return hbox({
                text(" ") | ftxui::color(fg),
                text(display) | ftxui::color(fg) | flex,
            });
        }
    }
}

// =============================================================================
// OnRender()
// =============================================================================

ftxui::Element DiffCard::OnRender() {
    using namespace ftxui;

    const ftxui::Color bg_c     = color_for(theme_, ThemeRole::Bg);
    const ftxui::Color fg_c     = color_for(theme_, ThemeRole::Fg);
    const ftxui::Color muted_c  = color_for(theme_, ThemeRole::Muted);
    const ftxui::Color accent_c = color_for(theme_, ThemeRole::AccentCyan);

    // -------------------------------------------------------------------------
    // Title bar
    // -------------------------------------------------------------------------
    const std::string op_label = operation_.empty() ? "edit" : operation_;
    const std::string title_text =
        "  " + op_label + " " + path_ + "  ";

    Element title_elem = hbox({
        text(title_text) | bold | ftxui::color(accent_c) | ftxui::bgcolor(bg_c),
        separator() | ftxui::color(muted_c),
    });

    // -------------------------------------------------------------------------
    // Key hint bar at the bottom
    // -------------------------------------------------------------------------
    Element hint_elem = hbox({
        text("  "),
        text("[Enter/y]") | bold | ftxui::color(accent_c),
        text(" accept  "),
        text("[Esc/n]") | bold | ftxui::color(ftxui::Color::Red),
        text(" reject  "),
        text("[↑/k ↓/j]") | ftxui::color(muted_c),
        text(" scroll  "),
    }) | ftxui::bgcolor(bg_c);

    // -------------------------------------------------------------------------
    // Diff rows (windowed to visible_height_)
    // -------------------------------------------------------------------------
    // Clamp scroll offset.
    const int total = static_cast<int>(rows_.size());
    // Reserve approx 6 lines for chrome (title, border, hints, padding).
    constexpr int kChrome = 6;
    // visible_height_ is updated from terminal size on next render; use
    // a fallback of 20 if not yet measured.
    const int display_lines = std::max(1, visible_height_ - kChrome);

    scroll_offset_ = std::max(0, std::min(scroll_offset_, total - display_lines));
    if (scroll_offset_ < 0) scroll_offset_ = 0;

    const int start = scroll_offset_;
    const int end   = std::min(start + display_lines, total);

    Elements row_elems;
    row_elems.reserve(static_cast<std::size_t>(end - start));
    for (int i = start; i < end; ++i) {
        row_elems.push_back(render_row(rows_[static_cast<std::size_t>(i)]));
    }

    // Scroll indicator line.
    Element scroll_indicator = emptyElement();
    if (total > display_lines) {
        const std::string pos_str =
            "  " + std::to_string(start + 1) + "-" +
            std::to_string(end) + "/" + std::to_string(total) + " lines";
        scroll_indicator = text(pos_str) | ftxui::color(muted_c);
    }

    // -------------------------------------------------------------------------
    // Compose modal box
    // -------------------------------------------------------------------------
    Element body = vbox(std::move(row_elems)) | ftxui::bgcolor(bg_c);

    Element modal_box = vbox({
        title_elem,
        separator() | ftxui::color(muted_c),
        body | flex,
        scroll_indicator,
        separator() | ftxui::color(muted_c),
        hint_elem,
    }) | border
      | ftxui::bgcolor(bg_c)
      | ftxui::color(fg_c)
      | size(WIDTH, GREATER_THAN, 60)
      | size(HEIGHT, LESS_THAN, 40)
      | clear_under;

    return modal_box | center;
}

// =============================================================================
// OnEvent()
// =============================================================================

bool DiffCard::OnEvent(ftxui::Event event) {
    // Accept: Enter or 'y'
    if (event == ftxui::Event::Return) {
        resolve(Decision::Accept);
        return true;
    }
    if (event == ftxui::Event::Character('y') ||
        event == ftxui::Event::Character('Y')) {
        resolve(Decision::Accept);
        return true;
    }

    // Reject: Escape or 'n'
    if (event == ftxui::Event::Escape) {
        resolve(Decision::Reject);
        return true;
    }
    if (event == ftxui::Event::Character('n') ||
        event == ftxui::Event::Character('N')) {
        resolve(Decision::Reject);
        return true;
    }

    // Scroll down: Arrow-down or 'j'
    if (event == ftxui::Event::ArrowDown ||
        event == ftxui::Event::Character('j')) {
        ++scroll_offset_;
        return true;
    }

    // Scroll up: Arrow-up or 'k'
    if (event == ftxui::Event::ArrowUp ||
        event == ftxui::Event::Character('k')) {
        scroll_offset_ = std::max(0, scroll_offset_ - 1);
        return true;
    }

    // Page down: Page-down or Ctrl+D
    if (event == ftxui::Event::PageDown) {
        scroll_offset_ += std::max(1, visible_height_ / 2);
        return true;
    }

    // Page up: Page-up or Ctrl+U
    if (event == ftxui::Event::PageUp) {
        scroll_offset_ = std::max(0, scroll_offset_ - std::max(1, visible_height_ / 2));
        return true;
    }

    return false;
}

} // namespace batbox::tui
