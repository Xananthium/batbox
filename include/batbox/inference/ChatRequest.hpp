// include/batbox/inference/ChatRequest.hpp
// ---------------------------------------------------------------------------
// OpenAI-compatible /v1/chat/completions request wire models.
//
// Types (all in namespace batbox::inference):
//   WireMessage     — one messages[] entry (role + content + tool_calls + tool_call_id + name)
//   WireToolCall    — tool_calls[] entry in an assistant message (id + function{name,arguments})
//   ToolDef         — one tools[] entry (type + function{name,description,schema})
//   ChatRequest     — the full POST body sent to the completions endpoint
//
// Serialisation strategy:
//   All four types implement free-function to_json / from_json hooks so that
//   nlohmann::json can serialise and deserialise them with standard syntax:
//
//     batbox::Json j = req;            // calls to_json
//     ChatRequest req2 = j;            // calls from_json
//
//   Optional fields are omitted from the serialised object when they have no
//   value and round-trip back to std::nullopt when absent in the parsed JSON.
//
// tool_choice wire encoding:
//   The OpenAI API accepts three forms:
//     "none"               — model must not call tools
//     "auto"               — model decides
//     {"type":"function","function":{"name":"<n>"}} — force a specific tool
//   ChatRequest::tool_choice is std::optional<std::string> where:
//     std::nullopt     → field omitted (same as "auto" for most providers)
//     "none" / "auto"  → serialised as a JSON string
//     "function:<n>"   → serialised as the object form {type,function{name}}
//   Use the helper ChatRequest::tool_choice_auto(), tool_choice_none(), and
//   tool_choice_function(name) to set the field unambiguously.
//
// stream_options:
//   When stream=true AND stream_options_include_usage=true the serialiser emits
//   "stream_options":{"include_usage":true} which causes the OpenAI API to
//   include a usage chunk as the final SSE event before [DONE].
//
// Build (standalone, no CMake — from repo root):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_chat_wire_model.cpp src/inference/ChatRequest.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       -o /tmp/test_chat_wire && /tmp/test_chat_wire
// ---------------------------------------------------------------------------

#pragma once

#include <batbox/core/Json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace batbox::inference {

// ============================================================================
// WireToolCall — tool_calls[] entry in an assistant wire message
// ============================================================================

/// Represents a single tool invocation emitted by the model in an assistant
/// message.  The function sub-object carries the resolved name and the JSON
/// arguments as a raw string (not yet parsed — parsing happens in
/// ToolCallAccumulator when the stream is complete).
struct WireToolCall {
    /// Provider-assigned call ID, e.g. "call_abc123".
    std::string id;

    struct Function {
        /// Tool name as declared in tools[].
        std::string name;
        /// Raw JSON string of the arguments object, e.g. "{\"path\":\"/tmp\"}".
        std::string arguments;
    } function;
};

void to_json(Json& j, const WireToolCall& tc);
void from_json(const Json& j, WireToolCall& tc);

// ============================================================================
// WireMessage — one messages[] entry
// ============================================================================

/// Represents a single turn in the conversation history as sent over the wire.
/// The role field must be one of: "system", "user", "assistant", "tool".
///
/// Field presence rules (matching OpenAI spec):
///   system      → content required; others absent
///   user        → content required; others absent
///   assistant   → content and/or tool_calls present; name optional
///   tool        → content required; tool_call_id required; name optional
struct WireMessage {
    /// "system" | "user" | "assistant" | "tool"
    std::string role;

    /// The text body.  For assistant messages the model may emit null content
    /// (with tool_calls instead) — hence optional.
    std::optional<std::string> content;

    /// Tool invocations emitted by the model (assistant messages only).
    std::optional<std::vector<WireToolCall>> tool_calls;

    /// Correlation ID linking this message to a tool call (tool role only).
    std::optional<std::string> tool_call_id;

    /// Display name for multi-agent scenarios; provider-specific.
    std::optional<std::string> name;
};

void to_json(Json& j, const WireMessage& msg);
void from_json(const Json& j, WireMessage& msg);

// ============================================================================
// ToolDef — one tools[] entry describing a callable function
// ============================================================================

/// Describes a tool the model may call.  The schema field is the raw JSON
/// Schema object that defines the arguments — it is passed through verbatim
/// so callers can supply any valid schema without re-encoding.
///
/// Wire shape:
///   { "type": "function",
///     "function": { "name": "...", "description": "...", "parameters": <schema> } }
struct ToolDef {
    /// Always "function" per the OpenAI spec.
    std::string type{"function"};

    /// The tool's canonical name; must match ITool::name().
    std::string name;

    /// Human-readable description sent to the model.
    std::string description;

    /// nlohmann::json object containing the JSON Schema for the parameters.
    Json schema;
};

void to_json(Json& j, const ToolDef& td);
void from_json(const Json& j, ToolDef& td);

// ============================================================================
// ChatRequest — the full POST body
// ============================================================================

/// The complete request body for POST /v1/chat/completions.
///
/// tool_choice encoding — use the three static helpers to avoid ambiguity:
///   ChatRequest::tool_choice_auto()          → nullopt (field omitted)
///   ChatRequest::tool_choice_none()          → "none"
///   ChatRequest::tool_choice_function("n")  → "function:n"
///
/// Serialisation omits optional fields when they hold no value so that
/// providers that reject unknown-or-extra fields remain compatible.
struct ChatRequest {
    // -----------------------------------------------------------------------
    // Required fields
    // -----------------------------------------------------------------------

    /// Model identifier, e.g. "gpt-4o", "claude-3-5-sonnet-20241022".
    std::string model;

    /// Ordered conversation turns.
    std::vector<WireMessage> messages;

    // -----------------------------------------------------------------------
    // Optional fields (omitted when not set)
    // -----------------------------------------------------------------------

    /// Tool definitions made available to the model.  Omitted when empty.
    std::vector<ToolDef> tools;

    /// Controls which (if any) tool the model must call.  Encoding:
    ///   std::nullopt       → field omitted (provider default; usually "auto")
    ///   "none"             → serialised as JSON string "none"
    ///   "auto"             → serialised as JSON string "auto"
    ///   "function:<name>"  → serialised as {"type":"function","function":{"name":"<name>"}}
    std::optional<std::string> tool_choice;

    /// Hard cap on generated tokens.  Omitted when not set.
    std::optional<int> max_tokens;

    /// Sampling temperature [0,2].  Omitted when not set.
    std::optional<double> temperature;

    /// Nucleus sampling probability mass.  Omitted when not set.
    std::optional<double> top_p;

    // -----------------------------------------------------------------------
    // Streaming
    // -----------------------------------------------------------------------

    /// Enable SSE streaming.  Defaults to true per the batbox design.
    bool stream{true};

    /// When stream=true: request a final usage chunk from the API.
    /// Serialised as "stream_options":{"include_usage":true}.
    std::optional<bool> stream_options_include_usage{true};

    // -----------------------------------------------------------------------
    // tool_choice helpers
    // -----------------------------------------------------------------------

    /// Return a value that serialises as the "auto" string (field omitted;
    /// "auto" is the provider default when tool_choice is absent).
    static std::optional<std::string> tool_choice_auto() noexcept {
        return std::nullopt;
    }

    /// Return a value that serialises as the JSON string "none".
    static std::optional<std::string> tool_choice_none() noexcept {
        return std::string{"none"};
    }

    /// Return a value that forces the model to call the named tool.
    /// Serialised as {"type":"function","function":{"name":"<name>"}}.
    static std::optional<std::string> tool_choice_function(std::string name) {
        return "function:" + std::move(name);
    }
};

void to_json(Json& j, const ChatRequest& req);
void from_json(const Json& j, ChatRequest& req);

} // namespace batbox::inference
