// src/inference/ToolCallAccumulator.cpp
// =============================================================================
// Implementation of batbox::inference::ToolCallAccumulator.
//
// OpenAI tool_call streaming protocol:
//   choices[0].delta.tool_calls[] is an array of fragments, each carrying:
//     index            — which parallel tool call this fragment belongs to
//     id               — present only in the FIRST fragment for that index
//     function.name    — present only in the FIRST fragment for that index
//     function.arguments — raw JSON string fragment; append across chunks
//
//   On finish_reason="tool_calls" the caller invokes finalize().  We
//   JSON-parse each accumulated arguments buffer once.  Malformed JSON in one
//   buffer does NOT prevent the others from being returned — the bad entry gets
//   a parse_error string so the model can receive it as an error tool_result
//   and self-correct.
// =============================================================================

#include <batbox/inference/ToolCallAccumulator.hpp>
#include <batbox/core/Logging.hpp>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace batbox::inference {

// =============================================================================
// ToolCallAccumulator::accumulate
// =============================================================================
void ToolCallAccumulator::accumulate(const ToolCallDelta& delta) {
    CallBuffer& buf = buffers_[delta.index];

    // id: set-once — first non-empty value wins; later deltas without id are
    // the common case and should not overwrite.
    if (buf.id.empty() && delta.id.has_value() && !delta.id->empty()) {
        buf.id = *delta.id;
    }

    // name: set-once — same logic as id.
    if (buf.name.empty() && delta.name.has_value() && !delta.name->empty()) {
        buf.name = *delta.name;
    }

    // arguments_fragment: always append, even empty strings (first chunk often
    // carries an empty fragment alongside id/name).
    if (delta.arguments_fragment.has_value()) {
        buf.arguments_buf.append(*delta.arguments_fragment);
    }
}

// =============================================================================
// ToolCallAccumulator::finalize
// =============================================================================
Result<std::vector<ToolCall>> ToolCallAccumulator::finalize() {
    auto lg = batbox::log::get("tool_call_accumulator");

    std::vector<ToolCall> calls;
    calls.reserve(buffers_.size());

    // buffers_ is a std::map keyed by index — iteration is in ascending key
    // order, so the output vector is naturally index-ordered.
    for (auto& [index, buf] : buffers_) {
        ToolCall call;
        call.id   = buf.id;
        call.name = buf.name;

        if (buf.arguments_buf.empty()) {
            // An empty arguments string is a valid JSON-null-equivalent only if
            // explicitly empty; treat it as an empty object rather than a parse
            // error — some tools have no parameters.
            call.arguments = Json::object();
        } else {
            try {
                call.arguments = Json::parse(buf.arguments_buf);
            } catch (const Json::parse_error& e) {
                call.parse_error = e.what();
                call.arguments   = Json{};   // null Json
                lg->warn(
                    "ToolCallAccumulator: index={} name='{}' arguments JSON "
                    "parse failed: {}  raw_buf='{}'",
                    index, buf.name, e.what(), buf.arguments_buf);
            }
        }

        calls.push_back(std::move(call));
    }

    return calls;
}

} // namespace batbox::inference
