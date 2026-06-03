// include/batbox/tools/ThresholdEngulfDecider.hpp
//
// batbox::tools::ThresholdEngulfDecider — the S1 threshold-gated engulf trigger
// (DIS-980).  Fills the IEngulfDecider hook the S7 envelope left as a
// PassThroughDecider, WITHOUT touching the seam (installed via
// ToolRegistry::envelope().set_decider).
//
// Ported from goose's large_response_handler.rs::process_tool_response, which
// checks a tool's text against large_text_threshold() (200k chars, env
// GOOSE_MAX_TOOL_RESPONSE_SIZE) and intervenes when it is exceeded.  batbox
// keeps the GATE and replaces the ACTION: goose spills-and-points (the model
// must re-read = re-pay); batbox routes the same trigger into a closed
// tool-subagent that reads the big output and returns only the golden line
// (the SubagentDistiller, S4).
//
// Decisions of record (see the issue's S1 scope):
//   * Size is the trigger, NOT tool identity.  A read-only tool's huge output
//     still engulfs — the cost is the bytes entering the parent context,
//     regardless of which tool produced them.
//   * Error results are NEVER engulfed.  The model must see an error verbatim
//     to self-correct, and distilling an error would hide the failure.
//   * Strictly-greater-than: a result exactly AT the threshold is small enough
//     to inline; only output that EXCEEDS it is engulfed.
//
// Contract: should_engulf MUST stay cheap and side-effect-free — it runs on
// every dispatched tool result, on the dispatch thread.

#pragma once

#include <batbox/tools/ToolSubagentEnvelope.hpp>

#include <cstddef>
#include <string_view>

namespace batbox::tools {

class ThresholdEngulfDecider final : public IEngulfDecider {
public:
    /// @param max_response_bytes  Byte threshold; a non-error result whose body
    ///                            size strictly exceeds this is engulfed.
    explicit ThresholdEngulfDecider(std::size_t max_response_bytes) noexcept
        : max_response_bytes_(max_response_bytes) {}

    [[nodiscard]] bool should_engulf(std::string_view  tool_name,
                                     const Json&        args,
                                     const ToolResult&  result,
                                     const ToolContext& ctx) const override;

    /// The configured threshold (exposed for diagnostics / tests).
    [[nodiscard]] std::size_t threshold() const noexcept { return max_response_bytes_; }

private:
    std::size_t max_response_bytes_;
};

} // namespace batbox::tools
