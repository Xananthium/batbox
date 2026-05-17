// include/batbox/conversation/Message.hpp
// =============================================================================
// batbox canonical conversation message model (CPP 3.1).
//
// This is the internal representation used throughout the conversation engine.
// It is deliberately separate from the wire format (WireMessage in C4) so that
// internal fields (id, ts, is_error) do not leak into API payloads.
//
// Structs defined here:
//   batbox::conversation::UsageDelta   — token counts + cost for one API call
//   batbox::conversation::ToolCall     — a single tool invocation from the model
//   batbox::conversation::Message      — one turn in a conversation
//
// Free functions (all in batbox::conversation):
//   from_json(Json)       → Message          (deserialise from stored JSON)
//   to_json(Message)      → Json             (serialise for storage / debug)
//   to_wire_role(Role)    → std::string      ("system"|"user"|"assistant"|"tool")
//   role_from_string(sv)  → Role             (parse from wire string, throws on unknown)
//
// Message helpers:
//   msg.text()            → std::string_view — the content field (convenience alias)
//   msg.is_tool_call()    → bool             — true when role==Assistant && tool_calls present
//   msg.is_tool_result()  → bool             — true when role==Tool
//
// Build standalone (no CMake, from repo root):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_message_model.cpp src/conversation/Message.cpp \
//       src/core/Uuid.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       -o /tmp/test_message && /tmp/test_message
// =============================================================================

#pragma once

#include <batbox/core/Json.hpp>
#include <batbox/core/Uuid.hpp>

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace batbox::conversation {

// =============================================================================
// UsageDelta — token counts and estimated cost from one inference call
// =============================================================================

struct UsageDelta {
    int    prompt_tokens     = 0;
    int    completion_tokens = 0;
    int    total_tokens      = 0;
    double cost_usd          = 0.0;
};

// =============================================================================
// ToolCall — a single tool invocation emitted by an assistant message
// =============================================================================

struct ToolCall {
    /// Provider-assigned call id (e.g. "call_abc123" from OpenAI).
    std::string id;
    /// Tool name as registered in the ToolRegistry.
    std::string name;
    /// Arguments as a parsed JSON object.  While streaming, this may hold
    /// partial JSON; callers should validate before dispatch.
    Json arguments;
};

// =============================================================================
// Message::Role — maps to wire "role" strings
// =============================================================================

enum class Role : std::uint8_t {
    System    = 0,
    User      = 1,
    Assistant = 2,
    Tool      = 3,
};

/// Convert Role to the wire string ("system", "user", "assistant", "tool").
[[nodiscard]] std::string_view to_wire_role(Role r) noexcept;

/// Parse a wire role string into Role.  Throws std::invalid_argument on unknown input.
[[nodiscard]] Role role_from_string(std::string_view s);

// =============================================================================
// Message — canonical internal representation of one conversation turn
// =============================================================================

struct Message {
    // ---- Identity -----------------------------------------------------------

    /// UUID v4, generated on construction.
    std::string id;

    // ---- Role + content -----------------------------------------------------

    Role role = Role::User;

    /// Markdown text for user/assistant messages.
    /// Tool-result body for Role::Tool messages.
    std::string content;

    // ---- Tool call fields (assistant emissions) -----------------------------

    /// Present when role==Assistant and the model requested one or more tool
    /// calls.  Each element represents one invocation to dispatch.
    std::optional<std::vector<ToolCall>> tool_calls;

    // ---- Tool result correlation (Role::Tool messages) ----------------------

    /// The call id this result is correlated to (provider-assigned).
    std::optional<std::string> tool_call_id;

    /// The name of the tool that produced this result.
    std::optional<std::string> tool_name;

    /// True when the tool raised an error rather than returning a value.
    std::optional<bool> is_error;

    // ---- Usage --------------------------------------------------------------

    /// Token usage and cost delta for this turn (set on assistant messages
    /// after a streaming turn completes; absent on user/tool messages).
    std::optional<UsageDelta> usage;

    // ---- Timestamp ----------------------------------------------------------

    std::chrono::system_clock::time_point ts;

    // ---- Constructor --------------------------------------------------------

    /// Default-construct: assigns a fresh UUID v4 and sets ts to now.
    Message();

    // ---- Convenience helpers ------------------------------------------------

    /// Returns a string_view over the content field.
    [[nodiscard]] std::string_view text() const noexcept { return content; }

    /// True when role == Assistant and at least one tool call is present.
    [[nodiscard]] bool is_tool_call() const noexcept;

    /// True when role == Tool (i.e. this message carries a tool result).
    [[nodiscard]] bool is_tool_result() const noexcept { return role == Role::Tool; }
};

// =============================================================================
// JSON serialisation helpers
// =============================================================================

/// Deserialise a Message from a stored JSON object.
/// Throws std::invalid_argument if required fields are absent or malformed.
[[nodiscard]] Message from_json(const Json& j);

/// Serialise a Message to a JSON object suitable for storage or debug output.
/// The wire-level conversion (to WireMessage) lives in C4's ChatRequest.hpp.
[[nodiscard]] Json to_json(const Message& m);

} // namespace batbox::conversation
