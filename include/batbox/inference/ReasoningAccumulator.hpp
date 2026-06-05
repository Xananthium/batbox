// include/batbox/inference/ReasoningAccumulator.hpp
// ---------------------------------------------------------------------------
// batbox::inference — ReasoningAccumulator (S10).
//
// The UNIFIED isolated reasoning channel.  It merges the model's reasoning into
// a single retrievable stream regardless of which wire form the endpoint used:
//
//   1. STRUCTURED — delta.reasoning_content (Magistral/Qwen3/DeepSeek emit a
//      dedicated wire field; already parsed by StreamDelta::from_json).
//   2. INLINE     — reasoning wrapped in `<think>…</think>` inside delta.content
//      (DeepSeek-R1 over raw endpoints, some Qwen/ollama configs); extracted by
//      the owned ThinkSplitter.
//
// Contract:
//   - Visible output is GUARANTEED reasoning-free no matter which form the model
//     used: structured reasoning never touches visible; inline reasoning is
//     stripped out by the ThinkSplitter.
//   - The full isolated reasoning text is retrievable via reasoning().  This is
//     the feedstock the notepad / future z-distillation will read.
//   - No double-counting: the two sources are disjoint by construction — a given
//     wire byte is either in reasoning_content, or in content (and within
//     content, either inside or outside the think tags).  Each byte is
//     classified exactly once.  A delta that carries BOTH forms contributes each
//     once.
//
// Sibling to ToolCallAccumulator (per-index tool_call reassembly).  Where that
// type turns ToolCallDelta fragments into ToolCalls, this one turns the reasoning
// halves of a StreamDelta into one isolated reasoning stream plus the clean
// visible text.
//
// Thread safety: NOT thread-safe.  One instance per streaming request.
//
// Build (standalone, no CMake — from repo root):
//   c++ -std=c++20 -I include \
//       tests/unit/test_reasoning_accumulator.cpp \
//       src/inference/ReasoningAccumulator.cpp \
//       src/inference/ThinkSplitter.cpp \
//       src/inference/ChatRequest.cpp \
//       build/vcpkg_installed/x64-linux/lib/libsimdjson.a \
//       -o /tmp/test_ra && /tmp/test_ra
// ---------------------------------------------------------------------------

#pragma once

#include <batbox/inference/ChatResponse.hpp>   // StreamDelta
#include <batbox/inference/ThinkSplitter.hpp>   // ReasoningTags, ThinkSplit

#include <string>

namespace batbox::inference {

class ReasoningAccumulator {
public:
    /// Construct with the provider's inline-tag convention.  Defaults to
    /// `<think>`/`</think>`.  Pass ReasoningTags::none() for a provider that
    /// uses the structured field only — inline splitting is then a pass-through
    /// (content is treated entirely as visible), while structured
    /// reasoning_content is still isolated.
    explicit ReasoningAccumulator(ReasoningTags tags = ReasoningTags{});

    // Non-copyable; movable.
    ReasoningAccumulator(const ReasoningAccumulator&)            = delete;
    ReasoningAccumulator& operator=(const ReasoningAccumulator&) = delete;
    ReasoningAccumulator(ReasoningAccumulator&&)                 = default;
    ReasoningAccumulator& operator=(ReasoningAccumulator&&)      = default;

    /// Process one streaming delta.  Routes delta.reasoning_content into the
    /// isolated reasoning channel directly, and delta.content through the
    /// ThinkSplitter (visible → visible buffer; inline `<think>` text → reasoning
    /// channel).  Returns ONLY the visible text produced by this delta, so the
    /// caller can forward it verbatim to the UI / accumulate the assistant
    /// message.  Reasoning is captured internally; read it via reasoning().
    [[nodiscard]] std::string accumulate(const StreamDelta& delta);

    /// Flush at end of stream.  Drains any buffered partial-tag tail from the
    /// ThinkSplitter (an unclosed inline block becomes reasoning).  Returns the
    /// final visible text produced, if any.
    [[nodiscard]] std::string finish();

    /// The full isolated reasoning text accumulated so far (both sources merged
    /// in arrival order).
    [[nodiscard]] const std::string& reasoning() const noexcept { return reasoning_; }

    /// The full visible (reasoning-free) text accumulated so far.
    [[nodiscard]] const std::string& visible() const noexcept { return visible_; }

    /// True when any reasoning text has been isolated from either source.
    [[nodiscard]] bool has_reasoning() const noexcept { return !reasoning_.empty(); }

    /// True while the inline splitter is inside a `<think>…` block.
    [[nodiscard]] bool in_reasoning_block() const noexcept {
        return splitter_.in_reasoning();
    }

private:
    ThinkSplitter splitter_;
    std::string   reasoning_;
    std::string   visible_;
};

} // namespace batbox::inference
