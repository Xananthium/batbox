// include/batbox/inference/ToolCallAccumulator.hpp
// ---------------------------------------------------------------------------
// Per-index tool_call delta accumulator for OpenAI-compatible streaming.
//
// OpenAI streaming protocol for tool_calls:
//   Each SSE chunk may carry one or more ToolCallDelta entries.  The first
//   delta for a given index carries the call id and function name; subsequent
//   deltas carry only arguments_fragment.  The full arguments string is the
//   concatenation of all fragments in arrival order.
//
//   Example for a single tool call spread across 5 SSE events:
//     chunk 1: index=0, id="call_abc", name="get_weather", arguments_fragment=""
//     chunk 2: index=0, arguments_fragment="{\"loc"
//     chunk 3: index=0, arguments_fragment="ation\":"
//     chunk 4: index=0, arguments_fragment="\"Paris\"}"
//     chunk 5: (finish_reason="tool_calls" — caller invokes finalize())
//
//   finalize() JSON-parses each per-index arguments buffer once and returns a
//   vector of ToolCall values.  If an individual buffer is malformed JSON, that
//   entry carries an error string in `parse_error` rather than poisoning the
//   whole vector; other valid calls remain intact.
//
// Usage:
//   ToolCallAccumulator acc;
//   for (auto& delta : stream_delta.tool_calls.value())
//       acc.accumulate(delta);
//   // ... on finish_reason == "tool_calls":
//   auto result = acc.finalize();
//   if (result) {
//       for (auto& call : result.value()) { ... }
//   }
//
// Thread safety: NOT thread-safe.  Instantiate one accumulator per streaming
// request; do not share across threads.
//
// Build (standalone, no CMake — from repo root):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_tool_call_accumulator.cpp \
//       src/inference/ToolCallAccumulator.cpp \
//       src/inference/ChatRequest.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       -o /tmp/test_tca && /tmp/test_tca
// ---------------------------------------------------------------------------

#pragma once

#include <batbox/core/Json.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/inference/ChatResponse.hpp>   // ToolCallDelta

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace batbox::inference {

// ============================================================================
// ToolCall — finalized tool call produced by ToolCallAccumulator::finalize()
// ============================================================================

/// A fully-assembled tool call ready for dispatch to the tool engine.
///
/// After ToolCallAccumulator::finalize() succeeds, arguments holds the parsed
/// JSON object.  If arguments_fragment concatenation produced malformed JSON,
/// parse_error holds the error message and arguments is null — the caller
/// should surface the error as a tool_result so the model can self-correct.
struct ToolCall {
    /// Provider-assigned call ID (e.g. "call_abc123").  Empty if the stream
    /// never delivered an id field for this index (malformed stream).
    std::string id;

    /// Function name as declared in tools[].  Empty if name was never received.
    std::string name;

    /// Parsed JSON arguments object.  Null when parse_error is non-empty.
    Json arguments;

    /// Non-empty when arguments_fragment concatenation failed JSON parsing.
    /// When set, arguments is left as a null Json value.
    std::string parse_error;
};

// ============================================================================
// ToolCallAccumulator — per-index streaming delta reassembler
// ============================================================================

/// Accumulates ToolCallDelta fragments keyed by index and finalises them into
/// a vector of ToolCall values when the stream reaches finish_reason="tool_calls".
///
/// Lifetime: one instance per streaming request; not reusable after finalize().
class ToolCallAccumulator {
public:
    ToolCallAccumulator() = default;

    // Non-copyable; movable.
    ToolCallAccumulator(const ToolCallAccumulator&)            = delete;
    ToolCallAccumulator& operator=(const ToolCallAccumulator&) = delete;
    ToolCallAccumulator(ToolCallAccumulator&&)                 = default;
    ToolCallAccumulator& operator=(ToolCallAccumulator&&)      = default;

    // -----------------------------------------------------------------------
    // accumulate — absorb one streaming delta fragment
    //
    // Rules:
    //   - delta.id:   set-once per index; later fragments with a non-empty id
    //                 are silently ignored (first writer wins).
    //   - delta.name: set-once per index; later fragments with a non-empty name
    //                 are silently ignored.
    //   - delta.arguments_fragment: appended to the per-index buffer regardless.
    //
    // May be called many times before finalize().
    // -----------------------------------------------------------------------
    void accumulate(const ToolCallDelta& delta);

    // -----------------------------------------------------------------------
    // finalize — parse all accumulated argument buffers and return ToolCalls
    //
    // Each per-index buffer is parsed with nlohmann::json::parse().  A buffer
    // that fails to parse produces a ToolCall with parse_error set and
    // arguments == null; the other calls in the vector are unaffected.
    //
    // Returns Ok(vector<ToolCall>) ordered by ascending index.
    // The outer Result is always Ok (errors are per-call, not global); the
    // Result wrapper is retained for API uniformity with other inference types.
    //
    // The accumulator must not be used after finalize() is called.
    // -----------------------------------------------------------------------
    [[nodiscard]] Result<std::vector<ToolCall>> finalize();

private:
    // Per-index accumulation buffer.
    struct CallBuffer {
        std::string id;
        std::string name;
        std::string arguments_buf;
    };

    // Ordered map so finalize() produces calls in ascending index order without
    // an explicit sort step.
    std::map<int, CallBuffer> buffers_;
};

} // namespace batbox::inference
