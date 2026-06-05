// include/batbox/inference/ThinkSplitter.hpp
// ---------------------------------------------------------------------------
// batbox::inference — ThinkSplitter (S10).
//
// A standalone, stateful streaming filter that separates streamed `content`
// fragments into VISIBLE text and REASONING text by detecting an inline tag
// pair (default `<think>` … `</think>`).
//
// Why this exists:
//   batbox already isolates reasoning when the endpoint emits it as a STRUCTURED
//   wire field (delta.reasoning_content — Magistral/Qwen3/DeepSeek).  But many
//   local / OAI-compatible endpoints (DeepSeek-R1 over raw endpoints, some
//   Qwen/ollama configs) instead emit reasoning INLINE inside `content`, wrapped
//   in `<think>…</think>` tags.  Without a splitter that reasoning leaks into the
//   visible output and is never isolated for downstream use.  This is goose's
//   `ThinkFilter` organ, which batbox lacked.  Isolation is the hard prerequisite
//   for any later distill/swap of the reasoning stream — "you can only distill or
//   swap the reasoning stream if you can isolate it."
//
// Core requirement — cross-chunk-boundary correctness:
//   The `<think>` / `</think>` markers arrive split arbitrarily across SSE chunks
//   (e.g. "<thi" in one chunk, "nk>" in the next).  ThinkSplitter buffers a
//   BOUNDED tail of potential-partial-tag bytes and never emits a partial marker
//   as visible text.  Feeding the same logical stream chopped at any boundary
//   (including byte-by-byte) yields identical visible + reasoning output to the
//   un-chopped case.
//
// Algorithm (incremental, O(marker) per byte, tail bounded by marker length):
//   State is {Visible | Reasoning} plus a `pending_` buffer that is always a
//   PREFIX of the marker currently being scanned for (the open marker while
//   Visible, the close marker while Reasoning).  For each incoming byte:
//     1. append it to pending_;
//     2. while pending_ is no longer a prefix of the marker, the first buffered
//        byte cannot begin a match — emit it to the current sink and re-test the
//        remaining tail (this tries every start position, correct even for
//        self-overlapping markers);
//     3. if pending_ now equals the full marker, the delimiter is complete:
//        clear pending_ and toggle state.  The marker itself is consumed — it is
//        emitted to NEITHER sink (it is wire framing, not text).
//   At finish(): whatever remains in pending_ is a partial marker that never
//   completed; it is flushed to the CURRENT sink (trailing look-alike text while
//   Visible; the tail of an unclosed think block while Reasoning).
//
// Edge cases (all handled + unit-tested):
//   - no think block            → pure pass-through to visible.
//   - block spanning many chunks → reassembled regardless of chunking.
//   - multiple blocks in a turn  → after a close, scanning resumes for the next open.
//   - text before/after a block  → routed to visible; inner text to reasoning.
//   - unclosed `<think>` at EOS  → remainder treated as reasoning (no hang).
//   - stray `</think>` (no open) → while Visible we scan only for the OPEN marker,
//                                  so a lone close tag passes through as visible text.
//
// Configurable tag pair:
//   Constructed from a ReasoningTags pair (default `<think>`/`</think>`).  A
//   provider can declare a different convention (`<thinking>`, `<reasoning>`, …)
//   or "no inline tags" (empty tags → disabled → pure pass-through).  The
//   per-provider convention is sourced from the S8 provider profile via
//   reasoning_tags_for_provider() (see Provider.hpp).
//
// Thread safety: NOT thread-safe.  One instance per streaming request; do not
// share across threads (same contract as ToolCallAccumulator).
//
// Build (standalone, no CMake — from repo root):
//   c++ -std=c++20 -I include \
//       tests/unit/test_think_splitter.cpp \
//       src/inference/ThinkSplitter.cpp \
//       -o /tmp/test_ts && /tmp/test_ts
// ---------------------------------------------------------------------------

#pragma once

#include <string>
#include <string_view>

namespace batbox::inference {

// ============================================================================
// ReasoningTags — the inline reasoning-tag convention for a provider
// ============================================================================

/// The open/close marker pair that delimits inline reasoning inside `content`.
///
/// Defaults to the de-facto `<think>` / `</think>` convention.  A provider that
/// emits reasoning only via the structured delta.reasoning_content field
/// declares "no inline tags" by setting either marker empty (see none()); in
/// that case ThinkSplitter is a pure pass-through.
struct ReasoningTags {
    std::string open  = "<think>";
    std::string close = "</think>";

    /// Inline-tag splitting is active only when BOTH markers are non-empty.
    [[nodiscard]] bool enabled() const noexcept {
        return !open.empty() && !close.empty();
    }

    /// "No inline tags" — provider uses the structured reasoning field only.
    [[nodiscard]] static ReasoningTags none() { return ReasoningTags{"", ""}; }
};

// ============================================================================
// ThinkSplit — the text produced by one push()/finish() call
// ============================================================================

/// Output of a single ThinkSplitter::push()/finish() call: the visible and
/// reasoning text produced by THAT call (not cumulative).  Concatenating the
/// `visible` (resp. `reasoning`) fields across all calls reconstructs the full
/// visible (resp. reasoning) stream.
struct ThinkSplit {
    std::string visible;
    std::string reasoning;
};

// ============================================================================
// ThinkSplitter — stateful streaming visible/reasoning separator
// ============================================================================

class ThinkSplitter {
public:
    /// Construct with a tag convention.  Defaults to `<think>`/`</think>`.
    /// When tags.enabled() is false the splitter is a pure pass-through:
    /// every byte is emitted as visible and nothing is buffered.
    explicit ThinkSplitter(ReasoningTags tags = ReasoningTags{});

    // Non-copyable; movable (same contract as ToolCallAccumulator).
    ThinkSplitter(const ThinkSplitter&)            = delete;
    ThinkSplitter& operator=(const ThinkSplitter&) = delete;
    ThinkSplitter(ThinkSplitter&&)                 = default;
    ThinkSplitter& operator=(ThinkSplitter&&)      = default;

    /// Consume one streamed `content` fragment.  Returns the visible and
    /// reasoning text that became unambiguous as a result of this fragment.
    /// Bytes that are a potential partial marker are buffered internally and
    /// NOT returned until they are resolved (so a partial marker is never
    /// emitted as visible).
    [[nodiscard]] ThinkSplit push(std::string_view fragment);

    /// Flush at end of stream.  Any buffered partial-marker tail is emitted to
    /// the current sink: visible if outside a block (trailing look-alike text),
    /// reasoning if inside an unclosed block.  Idempotent after the first call.
    [[nodiscard]] ThinkSplit finish();

    /// True while inside a `<think>…` block (i.e. accumulating reasoning).
    [[nodiscard]] bool in_reasoning() const noexcept {
        return state_ == State::Reasoning;
    }

    /// True when inline-tag splitting is active (tags were enabled).
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

private:
    enum class State { Visible, Reasoning };

    /// Feed a single byte through the state machine, appending any resolved
    /// output to @p out.
    void feed(char c, ThinkSplit& out);

    /// Append @p ch to the sink for the CURRENT state.
    void emit(ThinkSplit& out, char ch) const;

    /// The marker currently being scanned for (open while Visible, close while
    /// Reasoning).
    [[nodiscard]] const std::string& active_marker() const noexcept;

    ReasoningTags tags_;
    bool          enabled_;
    State         state_   = State::Visible;
    std::string   pending_;   ///< bounded partial-marker tail (≤ marker length)
};

// ============================================================================
// reasoning_tags_for_provider — S10 provider-profile declaration
// ============================================================================

/// The inline reasoning-tag convention declared by a provider.
///
/// Pure function of the canonical provider name (the resolve_provider_hint /
/// provider_key_for_config output), in the same data-light style as the other
/// S8 profile predicates.  Returns the open/close markers a ThinkSplitter should
/// scan `content` for, or ReasoningTags::none() when the provider emits reasoning
/// only via the structured delta.reasoning_content field.  Default convention is
/// `<think>` / `</think>`.  Defined in ReasoningTagProfile.cpp — a leaf TU that
/// depends only on this header and the standard library (no Client/cpr chain), so
/// it can be unit-tested in isolation.  Config-level and provider-method wrappers
/// live in Provider.hpp.
[[nodiscard]] ReasoningTags reasoning_tags_for_provider(std::string_view provider_name);

} // namespace batbox::inference
