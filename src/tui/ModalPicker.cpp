// src/tui/ModalPicker.cpp
//
// batbox::tui::ModalPicker implementation.
//
// See include/batbox/tui/ModalPicker.hpp for the full design contract.

#include "batbox/tui/ModalPicker.hpp"

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include <algorithm>
#include <cctype>
#include <numeric>
#include <string>
#include <vector>

namespace batbox::tui {

// =============================================================================
// Internal helpers
// =============================================================================

/// Case-insensitive substring search: true iff haystack contains needle.
static bool icontains(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(),   needle.end(),
        [](unsigned char a, unsigned char b) {
            return std::tolower(a) == std::tolower(b);
        });
    return it != haystack.end();
}

// =============================================================================
// Construction
// =============================================================================

ModalPicker::ModalPicker(batbox::theme::ThemeRef          theme,
                         std::string                       title,
                         std::vector<std::string>          items,
                         bool                              show_filter,
                         std::function<void(int)>          on_select,
                         std::function<void()>             on_cancel)
    : theme_(theme)
    , title_(std::move(title))
    , items_(std::move(items))
    , show_filter_(show_filter)
    , on_select_(std::move(on_select))
    , on_cancel_(std::move(on_cancel))
    , cursor_(0)
{
    rebuild_filtered();
}

// =============================================================================
// ComponentBase overrides
// =============================================================================

ftxui::Element ModalPicker::OnRender() {
    using namespace ftxui;

    const ftxui::Color fg_col     = color_for(theme_, ThemeRole::Fg);
    const ftxui::Color bg_col     = color_for(theme_, ThemeRole::Bg);
    const ftxui::Color accent_m   = color_for(theme_, ThemeRole::AccentMagenta);
    const ftxui::Color accent_c   = color_for(theme_, ThemeRole::AccentCyan);
    const ftxui::Color muted_col  = color_for(theme_, ThemeRole::Muted);

    // --- Title bar -----------------------------------------------------------
    Element title_elem = text(" " + title_ + " ")
        | ftxui::color(accent_c)
        | ftxui::bold;

    // --- Filter input (optional) --------------------------------------------
    Element filter_elem = ftxui::emptyElement();
    if (show_filter_) {
        std::string display_filter = filter_.empty()
            ? std::string("  filter: ")
            : std::string("  filter: ") + filter_;
        filter_elem = ftxui::hbox({
            ftxui::text(display_filter) | ftxui::color(muted_col),
            ftxui::text(filter_.empty() ? "_" : "") | ftxui::color(muted_col),
        }) | ftxui::size(HEIGHT, EQUAL, 1);
    }

    // --- Item list -----------------------------------------------------------
    Elements rows;
    rows.reserve(filtered_indices_.size());

    for (int fi = 0; fi < static_cast<int>(filtered_indices_.size()); ++fi) {
        const std::string& label = items_[static_cast<std::size_t>(
            filtered_indices_[static_cast<std::size_t>(fi)])];

        bool selected = (fi == cursor_);

        Element row;
        if (selected) {
            row = ftxui::text(" > " + label + " ")
                | ftxui::color(accent_m)
                | ftxui::bgcolor(bg_col)
                | ftxui::bold;
        } else {
            row = ftxui::text("   " + label + " ")
                | ftxui::color(fg_col)
                | ftxui::bgcolor(bg_col);
        }
        rows.push_back(std::move(row));
    }

    if (rows.empty()) {
        rows.push_back(
            ftxui::text("   (no matches)")
            | ftxui::color(muted_col));
    }

    Element list_elem = ftxui::vbox(std::move(rows));

    // --- Footer help bar -----------------------------------------------------
    Element footer_elem = ftxui::text(" ↑↓ navigate  ·  Enter select  ·  Esc cancel ")
        | ftxui::color(muted_col);

    // --- Compose panel -------------------------------------------------------
    Elements panel_children;
    panel_children.push_back(title_elem);
    panel_children.push_back(ftxui::separator());
    if (show_filter_) {
        panel_children.push_back(filter_elem);
        panel_children.push_back(ftxui::separator());
    }
    panel_children.push_back(list_elem);
    panel_children.push_back(ftxui::separator());
    panel_children.push_back(footer_elem);

    return ftxui::vbox(std::move(panel_children))
        | ftxui::border
        | ftxui::size(WIDTH,  GREATER_THAN, 40)
        | ftxui::size(HEIGHT, LESS_THAN,    30)
        | ftxui::bgcolor(bg_col)
        | ftxui::clear_under
        | ftxui::center;
}

bool ModalPicker::OnEvent(ftxui::Event event) {
    // --- Filter input: printable characters and backspace -------------------
    if (show_filter_) {
        if (event.is_character()) {
            filter_ += event.character();
            rebuild_filtered();
            return true;
        }
        if (event == ftxui::Event::Backspace) {
            if (!filter_.empty()) {
                filter_.pop_back();
                rebuild_filtered();
            }
            return true;
        }
    }

    // --- Navigation ----------------------------------------------------------
    if (event == ftxui::Event::ArrowUp ||
        (event.is_character() && event.character() == "k")) {
        if (!filtered_indices_.empty()) {
            cursor_ = (cursor_ == 0)
                ? static_cast<int>(filtered_indices_.size()) - 1
                : cursor_ - 1;
        }
        return true;
    }

    if (event == ftxui::Event::ArrowDown ||
        (event.is_character() && event.character() == "j")) {
        if (!filtered_indices_.empty()) {
            cursor_ = (cursor_ + 1) %
                      static_cast<int>(filtered_indices_.size());
        }
        return true;
    }

    // --- Confirm selection ---------------------------------------------------
    if (event == ftxui::Event::Return) {
        if (!filtered_indices_.empty() &&
            cursor_ < static_cast<int>(filtered_indices_.size())) {
            int original_idx = filtered_indices_[static_cast<std::size_t>(cursor_)];
            on_select_(original_idx);
        }
        return true;
    }

    // --- Cancel --------------------------------------------------------------
    if (event == ftxui::Event::Escape) {
        on_cancel_();
        return true;
    }

    return false;
}

// =============================================================================
// Runtime control
// =============================================================================

void ModalPicker::set_items(std::vector<std::string> new_items) {
    items_ = std::move(new_items);
    filter_.clear();
    cursor_ = 0;
    rebuild_filtered();
}

void ModalPicker::reset() {
    filter_.clear();
    cursor_ = 0;
    rebuild_filtered();
}

// =============================================================================
// Private helpers
// =============================================================================

void ModalPicker::rebuild_filtered() {
    filtered_indices_.clear();
    filtered_indices_.reserve(items_.size());

    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        if (icontains(items_[static_cast<std::size_t>(i)], filter_)) {
            filtered_indices_.push_back(i);
        }
    }

    // Clamp cursor to valid range.
    if (filtered_indices_.empty()) {
        cursor_ = 0;
    } else if (cursor_ >= static_cast<int>(filtered_indices_.size())) {
        cursor_ = static_cast<int>(filtered_indices_.size()) - 1;
    }
}

std::string ModalPicker::current_label() const {
    if (filtered_indices_.empty()) return {};
    if (cursor_ < 0 || cursor_ >= static_cast<int>(filtered_indices_.size()))
        return {};
    return items_[static_cast<std::size_t>(
        filtered_indices_[static_cast<std::size_t>(cursor_)])];
}

} // namespace batbox::tui
