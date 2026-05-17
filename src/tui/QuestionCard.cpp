// src/tui/QuestionCard.cpp
// ---------------------------------------------------------------------------
// Implementation of batbox::tui::QuestionCard.
//
// QuestionCard is an FTXUI modal that displays an AskUserQuestion prompt with
// numbered option rows.  T2 shipped the OnRender path only.  Keyboard handling
// and await_user_answer() are implemented here as of TUI-ASKQ-T3.
//
// Threading model:
//   • set_spec()         — called from any thread; writes payload_ under mtx_.
//   • hide()             — called from any thread; clears visible_ atomically.
//   • await_user_answer()— called from a worker thread; blocks on cv_ until
//                          OnEvent() resolves the question via resolved_ flag.
//   • OnRender()         — called on the UI thread by the FTXUI loop.
//   • OnEvent()          — called on the UI thread; navigates + resolves.
//
// Blueprint contract: batbox::tui::QuestionCard (task TUI-ASKQ-T3)
// Resolved in TUI-ASKQ-T3
// ---------------------------------------------------------------------------

#include <batbox/tui/QuestionCard.hpp>
#include <batbox/tui/ThemeApply.hpp>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace batbox::tui {

// =============================================================================
// Construction
// =============================================================================

QuestionCard::QuestionCard(const batbox::theme::Theme& theme)
    : theme_(theme)
{}

// =============================================================================
// truncate_header() — static helper
// =============================================================================

std::string QuestionCard::truncate_header(const std::string& header) {
    // Simple byte-level truncation.  For ASCII-dominant headers (typical for
    // tool labels like "Library", "Framework") this is exact.  For multi-byte
    // UTF-8 sequences we may cut at a byte boundary; FTXUI renders the partial
    // sequence as a replacement glyph which is acceptable for a chip label.
    constexpr std::size_t kMaxChars = 12;
    if (header.size() <= kMaxChars) {
        return header;
    }
    // Take first 11 bytes + ellipsis (3 UTF-8 bytes "…" = U+2026).
    return header.substr(0, kMaxChars - 1) + "\xe2\x80\xa6";  // U+2026 HORIZONTAL ELLIPSIS
}

// =============================================================================
// total_rows() — helper
// =============================================================================

int QuestionCard::total_rows() const noexcept {
    // Snapshot payload under lock is not needed here because total_rows() is
    // called only from OnRender() and OnEvent(), both on the UI thread, and
    // set_spec() is guarded by the same mtx_ (so a full render cycle won't
    // interleave with a set_spec update mid-call).  We rely on the caller
    // holding mtx_ when needed.
    int n = static_cast<int>(payload_.labels.size());
    if (payload_.allow_freeform)     ++n;
    if (payload_.allow_escape_hatch) ++n;
    return n;
}

// =============================================================================
// set_spec() — any thread
// =============================================================================

void QuestionCard::set_spec(const QuestionShowPayload& payload) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        payload_      = payload;
        cursor_index_ = 0;
        // Initialise checkbox state to all-unchecked.
        const int rows = static_cast<int>(payload_.labels.size())
                       + (payload_.allow_freeform     ? 1 : 0)
                       + (payload_.allow_escape_hatch ? 1 : 0);
        checked_.assign(static_cast<std::size_t>(rows), false);
        resolved_ = false;
        result_   = {};
    }
    visible_.store(true, std::memory_order_release);
}

// =============================================================================
// hide() — any thread
// =============================================================================

void QuestionCard::hide() {
    visible_.store(false, std::memory_order_release);
}

// =============================================================================
// is_visible() — any thread
// =============================================================================

bool QuestionCard::is_visible() const noexcept {
    return visible_.load(std::memory_order_acquire);
}

// =============================================================================
// await_user_answer() — worker thread entry point (TUI-ASKQ-T3)
// =============================================================================

QuestionResolvedPayload QuestionCard::await_user_answer() {
    // Defensive: if set_spec() was never called, return cancelled immediately
    // so the caller does not deadlock.  This mirrors PermissionCard's approach
    // of having all state pre-populated before the wait.
    {
        std::lock_guard<std::mutex> check(mtx_);
        if (payload_.labels.empty() && !payload_.allow_freeform && !payload_.allow_escape_hatch) {
            QuestionResolvedPayload cancelled{};
            cancelled.cancelled = true;
            return cancelled;
        }
    }

    std::unique_lock<std::mutex> lock(mtx_);
    cv_.wait(lock, [this] { return resolved_; });
    return result_;
}

// =============================================================================
// OnRender() — UI thread
// =============================================================================

ftxui::Element QuestionCard::OnRender() {
    using namespace ftxui;

    const ftxui::Color bg_c      = color_for(theme_, ThemeRole::Bg);
    const ftxui::Color fg_c      = color_for(theme_, ThemeRole::Fg);
    const ftxui::Color magenta_c = color_for(theme_, ThemeRole::AccentMagenta);
    const ftxui::Color muted_c   = color_for(theme_, ThemeRole::Muted);
    const ftxui::Color code_bg_c = color_for(theme_, ThemeRole::CodeBg);

    // -------------------------------------------------------------------------
    // Snapshot mutable state under lock so the UI thread never races with a
    // concurrent set_spec() call from a worker thread.
    // -------------------------------------------------------------------------
    QuestionShowPayload snap;
    int                 cursor_snap;
    std::vector<bool>   checked_snap;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        snap         = payload_;
        cursor_snap  = cursor_index_;
        checked_snap = checked_;
    }

    // -------------------------------------------------------------------------
    // Header chip: " <header> " in AccentMagenta on CodeBg.
    // -------------------------------------------------------------------------
    const std::string hdr_text = " " + truncate_header(snap.header) + " ";
    Element header_chip = text(hdr_text)
        | bold
        | ftxui::color(magenta_c)
        | ftxui::bgcolor(code_bg_c);

    // -------------------------------------------------------------------------
    // Question line: bold Fg text.
    // -------------------------------------------------------------------------
    Element question_elem = text("  " + snap.question)
        | bold
        | ftxui::color(fg_c);

    // -------------------------------------------------------------------------
    // Option rows.
    // Each row:
    //   [cursor: "▸ " or "  "] [radio/check marker] [number. ] [bold label]
    //   (optional indented dim description on the next line)
    // -------------------------------------------------------------------------
    Elements option_rows;
    const int label_count = static_cast<int>(snap.labels.size());

    auto make_option_row = [&](int idx, const std::string& label,
                               const std::string& description) -> Elements {
        Elements row_elems;

        // Cursor arrow.
        const bool   is_cursor = (idx == cursor_snap);
        const std::string arrow = is_cursor ? "\xe2\x96\xb8 " : "  ";  // ▸ U+25B8

        // Selection marker.
        std::string marker;
        if (snap.multi_select) {
            // checked_snap may be shorter than total rows if set_spec hasn't
            // been called yet — guard with bounds check.
            const bool is_checked = (idx < static_cast<int>(checked_snap.size()))
                                  && checked_snap[static_cast<std::size_t>(idx)];
            marker = is_checked ? "\xe2\x98\x92 " : "\xe2\x98\x90 ";  // ☒/☐
        } else {
            // Single-select: filled circle at cursor position.
            marker = is_cursor ? "\xe2\x97\x8f " : "\xe2\x97\x8b ";  // ●/○
        }

        // Number label: "1. ", "2. ", ...
        const std::string num_str = std::to_string(idx + 1) + ". ";

        Element label_elem = hbox({
            text("  ") | ftxui::color(fg_c),
            text(arrow) | ftxui::color(magenta_c),
            text(marker) | ftxui::color(is_cursor ? magenta_c : muted_c),
            text(num_str) | ftxui::color(muted_c),
            text(label) | bold | ftxui::color(fg_c),
        }) | ftxui::bgcolor(bg_c);

        row_elems.push_back(std::move(label_elem));

        // Optional dim description line, indented to align with the label text.
        if (!description.empty()) {
            Element desc_elem = hbox({
                // Indent: "  " (2) + arrow (2) + marker (2) + "N. " (≤3) = ~9 chars.
                text("       ") | ftxui::color(fg_c),
                text(description) | ftxui::color(muted_c),
            }) | ftxui::bgcolor(bg_c);
            row_elems.push_back(std::move(desc_elem));
        }

        return row_elems;
    };

    // Render the user-supplied option labels.
    for (int i = 0; i < label_count; ++i) {
        const std::string& lbl  = snap.labels[static_cast<std::size_t>(i)];
        const std::string& desc = (i < static_cast<int>(snap.descriptions.size()))
                                  ? snap.descriptions[static_cast<std::size_t>(i)]
                                  : std::string{};
        auto rows = make_option_row(i, lbl, desc);
        for (auto& r : rows) {
            option_rows.push_back(std::move(r));
        }
        // Blank spacer between options for readability.
        option_rows.push_back(text("") | ftxui::bgcolor(bg_c));
    }

    // Synthetic "Type something…" row.
    if (snap.allow_freeform) {
        const int idx = label_count;
        auto rows = make_option_row(idx,
                                    "Type something\xe2\x80\xa6",  // "Type something…"
                                    "");
        for (auto& r : rows) {
            option_rows.push_back(std::move(r));
        }
        option_rows.push_back(text("") | ftxui::bgcolor(bg_c));
    }

    // Synthetic "Chat about this" row.
    if (snap.allow_escape_hatch) {
        const int idx = label_count + (snap.allow_freeform ? 1 : 0);
        auto rows = make_option_row(idx,
                                    "Chat about this",
                                    "");
        for (auto& r : rows) {
            option_rows.push_back(std::move(r));
        }
        option_rows.push_back(text("") | ftxui::bgcolor(bg_c));
    }

    // -------------------------------------------------------------------------
    // Footer hint row.
    // -------------------------------------------------------------------------
    const std::string footer_text = snap.multi_select
        ? "  Space to toggle \xc2\xb7 Enter to confirm \xc2\xb7 Esc to cancel"
        : "  Enter to select \xc2\xb7 \xe2\x86\x91/\xe2\x86\x93 to navigate \xc2\xb7 Esc to cancel";
    // · = U+00B7 (C2 B7), ↑ = U+2191 (E2 86 91), ↓ = U+2193 (E2 86 93)

    Element footer_elem = text(footer_text)
        | ftxui::color(muted_c)
        | ftxui::bgcolor(bg_c);

    // -------------------------------------------------------------------------
    // Compose modal box.
    // -------------------------------------------------------------------------
    Elements body;
    body.push_back(hbox({header_chip}) | ftxui::bgcolor(bg_c));
    body.push_back(question_elem);
    body.push_back(separator() | ftxui::color(muted_c));
    body.push_back(text("") | ftxui::bgcolor(bg_c));
    for (auto& r : option_rows) {
        body.push_back(std::move(r));
    }
    body.push_back(separator() | ftxui::color(muted_c));
    body.push_back(footer_elem);

    Element modal_box = vbox(std::move(body))
        | border
        | ftxui::bgcolor(bg_c)
        | ftxui::color(fg_c)
        | size(WIDTH, GREATER_THAN, 52)
        | size(WIDTH, LESS_THAN,    80)
        | clear_under;

    return modal_box | center;
}

// =============================================================================
// resolve() — UI thread, called from OnEvent (TUI-ASKQ-T3)
// =============================================================================

void QuestionCard::resolve(QuestionResolvedPayload r) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        result_   = std::move(r);
        resolved_ = true;
    }
    cv_.notify_one();
    hide();
}

// =============================================================================
// OnEvent() — UI thread (TUI-ASKQ-T3)
// =============================================================================

bool QuestionCard::OnEvent(ftxui::Event event) {
    // Only handle events when visible and a spec has been loaded.
    if (!is_visible()) {
        return false;
    }

    // Snapshot selection state under lock for consistency.
    QuestionShowPayload snap;
    int cursor;
    int rows;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        snap   = payload_;
        cursor = cursor_index_;
        rows   = total_rows();
    }

    const int label_count = static_cast<int>(snap.labels.size());

    // -------------------------------------------------------------------------
    // ↑ / k  — move cursor up (clamp at 0)
    // -------------------------------------------------------------------------
    if (event == ftxui::Event::ArrowUp ||
        event == ftxui::Event::Character('k')) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (cursor_index_ > 0) {
            --cursor_index_;
        }
        return true;
    }

    // -------------------------------------------------------------------------
    // ↓ / j  — move cursor down (clamp at last row)
    // -------------------------------------------------------------------------
    if (event == ftxui::Event::ArrowDown ||
        event == ftxui::Event::Character('j')) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (cursor_index_ < rows - 1) {
            ++cursor_index_;
        }
        return true;
    }

    // -------------------------------------------------------------------------
    // Space — toggle checkbox (multi_select only; no-op for single-select)
    // -------------------------------------------------------------------------
    if (event == ftxui::Event::Character(' ')) {
        if (snap.multi_select) {
            std::lock_guard<std::mutex> lock(mtx_);
            if (cursor_index_ >= 0 &&
                cursor_index_ < static_cast<int>(checked_.size())) {
                checked_[static_cast<std::size_t>(cursor_index_)] =
                    !checked_[static_cast<std::size_t>(cursor_index_)];
            }
            return true;
        }
        // Single-select: Space is a no-op (Enter confirms).
        return false;
    }

    // -------------------------------------------------------------------------
    // 1–9 — jump cursor to that 1-indexed row (if within range)
    // -------------------------------------------------------------------------
    if (event.is_character()) {
        const std::string& ch = event.character();
        if (ch.size() == 1 && ch[0] >= '1' && ch[0] <= '9') {
            const int target = static_cast<int>(ch[0] - '1');  // 0-based
            if (target < rows) {
                std::lock_guard<std::mutex> lock(mtx_);
                cursor_index_ = target;
                return true;
            }
        }
    }

    // -------------------------------------------------------------------------
    // Enter — confirm selection
    // -------------------------------------------------------------------------
    if (event == ftxui::Event::Return) {
        QuestionResolvedPayload payload{};
        payload.cancelled = false;

        // Snapshot current state under lock.
        std::vector<bool> checked_snap;
        int               cursor_snap;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            checked_snap = checked_;
            cursor_snap  = cursor_index_;
        }

        // Determine the freeform row index (if present).
        const int freeform_row_idx    = snap.allow_freeform ? label_count : -1;
        // Determine the escape-hatch row index (if present).
        const int escape_hatch_row_idx = snap.allow_escape_hatch
            ? label_count + (snap.allow_freeform ? 1 : 0)
            : -1;

        if (snap.multi_select) {
            // Multi-select: collect all checked labels.
            for (int i = 0; i < label_count; ++i) {
                if (i < static_cast<int>(checked_snap.size()) &&
                    checked_snap[static_cast<std::size_t>(i)]) {
                    payload.chosen_labels.push_back(snap.labels[static_cast<std::size_t>(i)]);
                }
            }
            // If nothing was checked, fall back to the cursor row.
            if (payload.chosen_labels.empty()) {
                if (cursor_snap >= 0 && cursor_snap < label_count) {
                    payload.chosen_labels.push_back(
                        snap.labels[static_cast<std::size_t>(cursor_snap)]);
                } else if (cursor_snap == freeform_row_idx) {
                    // Freeform sentinel: leave chosen_labels empty, freeform_text
                    // will be populated by TUI-ASKQ-T4's modal text-entry handoff.
                    payload.freeform_text = "";
                } else if (cursor_snap == escape_hatch_row_idx) {
                    payload.escape_hatch = true;
                }
            }
        } else {
            // Single-select: use cursor position.
            if (cursor_snap >= 0 && cursor_snap < label_count) {
                payload.chosen_labels.push_back(
                    snap.labels[static_cast<std::size_t>(cursor_snap)]);
            } else if (cursor_snap == freeform_row_idx) {
                // Freeform row selected: sentinel — TUI-ASKQ-T4 handles text capture.
                // chosen_labels is left empty; freeform_text is empty for now.
                payload.freeform_text = "";
            } else if (cursor_snap == escape_hatch_row_idx) {
                payload.escape_hatch = true;
            }
        }

        resolve(std::move(payload));
        return true;
    }

    // -------------------------------------------------------------------------
    // Esc — cancel
    // -------------------------------------------------------------------------
    if (event == ftxui::Event::Escape) {
        QuestionResolvedPayload payload{};
        payload.cancelled = true;
        resolve(std::move(payload));
        return true;
    }

    return false;
}

} // namespace batbox::tui
