// src/tui/ModalPickerHost.cpp
//
// batbox::tui::ModalPickerHost — implementation.
//
// See include/batbox/tui/ModalPickerHost.hpp for design notes.

#include <batbox/tui/ModalPickerHost.hpp>

#include <string>
#include <vector>

namespace batbox::tui {

// =============================================================================
// Construction
// =============================================================================

ModalPickerHost::ModalPickerHost(batbox::theme::ThemeRef theme)
    : theme_(theme)
{
    // Construct a placeholder ModalPicker.  It is replaced (via set_items +
    // reset) at the start of each await_selection() call to load the new
    // title and items.  We construct it here so picker_component() is always
    // non-null for WireTui wiring.
    picker_ = std::shared_ptr<ModalPicker>(new ModalPicker(
        theme_,
        "",         // title — overwritten per call
        {},         // items — overwritten per call
        /*show_filter=*/true,
        [this](int idx) { resolve_select(idx); },
        [this]()        { resolve_cancel(); }));
}

// =============================================================================
// await_selection — worker thread entry point
// =============================================================================

std::optional<std::size_t> ModalPickerHost::await_selection(
    std::string_view             title,
    std::span<const std::string> items,
    std::size_t                  current_idx)
{
    // -------------------------------------------------------------------------
    // 1. Populate ModalPicker state before marking pending.
    //    set_items() resets filter and cursor; then we reconstruct with the
    //    new title (ModalPicker stores title at construction — we rebuild it).
    // -------------------------------------------------------------------------
    std::vector<std::string> items_vec(items.begin(), items.end());

    {
        std::lock_guard<std::mutex> lk(mtx_);
        resolved_ = false;
        result_   = std::nullopt;

        // Rebuild the picker with updated title and items.
        // The new picker captures the same resolve_select/resolve_cancel lambdas
        // via raw pointer (this), which is safe because ModalPickerHost outlives
        // the picker.
        picker_ = std::shared_ptr<ModalPicker>(new ModalPicker(
            theme_,
            std::string(title),
            items_vec,
            /*show_filter=*/true,
            [this](int idx) { resolve_select(idx); },
            [this]()        { resolve_cancel(); }));

        // Move cursor to current_idx if in range.
        // ModalPicker initialises cursor_ = 0; we do an equivalent number of
        // Down events by setting the cursor directly — but ModalPicker does not
        // expose set_cursor().  Instead, we accept that cursor starts at 0.
        // The user can see the currently-active model via items_vec contents
        // (ModelCmd will mark it visually in the label string).
        (void)current_idx;  // cursor start-at-0 is the documented default

        pending_  = true;
    }

    // -------------------------------------------------------------------------
    // 2. Block until the UI thread calls resolve_select() or resolve_cancel().
    // -------------------------------------------------------------------------
    std::unique_lock<std::mutex> lock(mtx_);
    cv_.wait(lock, [this] { return resolved_; });
    pending_ = false;
    return result_;
}

// =============================================================================
// resolve helpers — UI thread, called from ModalPicker callbacks
// =============================================================================

void ModalPickerHost::resolve_select(int original_idx) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        result_   = static_cast<std::size_t>(original_idx);
        resolved_ = true;
    }
    cv_.notify_one();
}

void ModalPickerHost::resolve_cancel() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        result_   = std::nullopt;
        resolved_ = true;
    }
    cv_.notify_one();
}

// =============================================================================
// pending accessor — UI thread
// =============================================================================

bool ModalPickerHost::pending() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return pending_;
}

} // namespace batbox::tui
