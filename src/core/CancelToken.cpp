// ---------------------------------------------------------------------------
// src/core/CancelToken.cpp
// Implementation of batbox::CancelToken and batbox::CancelSource.
// ---------------------------------------------------------------------------

#include "batbox/core/CancelToken.hpp"

#include <atomic>
#include <cassert>

namespace batbox {

// ---------------------------------------------------------------------------
// CancelSource::token()
// ---------------------------------------------------------------------------
CancelToken CancelSource::token() const {
    return CancelToken{source_.get_token()};
}

// ---------------------------------------------------------------------------
// CancelToken private helpers
// ---------------------------------------------------------------------------

// Helper type that holds a std::stop_callback<Fn>.
// Stored as shared_ptr<void> so we can type-erase it.
template<typename Fn>
struct StopCallbackNode {
    std::stop_callback<Fn> cb;
    explicit StopCallbackNode(std::stop_token tok, Fn fn)
        : cb(std::move(tok), std::move(fn)) {}
};

void CancelToken::ensure_state() {
    if (!state_) {
        state_ = std::make_shared<State>();
        // state_->primary is a no-stop-state token — stop_requested() always false.
        // That's the correct default for a never-cancelled token.
    }
}

void CancelToken::init_from_token(std::stop_token tok) {
    state_ = std::make_shared<State>();
    state_->primary = std::move(tok);
}

void CancelToken::add_watcher(const std::stop_token& extra) {
    // When `extra` fires, call request_stop() on our relay_source.
    // We wrap a stop_callback in a shared_ptr<void> for type erasure.
    assert(state_ && "add_watcher called before init_from_token");

    // Capture relay_source by value (shared state through the shared_ptr).
    std::stop_source relay = state_->relay_source;
    auto node = std::make_shared<StopCallbackNode<std::function<void()>>>(
        extra,
        [relay]() mutable { relay.request_stop(); }
    );
    state_->watchers.push_back(node);
}

// ---------------------------------------------------------------------------
// Constructors
// ---------------------------------------------------------------------------

CancelToken::CancelToken(std::stop_token tok) {
    init_from_token(std::move(tok));
}

CancelToken::CancelToken(std::vector<std::stop_token> tokens) {
    if (tokens.empty()) {
        // Never-cancelled token.
        ensure_state();
        return;
    }

    // Use the relay pattern: first token is our primary relay output,
    // and each additional token drives the relay source.
    // We create a fresh relay source, wire ALL tokens to it, and use its
    // output token as our primary.
    state_ = std::make_shared<State>();
    state_->primary = state_->relay_source.get_token();

    for (auto& t : tokens) {
        add_watcher(t);
    }
}

// ---------------------------------------------------------------------------
// stop_requested()
// ---------------------------------------------------------------------------
bool CancelToken::stop_requested() const noexcept {
    if (!state_) return false;
    return state_->primary.stop_requested();
}

// ---------------------------------------------------------------------------
// make_root()
// Returns (CancelSource, CancelToken) pair.
// ---------------------------------------------------------------------------
std::pair<CancelSource, CancelToken> CancelToken::make_root() {
    CancelSource src;
    CancelToken tok = src.token();
    return {std::move(src), std::move(tok)};
}

// ---------------------------------------------------------------------------
// child()
// Creates a child token that fires when the parent (this) fires.
// Also returns a CancelSource for independent child cancellation.
// ---------------------------------------------------------------------------
std::pair<CancelSource, CancelToken> CancelToken::child() const {
    // Grab the parent's primary stop_token.
    std::stop_token parent_tok;
    if (state_) {
        parent_tok = state_->primary;
    }
    // else parent_tok is a no-stop-state token — child is independent.

    // Create a fresh child source.
    CancelSource child_src;
    // Get the child's token from the child source.
    std::stop_token child_raw = child_src.native().get_token();

    // Build a combined CancelToken from both the child source's token
    // AND the parent token. We use the relay pattern: relay fires when
    // either fires, and child_token wraps the relay output.
    auto state = std::make_shared<State>();
    state->primary = state->relay_source.get_token();

    // Wire parent → relay
    {
        std::stop_source relay_copy = state->relay_source;
        auto node = std::make_shared<StopCallbackNode<std::function<void()>>>(
            parent_tok,
            [relay_copy]() mutable { relay_copy.request_stop(); }
        );
        state->watchers.push_back(node);
    }
    // Wire child_src → relay
    {
        std::stop_source relay_copy = state->relay_source;
        auto node = std::make_shared<StopCallbackNode<std::function<void()>>>(
            child_raw,
            [relay_copy]() mutable { relay_copy.request_stop(); }
        );
        state->watchers.push_back(node);
    }

    CancelToken child_tok;
    child_tok.state_ = std::move(state);

    return {std::move(child_src), std::move(child_tok)};
}

// ---------------------------------------------------------------------------
// on_cancel(fn)
// Registers callback; returns a handle whose lifetime controls registration.
// ---------------------------------------------------------------------------
std::shared_ptr<void> CancelToken::on_cancel(std::function<void()> fn) const {
    std::stop_token tok;
    if (state_) {
        tok = state_->primary;
    }
    // If no state_, tok is no-stop-state and the callback is never called —
    // which is correct for a never-cancelled token.
    auto node = std::make_shared<StopCallbackNode<std::function<void()>>>(
        std::move(tok),
        std::move(fn)
    );
    return node;
}

// ---------------------------------------------------------------------------
// combine_tokens(a, b)
// ---------------------------------------------------------------------------
CancelToken combine_tokens(CancelToken a, CancelToken b) {
    std::stop_token tok_a, tok_b;
    if (a.state_) tok_a = a.state_->primary;
    if (b.state_) tok_b = b.state_->primary;

    // Build a fresh relay that fires when either fires.
    auto state = std::make_shared<CancelToken::State>();
    state->primary = state->relay_source.get_token();

    // Wire tok_a → relay
    {
        std::stop_source relay_copy = state->relay_source;
        auto node = std::make_shared<StopCallbackNode<std::function<void()>>>(
            tok_a,
            [relay_copy]() mutable { relay_copy.request_stop(); }
        );
        state->watchers.push_back(node);
    }
    // Wire tok_b → relay
    {
        std::stop_source relay_copy = state->relay_source;
        auto node = std::make_shared<StopCallbackNode<std::function<void()>>>(
            tok_b,
            [relay_copy]() mutable { relay_copy.request_stop(); }
        );
        state->watchers.push_back(node);
    }

    CancelToken result;
    result.state_ = std::move(state);
    return result;
}

} // namespace batbox
