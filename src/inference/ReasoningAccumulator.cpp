// src/inference/ReasoningAccumulator.cpp
// ---------------------------------------------------------------------------
// Implementation of batbox::inference::ReasoningAccumulator.  See the header
// for the unified-channel contract and the no-double-count invariant.
// ---------------------------------------------------------------------------

#include <batbox/inference/ReasoningAccumulator.hpp>

namespace batbox::inference {

ReasoningAccumulator::ReasoningAccumulator(ReasoningTags tags)
    : splitter_(std::move(tags)) {}

std::string ReasoningAccumulator::accumulate(const StreamDelta& delta) {
    std::string produced_visible;

    // Source 1 — STRUCTURED reasoning field.  Isolated directly; never visible.
    // Mirrors Client's content-only telemetry: empty strings are not real tokens.
    if (delta.reasoning_content.has_value() && !delta.reasoning_content->empty()) {
        reasoning_ += *delta.reasoning_content;
    }

    // Source 2 — INLINE reasoning carried inside content.  The ThinkSplitter
    // separates visible from `<think>…</think>` text.  When tags are disabled
    // the splitter is a pass-through (all content is visible).
    if (delta.content.has_value() && !delta.content->empty()) {
        ThinkSplit split = splitter_.push(*delta.content);
        produced_visible += split.visible;
        reasoning_       += split.reasoning;
    }

    visible_ += produced_visible;
    return produced_visible;
}

std::string ReasoningAccumulator::finish() {
    ThinkSplit split = splitter_.finish();
    reasoning_ += split.reasoning;
    visible_   += split.visible;
    return split.visible;
}

} // namespace batbox::inference
