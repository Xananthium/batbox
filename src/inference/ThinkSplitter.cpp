// src/inference/ThinkSplitter.cpp
// ---------------------------------------------------------------------------
// Implementation of batbox::inference::ThinkSplitter.  See ThinkSplitter.hpp
// for the design, the incremental algorithm, and the edge-case contract.
// ---------------------------------------------------------------------------

#include <batbox/inference/ThinkSplitter.hpp>

namespace batbox::inference {

ThinkSplitter::ThinkSplitter(ReasoningTags tags)
    : tags_(std::move(tags)),
      enabled_(tags_.enabled()) {}

const std::string& ThinkSplitter::active_marker() const noexcept {
    return (state_ == State::Visible) ? tags_.open : tags_.close;
}

void ThinkSplitter::emit(ThinkSplit& out, char ch) const {
    if (state_ == State::Visible) {
        out.visible += ch;
    } else {
        out.reasoning += ch;
    }
}

void ThinkSplitter::feed(char c, ThinkSplit& out) {
    const std::string& marker = active_marker();

    pending_ += c;

    // Shrink pending_ until it is once again a prefix of the active marker.
    // Each byte that can no longer begin a match is confirmed sink text.
    // Re-testing the remaining tail after each drop tries every possible start
    // position, which is correct even for self-overlapping markers.
    while (!pending_.empty()) {
        const bool is_prefix =
            pending_.size() <= marker.size() &&
            marker.compare(0, pending_.size(), pending_) == 0;
        if (is_prefix) {
            break;
        }
        emit(out, pending_.front());
        pending_.erase(pending_.begin());
    }

    // A full marker match (pending_ is a prefix of marker of equal length, hence
    // equal to marker) completes the delimiter: consume it (emit to neither
    // sink) and toggle state.
    if (pending_.size() == marker.size()) {
        pending_.clear();
        state_ = (state_ == State::Visible) ? State::Reasoning : State::Visible;
    }
}

ThinkSplit ThinkSplitter::push(std::string_view fragment) {
    ThinkSplit out;

    // Disabled: pure pass-through, no buffering.
    if (!enabled_) {
        out.visible.assign(fragment.begin(), fragment.end());
        return out;
    }

    for (char c : fragment) {
        feed(c, out);
    }
    return out;
}

ThinkSplit ThinkSplitter::finish() {
    ThinkSplit out;
    if (!enabled_) {
        return out;
    }
    // Flush the buffered partial-marker tail to the current sink.  Outside a
    // block this is trailing text that merely looked like a tag start; inside an
    // unclosed block it is the remainder of the reasoning.
    for (char c : pending_) {
        emit(out, c);
    }
    pending_.clear();
    return out;
}

} // namespace batbox::inference
