// include/batbox/conversation/Compactor.hpp
// =============================================================================
// batbox::conversation::Compactor — protected-tail compaction.
//
// TWO sinks live here.  The DEFAULT, load-bearing one (S5, DIS-983) is the
// NOTEPAD; the original LLM-summary sink is retained as a LEGACY path only.
//
//   compact_to_notepad()  — THE SINK (S5).  Splits messages into "head"
//     (older turns) and "tail" (last N kept verbatim).  It does NOT call a
//     model: it structurally prunes the raw tool-output bodies in the head to
//     small TOMBSTONES that reference the working notepad, where the distiller
//     (S1+S4) and the agent (S6) already wrote the gold.  Because the gold
//     lives OUT-OF-BAND in the notepad (it is not a Message — it survives
//     compaction by construction) and every prune leaves a tombstone, nothing
//     is silently lost.  This is what makes aggressive trimming SAFE and is the
//     payoff the whole batbox thesis was built toward: "the notepad is the
//     reason we can throw away the 63k-char tool dump that ate the window."
//     Deterministic, network-free, fully authored, cache-friendly.
//
//   compact()  — LEGACY LLM summary path.  Sends the head to the model for a
//     one-paragraph summary and returns [System summary] + verbatim tail.  This
//     is exactly the lossy/unauthored sink S5 was built to replace; it is NO
//     LONGER on the Conversation auto-compact path.  Kept (not deleted) so the
//     existing summariser tests keep proving the head/tail split + status
//     callback contract — i.e. demoted, not removed (DIS-983 design decision A).
//
//   A status string is delivered via an optional callback so the TUI status-line
//   can display it ("context compacted: 23 turns → 6 pruned to notepad + 10
//   recent" for the notepad path; "… → 1 summary + 10 recent" for the legacy).
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
// Build standalone (no CMake, from repo root, x64-linux triplet):
//   The notepad-sink path (compact_to_notepad) is network-free, so the
//   notepad-prune test links NO cpr/curl — only Compactor/Message/Uuid/Json:
//     c++ -std=c++20 -I include -I build/vcpkg_installed/x64-linux/include \
//         tests/unit/test_compact_to_notepad.cpp \
//         src/conversation/Compactor.cpp src/conversation/Message.cpp \
//         src/core/Uuid.cpp \
//         build/vcpkg_installed/x64-linux/lib/libsimdjson.a \
//         -o /tmp/test_compact_to_notepad && /tmp/test_compact_to_notepad
//   The LEGACY summariser test (compact()) additionally needs the HTTP client:
//     … src/inference/Client.cpp src/inference/ChatRequest.cpp \
//        src/inference/SseParser.cpp src/core/Logging.cpp src/core/Json.cpp \
//        src/core/CancelToken.cpp \
//        …/libcpr.a …/libcurl.a …/libsimdjson.a …/libspdlog.a …/libfmt.a \
//        …/libssl.a …/libcrypto.a …/libz.a -lpthread -ldl
//   (Uuid.cpp must be the DIS-969-fixed copy on this chain:
//    `git show fix/linux-build-breakage-dis969:src/core/Uuid.cpp`.)
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
    // compact_to_notepad()  — THE SINK (S5, DIS-983)
    //
    // Structurally prune the conversation WITHOUT calling a model:
    //   - Splits msgs into head (all but last keep_last_n_) and tail
    //     (last keep_last_n_ messages).
    //   - If head is empty (not enough messages to split), returns msgs
    //     unchanged — no-op compact.
    //   - In the head, every Role::Tool message's body is replaced by a small
    //     TOMBSTONE that records what was dropped (tool name + byte count) and
    //     points the reader at the working notepad (`notepad_ref`), where the
    //     distilled gold lives.  The tombstone KEEPS the message's role and
    //     tool_call_id/tool_name so the assistant↔tool wire pairing stays valid.
    //   - User / Assistant text turns in the head are kept verbatim (design
    //     decision A: prune raw tool-output bodies; preserve authored text).
    //   - The protected tail (last keep_last_n_) is preserved verbatim.
    //
    // Gold-preservation invariant (AC3): nothing leaves the context unless it is
    // (a) in the protected tail, (b) its gold is in the notepad, or (c) a
    // tombstone marking what was dropped remains.  Every prune here leaves a
    // tombstone, so (c) holds by construction; the tombstone also cites the pad
    // so (b) is reachable.  (The full raw also remains on disk in the session
    // transcript — compaction only rewrites the in-memory message array.)
    //
    // Parameters:
    //   msgs        — the full current conversation history
    //   notepad_ref — a human-readable reference to the working notepad (the
    //                 pad file path, NotepadStore::pad_path(key).string()), cited
    //                 in each tombstone.  May be empty (tombstones then omit the
    //                 path but still record the byte count).
    //
    // Returns:
    //   Ok(vector<Message>) — new compacted conversation (no network, never errs
    //                         in practice; Result kept for call-site symmetry).
    //
    // const: does not mutate the Compactor; fires on_status_ if set.
    // -------------------------------------------------------------------------
    [[nodiscard]] Result<std::vector<Message>>
    compact_to_notepad(const std::vector<Message>& msgs,
                       const std::string&          notepad_ref) const;

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

    // Format the status note string (legacy summary path).
    static std::string format_status_note(int total_turns,
                                          int summary_count,
                                          int verbatim_count);

    // Format the status note for the notepad-sink path.
    static std::string format_notepad_status_note(int total_turns,
                                                  int pruned_count,
                                                  int verbatim_count);

    // Build the tombstone body that replaces a pruned tool-result.  Records the
    // tool name and original byte count and cites the notepad (when non-empty).
    static std::string make_tombstone(const std::string& tool_name,
                                      std::size_t        original_bytes,
                                      const std::string& notepad_ref);
};

// =============================================================================
// compaction_should_run() — S9 stand-down gate (S5, DIS-983, AC5)
// =============================================================================
//
// Pure decision predicate: compaction runs only when the context-window gate
// says it is needed AND the active provider does NOT manage its own context
// window.  When a provider's underlying backend owns the window
// (Provider::manages_own_context() == true), batbox stands its compaction down
// entirely so it does not double-manage the window.
//
//   compaction_should_run(needs, manages) == (needs && !manages)
//
// Kept as a free function so the stand-down decision is unit-testable in
// isolation and shared by both the proactive (pre-flight) and reactive
// (overflow-retry) compaction paths in Conversation.
[[nodiscard]] bool compaction_should_run(bool gate_needs_compact,
                                        bool provider_manages_own_context) noexcept;

} // namespace batbox::conversation
