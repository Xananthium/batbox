// include/batbox/conversation/Compactor.hpp
// =============================================================================
// batbox::conversation::Compactor — summarisation-by-LLM with verbatim tail.
//
// Responsibilities (CPP 3.5):
//   Given a conversation that has grown too large, compact it:
//     1. Split messages into "head" (older turns to summarise) and "tail"
//        (last N turns kept verbatim, where N = keep_last_n_turns_verbatim).
//     2. Send the head to the inference client with a summarisation system
//        prompt; receive a single-paragraph summary.
//     3. Return a new message list: one System-role summary message + the
//        verbatim tail.
//   A status string ("context compacted: 23 turns → 1 summary + 10 recent")
//   is delivered via an optional callback so the TUI status-line can display it.
//
// Compaction trigger:
//   Callers check ContextWindow::needs_compact() before calling compact().
//   compact() itself does NOT call needs_compact(); it always compacts when
//   invoked.
//
// Disabling:
//   Set BATBOX_AUTO_COMPACT_AT_PCT=100 (config.compact.auto_compact_at_pct==100)
//   to prevent ContextWindow::needs_compact() from ever returning true.
//   The Compactor class itself has no notion of enabling/disabling.
//
// Error semantics:
//   Propagates Result<> from Client::chat() on inference failure.
//   If the conversation is too short to split (head would be empty), returns
//   the original messages unchanged (no-op compact).
//
// Thread safety:
//   Compactor is not thread-safe; use one instance per conversation thread.
//
// Build standalone (no CMake, from repo root):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/integration/test_compactor.cpp \
//       src/conversation/Compactor.cpp \
//       src/conversation/Message.cpp \
//       src/inference/Client.cpp \
//       src/inference/ChatRequest.cpp \
//       src/core/Uuid.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libcpr.a \
//       build/vcpkg_installed/arm64-osx/lib/libcurl.a \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       -o /tmp/test_compactor && /tmp/test_compactor
// =============================================================================

#pragma once

#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/conversation/Message.hpp>
#include <batbox/inference/Client.hpp>

#include <functional>
#include <string>
#include <vector>

namespace batbox::conversation {

// =============================================================================
// Compactor
// =============================================================================

class Compactor {
public:
    // -------------------------------------------------------------------------
    // Status callback type
    //
    // Invoked once after a successful compact() with a human-readable note
    // such as "context compacted: 23 turns → 1 summary + 10 recent".
    // The callback is invoked on the calling thread before compact() returns.
    // May be nullptr if the caller does not need status notifications.
    // -------------------------------------------------------------------------
    using StatusCallback = std::function<void(const std::string& note)>;

    // -------------------------------------------------------------------------
    // Constructor
    //
    // keep_last_n — number of most-recent turns to preserve verbatim.
    //               Corresponds to BATBOX_KEEP_LAST_N_TURNS_VERBATIM.
    //               Must be >= 0.  If 0, all turns are summarised and the
    //               result is only the summary message.
    // on_status   — optional callback for the status note; may be nullptr.
    // -------------------------------------------------------------------------
    explicit Compactor(int keep_last_n, StatusCallback on_status = nullptr);

    // Non-copyable, movable.
    Compactor(const Compactor&)            = delete;
    Compactor& operator=(const Compactor&) = delete;
    Compactor(Compactor&&)                 = default;
    Compactor& operator=(Compactor&&)      = default;

    // -------------------------------------------------------------------------
    // compact()
    //
    // Compacts the conversation:
    //   - Splits msgs into head (all but last keep_last_n_) and tail
    //     (last keep_last_n_ messages, or all if fewer than keep_last_n_).
    //   - If head is empty (not enough messages to split), returns msgs
    //     unchanged — no network call is made.
    //   - Sends head to client with a summarisation system prompt
    //     (stream=false, max_tokens=1024).
    //   - On success: builds a new vector with one summary Message
    //     (Role::System, content = the model's summary) followed by the
    //     verbatim tail, then fires on_status_ if set.
    //   - On inference error: propagates the Err unchanged.
    //
    // Parameters:
    //   msgs   — the full current conversation history
    //   client — inference client to call for the summary (must be alive for
    //            the duration of compact())
    //   ct     — cancellation token; checked before the network call
    //
    // Returns:
    //   Ok(vector<Message>)  — new compacted conversation
    //   Err(string)          — inference or cancellation error message
    // -------------------------------------------------------------------------
    [[nodiscard]] Result<std::vector<Message>>
    compact(const std::vector<Message>& msgs,
            batbox::inference::Client&  client,
            batbox::CancelToken         ct);

    // -------------------------------------------------------------------------
    // Accessors
    // -------------------------------------------------------------------------
    [[nodiscard]] int keep_last_n() const noexcept { return keep_last_n_; }

private:
    int            keep_last_n_;
    StatusCallback on_status_;

    // Build the wire messages for the summarisation request.
    // Returns the WireMessage list: system prompt + head turns as user/assistant.
    static std::vector<batbox::inference::WireMessage>
    build_summary_request_messages(const std::vector<Message>& head);

    // Format the status note string.
    static std::string format_status_note(int total_turns,
                                          int summary_count,
                                          int verbatim_count);
};

} // namespace batbox::conversation
