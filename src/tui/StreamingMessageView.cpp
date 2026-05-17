// src/tui/StreamingMessageView.cpp
// ---------------------------------------------------------------------------
// batbox::tui::StreamingMessageView — implementation
//
// See include/batbox/tui/StreamingMessageView.hpp for full design notes.
// ---------------------------------------------------------------------------

#include <batbox/tui/StreamingMessageView.hpp>
#include <batbox/tui/Events.hpp>
#include <batbox/tui/ThemeApply.hpp>

#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <string>
#include <string_view>

namespace batbox::tui {

// =============================================================================
// Construction
// =============================================================================

StreamingMessageView::StreamingMessageView(const batbox::theme::Theme& theme)
    : theme_(theme)
    , renderer_(theme)
{
    // streaming_ starts true: tokens are expected until close_stream() is called.
    // renderer_ is freshly constructed with an empty state ready for append().
}

// =============================================================================
// Streaming interface
// =============================================================================

void StreamingMessageView::append_chunk(std::string_view text) {
    if (text.empty()) {
        return;
    }

    // Append to the raw text buffer under unique_lock so background threads
    // (SSE reader posting directly) are safe.
    {
        std::unique_lock<std::shared_mutex> lk(text_mtx_);
        current_text_ += text;
    }

    // renderer_.append() is NOT called here when this function may be invoked
    // from a background thread.  Instead we set renderer_dirty_ so that the
    // next OnRender() on the UI thread will re-sync.
    //
    // When called from the UI thread (via OnEvent → Token handling), the
    // renderer is updated immediately inside OnEvent() after this call returns,
    // because OnEvent() knows it is on the UI thread.
    renderer_dirty_.store(true, std::memory_order_release);
}

void StreamingMessageView::close_stream() {
    streaming_.store(false, std::memory_order_release);
}

std::string StreamingMessageView::current_text() const {
    std::shared_lock<std::shared_mutex> lk(text_mtx_);
    return current_text_;
}

bool StreamingMessageView::is_streaming() const noexcept {
    return streaming_.load(std::memory_order_acquire);
}

// =============================================================================
// Reset (UI thread only)
// =============================================================================

void StreamingMessageView::reset() {
    // Clear the text buffer under unique_lock so any background thread that
    // might still be in-flight sees the cleared state.
    {
        std::unique_lock<std::shared_mutex> lk(text_mtx_);
        current_text_.clear();
    }

    // renderer_ mutations are UI-thread-only; reset clears all cached elements.
    renderer_.reset();

    // Restore streaming state for a new turn.
    streaming_.store(true, std::memory_order_release);
    renderer_dirty_.store(false, std::memory_order_release);
}

// =============================================================================
// FTXUI ComponentBase overrides
// =============================================================================

ftxui::Element StreamingMessageView::OnRender() {
    // -------------------------------------------------------------------------
    // Sync renderer_ if a background thread appended text directly.
    //
    // We detect this via renderer_dirty_.  If set, the renderer may be behind
    // current_text_.  We reset and re-feed the renderer from the full text
    // snapshot.  This is a fallback path; the normal path (Token events) keeps
    // the renderer in sync incrementally via OnEvent().
    // -------------------------------------------------------------------------
    if (renderer_dirty_.load(std::memory_order_acquire)) {
        // Snapshot under shared_lock.
        std::string snapshot;
        {
            std::shared_lock<std::shared_mutex> lk(text_mtx_);
            snapshot = current_text_;
        }

        // Re-feed from scratch only if the renderer is meaningfully behind.
        // We check by seeing if the renderer would produce the same element.
        // Since we cannot cheaply compare, we check if cached_block_count is
        // stale: if snapshot is non-empty and renderer has no content but
        // snapshot has content, reset and re-feed.
        //
        // The simpler and more correct approach: reset + re-feed whenever dirty.
        // This is O(snapshot.size()) but happens only on the dirty path (direct
        // background calls, not the normal Token event path).
        renderer_.reset();
        if (!snapshot.empty()) {
            renderer_.append(snapshot);
        }

        renderer_dirty_.store(false, std::memory_order_release);
    }

    // -------------------------------------------------------------------------
    // Render the current content.
    // -------------------------------------------------------------------------
    ftxui::Element content = renderer_.render();

    if (!is_streaming()) {
        // Stream is closed: render the final message without cursor decoration.
        return content;
    }

    // While streaming: append a blinking cursor indicator to signal activity.
    // We use a simple "▋" block cursor character styled with accent_magenta.
    ftxui::Color cursor_col = color_for(theme_, ThemeRole::AccentMagenta);
    ftxui::Element cursor   = ftxui::text("▋") | ftxui::color(cursor_col);

    // Check if there is any content to display by inspecting the text buffer.
    // We cannot compare ftxui::Element pointers (each call returns a new heap object).
    bool has_content;
    {
        std::shared_lock<std::shared_mutex> lk(text_mtx_);
        has_content = !current_text_.empty();
    }

    if (!has_content) {
        return cursor;
    }

    return ftxui::vbox({
        std::move(content),
        std::move(cursor),
    });
}

bool StreamingMessageView::OnEvent(ftxui::Event event) {
    // -------------------------------------------------------------------------
    // Token event: extract payload, append to renderer, mark for redraw.
    // -------------------------------------------------------------------------
    if (auto payload = extract_token(event)) {
        // We are on the UI thread here.  It is safe to call renderer_.append()
        // directly without the dirty-flag indirection.
        const std::string& text = payload->text;

        if (!text.empty()) {
            // Update raw text buffer.
            {
                std::unique_lock<std::shared_mutex> lk(text_mtx_);
                current_text_ += text;
            }

            // Update renderer incrementally (UI thread — safe).
            renderer_.append(text);

            // Clear the dirty flag since renderer_ is now up-to-date.
            renderer_dirty_.store(false, std::memory_order_release);
        }

        // Event consumed.
        return true;
    }

    // All other events are not handled by this display-only component.
    return ComponentBase::OnEvent(std::move(event));
}

bool StreamingMessageView::Focusable() const {
    // StreamingMessageView is a display-only component; it does not participate
    // in keyboard focus navigation.
    return false;
}

} // namespace batbox::tui
