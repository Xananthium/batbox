// src/conversation/Message.cpp
// =============================================================================
// Implementation of batbox::conversation message model helpers (CPP 3.1).
//
// Responsibilities:
//   - Message default constructor (UUID + timestamp assignment)
//   - Role ↔ wire string conversion
//   - to_json / from_json (internal storage format, NOT the wire API format)
//
// The wire-level ChatRequest/WireMessage conversion lives in C4
// (include/batbox/inference/ChatRequest.hpp).
// =============================================================================

#include <batbox/conversation/Message.hpp>
#include <batbox/core/Uuid.hpp>

#include <chrono>
#include <stdexcept>
#include <string>

namespace batbox::conversation {

// =============================================================================
// Message default constructor
// =============================================================================

Message::Message()
    : id(Uuid::v4().to_string())
    , ts(std::chrono::system_clock::now())
{}

// =============================================================================
// bool helpers
// =============================================================================

bool Message::is_tool_call() const noexcept {
    return role == Role::Assistant
        && tool_calls.has_value()
        && !tool_calls->empty();
}

// =============================================================================
// Role ↔ wire string
// =============================================================================

std::string_view to_wire_role(Role r) noexcept {
    switch (r) {
        case Role::System:    return "system";
        case Role::User:      return "user";
        case Role::Assistant: return "assistant";
        case Role::Tool:      return "tool";
    }
    return "user"; // unreachable; silences -Wreturn-type on older compilers
}

Role role_from_string(std::string_view s) {
    if (s == "system")    return Role::System;
    if (s == "user")      return Role::User;
    if (s == "assistant") return Role::Assistant;
    if (s == "tool")      return Role::Tool;
    throw std::invalid_argument(
        std::string("batbox::conversation::role_from_string: unknown role '")
        + std::string(s) + "'");
}

// =============================================================================
// JSON helpers — internal storage format
//
// Schema:
// {
//   "id":           "<uuid>",
//   "role":         "user" | "assistant" | "system" | "tool",
//   "content":      "<string>",
//   "ts":           <int64 unix ms>,
//   "tool_calls":   [{"id":"<str>","name":"<str>","arguments":{...}}, ...],  // optional
//   "tool_call_id": "<str>",   // optional
//   "tool_name":    "<str>",   // optional
//   "is_error":     true|false, // optional
//   "usage": {                 // optional
//     "prompt_tokens":     <int>,
//     "completion_tokens": <int>,
//     "total_tokens":      <int>,
//     "cost_usd":          <double>
//   }
// }
// =============================================================================

Json to_json(const Message& m) {
    Json j = Json::object();

    j["id"]      = m.id;
    j["role"]    = std::string(to_wire_role(m.role));
    j["content"] = m.content;

    // Timestamp as milliseconds since epoch (portable, avoids locale/TZ issues)
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        m.ts.time_since_epoch()).count();
    j["ts"] = ms;

    if (m.tool_calls.has_value()) {
        Json arr = Json::array();
        for (const auto& tc : *m.tool_calls) {
            Json item = Json::object();
            item["id"]        = tc.id;
            item["name"]      = tc.name;
            item["arguments"] = tc.arguments;
            arr.push_back(std::move(item));
        }
        j["tool_calls"] = std::move(arr);
    }

    if (m.tool_call_id.has_value()) {
        j["tool_call_id"] = *m.tool_call_id;
    }

    if (m.tool_name.has_value()) {
        j["tool_name"] = *m.tool_name;
    }

    if (m.is_error.has_value()) {
        j["is_error"] = *m.is_error;
    }

    if (m.usage.has_value()) {
        Json u = Json::object();
        u["prompt_tokens"]     = m.usage->prompt_tokens;
        u["completion_tokens"] = m.usage->completion_tokens;
        u["total_tokens"]      = m.usage->total_tokens;
        u["cost_usd"]          = m.usage->cost_usd;
        j["usage"] = std::move(u);
    }

    return j;
}

Message from_json(const Json& j) {
    // Validate required fields
    if (!j.contains("id") || !j["id"].is_string()) {
        throw std::invalid_argument("Message JSON missing or invalid 'id' field");
    }
    if (!j.contains("role") || !j["role"].is_string()) {
        throw std::invalid_argument("Message JSON missing or invalid 'role' field");
    }
    if (!j.contains("content") || !j["content"].is_string()) {
        throw std::invalid_argument("Message JSON missing or invalid 'content' field");
    }
    if (!j.contains("ts") || !j["ts"].is_number()) {
        throw std::invalid_argument("Message JSON missing or invalid 'ts' field");
    }

    Message m;

    m.id      = j["id"].get<std::string>();
    m.role    = role_from_string(j["role"].get<std::string>());
    m.content = j["content"].get<std::string>();

    // Restore timestamp
    const std::int64_t ms = j["ts"].get<std::int64_t>();
    m.ts = std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));

    // Optional: tool_calls
    if (j.contains("tool_calls") && j["tool_calls"].is_array()) {
        std::vector<ToolCall> calls;
        for (const auto& item : j["tool_calls"]) {
            ToolCall tc;
            tc.id        = item.at("id").get<std::string>();
            tc.name      = item.at("name").get<std::string>();
            tc.arguments = item.contains("arguments") ? item["arguments"] : Json::object();
            calls.push_back(std::move(tc));
        }
        m.tool_calls = std::move(calls);
    }

    // Optional: tool_call_id
    if (j.contains("tool_call_id") && j["tool_call_id"].is_string()) {
        m.tool_call_id = j["tool_call_id"].get<std::string>();
    }

    // Optional: tool_name
    if (j.contains("tool_name") && j["tool_name"].is_string()) {
        m.tool_name = j["tool_name"].get<std::string>();
    }

    // Optional: is_error
    if (j.contains("is_error") && j["is_error"].is_boolean()) {
        m.is_error = j["is_error"].get<bool>();
    }

    // Optional: usage
    if (j.contains("usage") && j["usage"].is_object()) {
        const auto& u = j["usage"];
        UsageDelta ud;
        ud.prompt_tokens     = u.value("prompt_tokens",     0);
        ud.completion_tokens = u.value("completion_tokens", 0);
        ud.total_tokens      = u.value("total_tokens",      0);
        ud.cost_usd          = u.value("cost_usd",          0.0);
        m.usage = ud;
    }

    return m;
}

} // namespace batbox::conversation
