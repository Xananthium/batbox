#pragma once

// ---------------------------------------------------------------------------
// batbox/core/CancelToken.hpp
//
// Thin wrapper around std::stop_source / std::stop_token so callers can
// propagate cancellation across API boundaries without exposing <stop_token>
// everywhere.
//
// Design:
//   CancelSource  – owning side; call request_stop() to cancel.
//   CancelToken   – non-owning handle; passed into long-running operations.
//   combine_tokens(a, b) – returns a token that fires when EITHER a or b fires.
//
// CancelToken is move-only (copy explicitly deleted).
//
// Cooperative cancellation pattern:
//
//   auto [src, tok] = CancelToken::make_root();
//   // pass tok to workers ...
//   src.request_stop();          // cancel everything
//
//   // inside a worker:
//   tok.throw_if_cancelled();    // throws CancelledException
//   tok.on_cancel([&]{ io.cancel(); });
// ---------------------------------------------------------------------------

#include <stop_token>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace batbox {

// ---------------------------------------------------------------------------
// CancelledException
// Thrown by CancelToken::throw_if_cancelled() when the token has been fired.
// ---------------------------------------------------------------------------
class CancelledException : public std::runtime_error {
public:
    explicit CancelledException(const std::string& msg = "operation cancelled")
        : std::runtime_error(msg) {}
};

// ---------------------------------------------------------------------------
// Forward declaration
// ---------------------------------------------------------------------------
class CancelToken;

// ---------------------------------------------------------------------------
// CancelSource
// Owns a std::stop_source; produces CancelToken handles via token().
// Call request_stop() to cancel all tokens vended from this source.
// ---------------------------------------------------------------------------
class CancelSource {
public:
    CancelSource() = default;

    // Non-copyable, movable.
    CancelSource(const CancelSource&) = delete;
    CancelSource& operator=(const CancelSource&) = delete;
    CancelSource(CancelSource&&) noexcept = default;
    CancelSource& operator=(CancelSource&&) noexcept = default;

    // Returns a CancelToken connected to this source.
    [[nodiscard]] CancelToken token() const;

    // Fires all registered callbacks and marks all vended tokens as cancelled.
    void request_stop() noexcept { source_.request_stop(); }

    // Returns true if stop has already been requested.
    [[nodiscard]] bool stop_requested() const noexcept {
        return source_.stop_requested();
    }

    // Direct access to the underlying stop_source (for advanced use).
    [[nodiscard]] const std::stop_source& native() const noexcept { return source_; }
    [[nodiscard]] std::stop_source& native() noexcept { return source_; }

private:
    std::stop_source source_;
};

// ---------------------------------------------------------------------------
// CancelToken
// Non-owning, move-only cancellation handle.
// Wraps one or more std::stop_token values; is_cancelled() returns true
// when ANY of them has been requested to stop.
// ---------------------------------------------------------------------------
class CancelToken {
public:
    // Default-constructs a token that is never cancelled (always-alive token).
    CancelToken() = default;

    // Move-only; copy is explicitly deleted.
    CancelToken(const CancelToken&) = delete;
    CancelToken& operator=(const CancelToken&) = delete;
    CancelToken(CancelToken&&) noexcept = default;
    CancelToken& operator=(CancelToken&&) noexcept = default;

    // -----------------------------------------------------------------------
    // Factory: make_root()
    // Returns a (source, token) pair. source owns the cancellation signal;
    // token is vended to workers.
    // -----------------------------------------------------------------------
    [[nodiscard]] static std::pair<CancelSource, CancelToken> make_root();

    // -----------------------------------------------------------------------
    // child()
    // Creates a linked child CancelToken that fires when THIS token fires.
    // Also returns a CancelSource so the child can be cancelled independently.
    // -----------------------------------------------------------------------
    [[nodiscard]] std::pair<CancelSource, CancelToken> child() const;

    // -----------------------------------------------------------------------
    // Query
    // -----------------------------------------------------------------------
    [[nodiscard]] bool is_cancelled() const noexcept { return stop_requested(); }

    // Compatibility alias matching std::stop_token API.
    [[nodiscard]] bool stop_requested() const noexcept;

    // -----------------------------------------------------------------------
    // throw_if_cancelled()
    // Throws CancelledException if this token has been cancelled.
    // Use at suspension points inside long-running operations.
    // -----------------------------------------------------------------------
    void throw_if_cancelled() const {
        if (stop_requested()) throw CancelledException{};
    }

    // -----------------------------------------------------------------------
    // on_cancel(fn)
    // Registers a callback invoked when the token fires (may be immediate if
    // already cancelled). The callback is guaranteed to be called exactly once
    // on the thread that calls request_stop(), or on the calling thread if
    // already cancelled. Callbacks are invoked in order of registration.
    //
    // Returns a handle object (stop_callback_node) whose lifetime controls
    // registration. Destroy the returned object to deregister (RAII).
    // The caller is responsible for keeping the handle alive as long as the
    // callback should remain registered.
    // -----------------------------------------------------------------------
    [[nodiscard]] std::shared_ptr<void> on_cancel(std::function<void()> fn) const;

    // -----------------------------------------------------------------------
    // Internal construction from a raw stop_token (used by CancelSource,
    // combine_tokens, and child()).
    // -----------------------------------------------------------------------
    explicit CancelToken(std::stop_token tok);

    // Merge-constructor: fires when ANY of the given tokens fires.
    explicit CancelToken(std::vector<std::stop_token> tokens);

private:
    // Each CancelToken can watch multiple stop_tokens (for combine_tokens /
    // child links). We store them in a shared, heap-allocated state so the
    // CancelToken itself can be cheaply moved.
    struct State {
        // The primary token — always valid (stop_token's no-stop-state is safe).
        std::stop_token primary;

        // Additional tokens watched (for combine / child scenarios).
        // When any of these fires, the CancelSource inside `relay_source` is
        // triggered, which in turn fires `primary`.
        std::stop_source relay_source;
        // Callbacks wired from each watched token into relay_source.
        // Held here to keep the std::stop_callback alive.
        std::vector<std::shared_ptr<void>> watchers;
    };

    std::shared_ptr<State> state_;

    // Helper: ensure state_ is initialised.
    void ensure_state();

    // Build state from a single token (may be the relay output).
    void init_from_token(std::stop_token tok);

    // Wire an additional token so that when `extra` fires, our relay also fires.
    void add_watcher(const std::stop_token& extra);

    friend CancelToken combine_tokens(CancelToken a, CancelToken b);
};

// ---------------------------------------------------------------------------
// combine_tokens(a, b)
// Returns a new CancelToken that fires when EITHER a OR b fires.
// The caller passes ownership of both tokens (they are moved in).
// ---------------------------------------------------------------------------
[[nodiscard]] CancelToken combine_tokens(CancelToken a, CancelToken b);

} // namespace batbox
