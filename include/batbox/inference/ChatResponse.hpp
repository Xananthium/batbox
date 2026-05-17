// include/batbox/inference/ChatResponse.hpp
// ---------------------------------------------------------------------------
// OpenAI-compatible /v1/chat/completions response wire models.
//
// Types (all in namespace batbox::inference):
//   UsageDelta      — token usage counts (prompt + completion + total)
//   ToolCallDelta   — one streaming tool_call fragment keyed by index
//   StreamDelta     — one SSE chunk's delta object (content XOR tool_calls XOR finish)
//   ChatResponse    — complete non-streaming response (used by Client::chat())
//
// Streaming model:
//   OpenAI SSE streams emit events shaped as:
//     {"id":"...", "object":"chat.completion.chunk",
//      "choices":[{"index":0,"delta":{...},"finish_reason":null|"stop"|"tool_calls"}]}
//
//   Each delta object maps to StreamDelta.  The caller (ToolCallAccumulator)
//   accumulates ToolCallDelta fragments across chunks keyed by ToolCallDelta::index
//   until finish_reason="tool_calls", at which point arguments_fragment buffers
//   are JSON-parsed.
//
//   The final chunk (when stream_options.include_usage=true) carries an empty
//   delta and a top-level "usage" field; StreamDelta::usage captures that.
//
// Non-streaming model:
//   A complete response maps to ChatResponse.  The id, model, finish_reason,
//   and usage fields are all present.  content and tool_calls are mutually
//   exclusive (only one should be non-null for a given finish_reason).
//
// Serialisation:
//   All types implement free-function to_json / from_json hooks.  Optional
//   fields are omitted when absent in serialisation and round-trip cleanly.
//
// Build (standalone, no CMake — from repo root):
//   see ChatRequest.hpp build comment — same compilation unit.
// ---------------------------------------------------------------------------

#pragma once

#include <batbox/core/Json.hpp>
#include <batbox/inference/ChatRequest.hpp>   // for WireToolCall

#include <optional>
#include <string>
#include <vector>

namespace batbox::inference {

// ============================================================================
// UsageDelta — token counts reported by the API
// ============================================================================

/// Token usage for one API call.  Emitted in the final streaming chunk when
/// stream_options.include_usage=true, or in the top-level non-streaming
/// response object.
///
/// The cost_usd field is computed locally by UsageTracker and is never
/// deserialised from the wire — it defaults to 0.0.
struct UsageDelta {
    int prompt_tokens{0};
    int completion_tokens{0};
    int total_tokens{0};

    /// Locally-computed cost in USD.  Not present on the wire; populated by
    /// UsageTracker::charge() after the request completes.
    double cost_usd{0.0};
};

void to_json(Json& j, const UsageDelta& u);
void from_json(const Json& j, UsageDelta& u);

// ============================================================================
// ToolCallDelta — streaming tool_call fragment
// ============================================================================

/// One element of choices[0].delta.tool_calls[] in a streaming chunk.
/// Fragments arrive across multiple SSE events and are accumulated by
/// ToolCallAccumulator keyed on index.
///
/// Field presence rules per chunk:
///   First chunk for a call  → id and name are present; arguments_fragment may be ""
///   Subsequent chunks       → only arguments_fragment is present
///   All chunks              → index is always present
struct ToolCallDelta {
    /// Position in the tool_calls array; used as the accumulation key.
    int index{0};

    /// Provider-assigned call ID.  Present only in the first fragment.
    std::optional<std::string> id;

    /// Tool name.  Present only in the first fragment.
    std::optional<std::string> name;

    /// One raw string fragment of the arguments JSON.  Concatenate all
    /// fragments in index order to obtain the full arguments string.
    std::optional<std::string> arguments_fragment;
};

void to_json(Json& j, const ToolCallDelta& tcd);
void from_json(const Json& j, ToolCallDelta& tcd);

// ============================================================================
// StreamDelta — one SSE chunk's delta payload
// ============================================================================

/// The delta object from a single choices[0] entry in a streaming chunk.
///
/// Exactly one of content, tool_calls, or finish_reason is non-null per chunk
/// in normal operation (though the API occasionally emits a chunk where only
/// finish_reason is set and content/tool_calls are absent).
///
/// usage is populated from the top-level "usage" field of the final chunk
/// (only when stream_options.include_usage=true was sent in the request).
///
/// reasoning_content carries internal chain-of-thought text emitted by
/// reasoning models (Magistral, Qwen3, DeepSeek) before visible content.
/// It is NOT delivered to the user; it exists only so the stream_started
/// retry gate can recognise that the server has begun responding.
struct StreamDelta {
    /// Text content fragment.  Present on content-streaming chunks.
    std::optional<std::string> content;

    /// Reasoning/chain-of-thought fragment from reasoning models (Magistral,
    /// Qwen3, DeepSeek).  Emitted under delta.reasoning_content before any
    /// delta.content arrives.  Not user-visible; used by the retry gate.
    std::optional<std::string> reasoning_content;

    /// Tool-call fragments.  Present when the model is accumulating tool args.
    std::optional<std::vector<ToolCallDelta>> tool_calls;

    /// Termination reason.  "stop" | "tool_calls" | "length" | "content_filter".
    /// Present on the final content chunk; absent on all earlier chunks.
    std::optional<std::string> finish_reason;

    /// Token usage from the final chunk.  Absent on all non-terminal chunks.
    std::optional<UsageDelta> usage;
};

void to_json(Json& j, const StreamDelta& sd);
void from_json(const Json& j, StreamDelta& sd);

// ============================================================================
// ChatResponse — complete non-streaming response
// ============================================================================

/// The full response body for a non-streaming POST /v1/chat/completions call.
/// Used by Client::chat() (the synchronous, non-streaming overload).
///
/// content and tool_calls are mutually exclusive:
///   finish_reason == "stop"        → content is present; tool_calls absent
///   finish_reason == "tool_calls"  → tool_calls present; content may be null
struct ChatResponse {
    /// Provider-assigned response ID, e.g. "chatcmpl-abc123".
    std::string id;

    /// Model that generated this response, echoed from the request.
    std::string model;

    /// Generated text.  Null when finish_reason is "tool_calls".
    std::optional<std::string> content;

    /// Tool calls requested by the model.  Null when finish_reason is "stop".
    std::optional<std::vector<WireToolCall>> tool_calls;

    /// Why generation stopped: "stop" | "tool_calls" | "length" | "content_filter".
    std::string finish_reason;

    /// Token usage for this request.
    UsageDelta usage;
};

void to_json(Json& j, const ChatResponse& cr);
void from_json(const Json& j, ChatResponse& cr);

} // namespace batbox::inference
