// src/tools/ToolSubagentEnvelope.cpp
//
// Implementation of batbox::tools::ToolSubagentEnvelope.
//
// See include/batbox/tools/ToolSubagentEnvelope.hpp for the full contract and
// the rationale for why this is batbox's single subagent-dispatch seam.

#include <batbox/tools/ToolSubagentEnvelope.hpp>

#include <utility>

namespace batbox::tools {

// =============================================================================
// Construction
// =============================================================================

ToolSubagentEnvelope::ToolSubagentEnvelope()
    : decider_(std::make_shared<PassThroughDecider>())
    , distiller_(std::make_shared<IdentityDistiller>())
{}

ToolSubagentEnvelope::ToolSubagentEnvelope(
        std::shared_ptr<IEngulfDecider>   decider,
        std::shared_ptr<IResultDistiller> distiller)
    : decider_(decider ? std::move(decider)
                       : std::shared_ptr<IEngulfDecider>(std::make_shared<PassThroughDecider>()))
    , distiller_(distiller ? std::move(distiller)
                           : std::shared_ptr<IResultDistiller>(std::make_shared<IdentityDistiller>()))
{}

// =============================================================================
// process — the universal seam
// =============================================================================

ToolResult ToolSubagentEnvelope::process(std::string_view tool_name,
                                         const Json&      args,
                                         ToolResult       result,
                                         ToolContext&     ctx) const {
    // Decision hook: should this result be engulfed into a subagent?
    // S7 default (PassThroughDecider) always returns false → pure pass-through.
    if (decider_->should_engulf(tool_name, args, result, ctx)) {
        // Distiller hook: engulf + distill to the golden line.
        // S7 default (IdentityDistiller) returns the result unchanged.
        return distiller_->distill(tool_name, args, std::move(result), ctx);
    }
    return result;
}

// =============================================================================
// Hook installation
// =============================================================================

void ToolSubagentEnvelope::set_decider(std::shared_ptr<IEngulfDecider> decider) {
    // Reject null to preserve the never-null invariant.
    if (decider) {
        decider_ = std::move(decider);
    }
}

void ToolSubagentEnvelope::set_distiller(std::shared_ptr<IResultDistiller> distiller) {
    if (distiller) {
        distiller_ = std::move(distiller);
    }
}

} // namespace batbox::tools
