// include/batbox/tui/StreamingMessageView.hpp
// ---------------------------------------------------------------------------
// batbox::tui::StreamingMessageView — incremental SSE-driven assistant message
//
// Design
// ------
// StreamingMessageView is a ComponentBase subclass that renders an in-flight
// assistant message while the SSE stream is open.  It is the fast path for
// token-by-token display.
//
// Threading model
// ---------------
// Two threads interact with this component:
//
//   Background (SSE reader / ScreenManager::post_token):
//     Calls append_chunk(text) directly (acquires unique_lock on text_mtx_).
//     OR posts Events::Token events via ScreenManager::post_token(), which
//     are delivered to OnEvent() on the UI thread.
//
//   UI thread (FTXUI loop):
//     Calls OnRender() and OnEvent() — both run on the UI thread.
//     OnEvent() calls append_chunk() for Token events.
//     OnRender() reads current_text_ under shared_lock.
//
// Recommended usage: post_token() → Events::Token → OnEvent() → append_chunk()
// This keeps all MarkdownRenderer mutations on the UI thread where they belong.
// If append_chunk() is called from a background thread, the shared_mutex
// guards current_text_ but renderer_ is NOT guarded — callers must ensure
// renderer_ mutations happen only on the UI thread.
//
// Streaming contract
// ------------------
// 1. Construct StreamingMessageView with the active theme.
// 2. Mount into FTXUI component tree (ChatView embeds this at the bottom).
// 3. On each SSE token: ScreenManager::post_token(text)
//    → make_token_event → FTXUI PostEvent → OnEvent() → append_chunk()
//    → renderer_.append() → marks dirty → FTXUI re-renders → OnRender()
//    → renderer_.render() produces fresh Element.
// 4. When SSE stream closes: call close_stream().  The component renders the
//    final snapshot; ChatView promotes this to a permanent Message entry.
// 5. For a new conversation turn: call reset() to clear all state.
//
// Performance
// -----------
// MarkdownRenderer caches finalized block elements.  Each append() only
// re-parses the current open block's tail.  Render time is O(cached_blocks)
// for element assembly plus O(tail) for the open block re-parse.  Target:
// <2ms per token on a typical assistant response (CPP 1.8 AC2).
//
// Theme integration
// -----------------
// ThemeRef is passed at construction; forwarded directly to MarkdownRenderer.
// All colour lookups happen inside MarkdownRenderer::render() via ThemeRole.
//
// Blueprint contract
// ------------------
// class batbox::tui::StreamingMessageView : public ftxui::ComponentBase
//   (blueprints table, task CPP 1.8)
// ---------------------------------------------------------------------------
#pragma once

#include <batbox/tui/MarkdownRender.hpp>
#include <batbox/theme/Theme.hpp>

#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <atomic>
#include <shared_mutex>
#include <string>
#include <string_view>

namespace batbox::tui {

// =============================================================================
// StreamingMessageView
// =============================================================================

/// FTXUI component that renders an in-flight assistant message token-by-token.
///
/// Typical usage (via ChatView):
/// @code
///   auto view = std::make_shared<batbox::tui::StreamingMessageView>(theme);
///   // Registered in the FTXUI component tree…
///
///   // From SSE reader thread (via ScreenManager::post_token):
///   screen.post_token(text);   // → Token event → OnEvent() → append_chunk()
///
///   // When stream closes:
///   view->close_stream();
///
///   // For a new turn:
///   view->reset();
/// @endcode
///
/// Thread safety:
///   append_chunk() — safe to call from any thread (acquires unique_lock).
///   close_stream() — safe to call from any thread.
///   reset()        — call from the UI thread only (clears renderer_ state).
///   OnRender()     — called on UI thread by FTXUI.
///   OnEvent()      — called on UI thread by FTXUI.
class StreamingMessageView : public ftxui::ComponentBase {
public:
    // -----------------------------------------------------------------------
    // Construction
    // -----------------------------------------------------------------------

    /// Construct with a theme reference.
    ///
    /// @param theme  Active theme; must outlive this component.
    explicit StreamingMessageView(const batbox::theme::Theme& theme);

    // Non-copyable, non-movable (ComponentBase contract + mutex).
    StreamingMessageView(const StreamingMessageView&)            = delete;
    StreamingMessageView& operator=(const StreamingMessageView&) = delete;
    StreamingMessageView(StreamingMessageView&&)                  = delete;
    StreamingMessageView& operator=(StreamingMessageView&&)       = delete;

    ~StreamingMessageView() override = default;

    // -----------------------------------------------------------------------
    // Streaming interface (thread-safe)
    // -----------------------------------------------------------------------

    /// Append a text chunk to the streaming buffer.
    ///
    /// Safe to call from any thread.  Acquires a unique_lock on text_mtx_ to
    /// append to current_text_.  The MarkdownRenderer is NOT updated here when
    /// called from a background thread — updates happen inside OnEvent() on the
    /// UI thread where renderer_ mutations are safe.
    ///
    /// When called from the UI thread (e.g. from OnEvent()), renderer_.append()
    /// is invoked immediately after updating current_text_.
    ///
    /// @param text  UTF-8 text fragment.  May be empty, may contain newlines.
    void append_chunk(std::string_view text);

    /// Signal that the SSE stream has closed.
    ///
    /// After close_stream() the component renders the complete final text.
    /// The caller (ChatView) should promote this message to a permanent entry
    /// and call reset() before starting the next turn.
    ///
    /// Thread-safe.
    void close_stream();

    /// Return the full accumulated text (snapshot under shared_lock).
    ///
    /// Intended for ChatView to extract the final text on stream close.
    /// Thread-safe.
    [[nodiscard]] std::string current_text() const;

    /// Return true when the SSE stream is still open (tokens may still arrive).
    [[nodiscard]] bool is_streaming() const noexcept;

    // -----------------------------------------------------------------------
    // Reset (UI thread only)
    // -----------------------------------------------------------------------

    /// Reset all state for a new conversation turn.
    ///
    /// Clears current_text_, renderer_ cache, and resets streaming_ to true.
    /// Must be called on the UI thread.
    void reset();

    // -----------------------------------------------------------------------
    // FTXUI ComponentBase overrides (UI thread)
    // -----------------------------------------------------------------------

    /// Render the current streaming message as an ftxui::Element.
    ///
    /// Reads current_text_ under shared_lock; delegates rendering to
    /// MarkdownRenderer::render() which is called on the UI thread.
    ///
    /// Returns ftxui::emptyElement() when no text has been received yet.
    ftxui::Element OnRender() override;

    /// Handle FTXUI events.
    ///
    /// Intercepts Events::Token events: extracts the TokenPayload, calls
    /// append_chunk() on the UI thread (so renderer_.append() is safe), and
    /// returns true to mark the event handled.
    ///
    /// All other events are forwarded to ComponentBase::OnEvent().
    bool OnEvent(ftxui::Event event) override;

    /// This component is not focusable (display-only).
    bool Focusable() const override;

private:
    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------

    /// Active theme reference (passed to MarkdownRenderer).
    const batbox::theme::Theme& theme_;

    /// The full accumulated text received so far.
    /// Protected by text_mtx_ for cross-thread appends.
    std::string current_text_;

    /// Guards current_text_ for concurrent reads/writes.
    mutable std::shared_mutex text_mtx_;

    /// Incremental markdown renderer.
    /// Mutations (append, reset) must happen on the UI thread.
    MarkdownRenderer renderer_;

    /// True while the SSE stream is open (more tokens may arrive).
    std::atomic<bool> streaming_{true};

    /// Flag set by OnEvent() when a Token event arrives but renderer_ has not
    /// yet been synced (i.e. append_chunk was called from a bg thread).
    /// On the next OnRender() call we re-sync from current_text_.
    std::atomic<bool> renderer_dirty_{false};
};

} // namespace batbox::tui
