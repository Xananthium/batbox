// src/tools/ThresholdEngulfDecider.cpp
//
// Implementation of batbox::tools::ThresholdEngulfDecider.
// See include/batbox/tools/ThresholdEngulfDecider.hpp for the full contract.

#include <batbox/tools/ThresholdEngulfDecider.hpp>

namespace batbox::tools {

bool ThresholdEngulfDecider::should_engulf(std::string_view  /*tool_name*/,
                                           const Json&        /*args*/,
                                           const ToolResult&  result,
                                           const ToolContext& /*ctx*/) const {
    // Never engulf an error: the model needs the error verbatim to self-correct,
    // and distilling it would hide the failure.
    if (result.is_error) {
        return false;
    }
    // Size is the trigger, not tool identity.  Strictly-greater: a result AT the
    // threshold is small enough to inline.
    return result.body.size() > max_response_bytes_;
}

} // namespace batbox::tools
