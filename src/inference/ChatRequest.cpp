// src/inference/ChatRequest.cpp
// ---------------------------------------------------------------------------
// nlohmann::json serialisation / deserialisation for the C4 wire models:
//   WireToolCall, WireMessage, ToolDef, ChatRequest   (ChatRequest.hpp)
//   UsageDelta, ToolCallDelta, StreamDelta, ChatResponse (ChatResponse.hpp)
//
// All to_json / from_json functions are free functions in namespace
// batbox::inference so that nlohmann's ADL picks them up automatically when
// code calls:
//   batbox::Json j = my_chat_request;
//   ChatRequest req = j.get<ChatRequest>();
// ---------------------------------------------------------------------------

#include <batbox/inference/ChatRequest.hpp>
#include <batbox/inference/ChatResponse.hpp>

#include <stdexcept>
#include <string>

namespace batbox::inference {

// ============================================================================
// WireToolCall
// ============================================================================

void to_json(Json& j, const WireToolCall& tc) {
    j = Json::object();
    j["id"] = tc.id;
    j["type"] = "function";
    j["function"] = {
        {"name",      tc.function.name},
        {"arguments", tc.function.arguments}
    };
}

void from_json(const Json& j, WireToolCall& tc) {
    j.at("id").get_to(tc.id);
    const auto& fn = j.at("function");
    fn.at("name").get_to(tc.function.name);
    fn.at("arguments").get_to(tc.function.arguments);
}

// ============================================================================
// WireMessage
// ============================================================================

void to_json(Json& j, const WireMessage& msg) {
    j = Json::object();
    j["role"] = msg.role;

    if (msg.content.has_value()) {
        j["content"] = *msg.content;
    } else {
        // Assistant messages that emit tool_calls may have null content.
        // Explicitly emit null so the wire representation is unambiguous.
        j["content"] = nullptr;
    }

    if (msg.tool_calls.has_value()) {
        j["tool_calls"] = *msg.tool_calls;
    }
    if (msg.tool_call_id.has_value()) {
        j["tool_call_id"] = *msg.tool_call_id;
    }
    if (msg.name.has_value()) {
        j["name"] = *msg.name;
    }
}

void from_json(const Json& j, WireMessage& msg) {
    j.at("role").get_to(msg.role);

    if (j.contains("content") && !j["content"].is_null()) {
        msg.content = j["content"].get<std::string>();
    } else {
        msg.content = std::nullopt;
    }

    if (j.contains("tool_calls") && !j["tool_calls"].is_null()) {
        msg.tool_calls = j["tool_calls"].get<std::vector<WireToolCall>>();
    } else {
        msg.tool_calls = std::nullopt;
    }

    if (j.contains("tool_call_id") && !j["tool_call_id"].is_null()) {
        msg.tool_call_id = j["tool_call_id"].get<std::string>();
    } else {
        msg.tool_call_id = std::nullopt;
    }

    if (j.contains("name") && !j["name"].is_null()) {
        msg.name = j["name"].get<std::string>();
    } else {
        msg.name = std::nullopt;
    }
}

// ============================================================================
// ToolDef
// ============================================================================

void to_json(Json& j, const ToolDef& td) {
    j = {
        {"type", td.type},
        {"function", {
            {"name",        td.name},
            {"description", td.description},
            {"parameters",  td.schema}
        }}
    };
}

void from_json(const Json& j, ToolDef& td) {
    j.at("type").get_to(td.type);
    const auto& fn = j.at("function");
    fn.at("name").get_to(td.name);
    fn.at("description").get_to(td.description);
    td.schema = fn.value("parameters", Json::object());
}

// ============================================================================
// ChatRequest — tool_choice wire encoding
//
// std::nullopt          → field omitted entirely
// "none" / "auto"       → JSON string
// "function:<name>"     → {"type":"function","function":{"name":"<name>"}}
// ============================================================================

void to_json(Json& j, const ChatRequest& req) {
    j = Json::object();
    j["model"]    = req.model;
    j["messages"] = req.messages;
    j["stream"]   = req.stream;

    if (!req.tools.empty()) {
        j["tools"] = req.tools;
    }

    // tool_choice encoding
    if (req.tool_choice.has_value()) {
        const std::string& tc = *req.tool_choice;
        if (tc == "none" || tc == "auto") {
            j["tool_choice"] = tc;
        } else if (tc.size() > 9 && tc.substr(0, 9) == "function:") {
            std::string fname = tc.substr(9);
            j["tool_choice"] = {
                {"type",     "function"},
                {"function", {{"name", fname}}}
            };
        } else {
            // Treat as a bare string (e.g. "required" for newer providers).
            j["tool_choice"] = tc;
        }
    }
    // nullopt → field omitted

    if (req.max_tokens.has_value()) {
        j["max_tokens"] = *req.max_tokens;
    }
    if (req.temperature.has_value()) {
        j["temperature"] = *req.temperature;
    }
    if (req.top_p.has_value()) {
        j["top_p"] = *req.top_p;
    }

    // stream_options: only emit when streaming and include_usage requested
    if (req.stream && req.stream_options_include_usage.has_value()
                   && *req.stream_options_include_usage) {
        j["stream_options"] = {{"include_usage", true}};
    }
}

void from_json(const Json& j, ChatRequest& req) {
    j.at("model").get_to(req.model);
    j.at("messages").get_to(req.messages);

    req.stream = j.value("stream", true);

    if (j.contains("tools") && j["tools"].is_array()) {
        req.tools = j["tools"].get<std::vector<ToolDef>>();
    } else {
        req.tools.clear();
    }

    // tool_choice decoding
    if (j.contains("tool_choice") && !j["tool_choice"].is_null()) {
        const auto& tc = j["tool_choice"];
        if (tc.is_string()) {
            req.tool_choice = tc.get<std::string>();
        } else if (tc.is_object()) {
            // {"type":"function","function":{"name":"<n>"}}
            std::string fname = tc.value("function", Json::object())
                                  .value("name", std::string{});
            req.tool_choice = "function:" + fname;
        }
    } else {
        req.tool_choice = std::nullopt;
    }

    if (j.contains("max_tokens") && !j["max_tokens"].is_null()) {
        req.max_tokens = j["max_tokens"].get<int>();
    } else {
        req.max_tokens = std::nullopt;
    }

    if (j.contains("temperature") && !j["temperature"].is_null()) {
        req.temperature = j["temperature"].get<double>();
    } else {
        req.temperature = std::nullopt;
    }

    if (j.contains("top_p") && !j["top_p"].is_null()) {
        req.top_p = j["top_p"].get<double>();
    } else {
        req.top_p = std::nullopt;
    }

    // stream_options
    if (j.contains("stream_options") && j["stream_options"].is_object()) {
        const auto& so = j["stream_options"];
        if (so.contains("include_usage")) {
            req.stream_options_include_usage = so["include_usage"].get<bool>();
        }
    }
}

// ============================================================================
// UsageDelta
// ============================================================================

void to_json(Json& j, const UsageDelta& u) {
    j = {
        {"prompt_tokens",     u.prompt_tokens},
        {"completion_tokens", u.completion_tokens},
        {"total_tokens",      u.total_tokens}
    };
    // cost_usd is locally computed; never emitted on the wire.
}

void from_json(const Json& j, UsageDelta& u) {
    u.prompt_tokens     = j.value("prompt_tokens",     0);
    u.completion_tokens = j.value("completion_tokens", 0);
    u.total_tokens      = j.value("total_tokens",      0);
    u.cost_usd          = 0.0;  // computed locally by UsageTracker
}

// ============================================================================
// ToolCallDelta
// ============================================================================

void to_json(Json& j, const ToolCallDelta& tcd) {
    j = Json::object();
    j["index"] = tcd.index;

    Json fn = Json::object();
    if (tcd.id.has_value())   { j["id"] = *tcd.id; }
    if (tcd.name.has_value()) { fn["name"] = *tcd.name; }
    if (tcd.arguments_fragment.has_value()) {
        fn["arguments"] = *tcd.arguments_fragment;
    }
    if (!fn.empty()) {
        j["function"] = fn;
    }
    // type is always "function" for tool_call deltas
    j["type"] = "function";
}

void from_json(const Json& j, ToolCallDelta& tcd) {
    tcd.index = j.value("index", 0);

    if (j.contains("id") && !j["id"].is_null()) {
        tcd.id = j["id"].get<std::string>();
    } else {
        tcd.id = std::nullopt;
    }

    if (j.contains("function") && j["function"].is_object()) {
        const auto& fn = j["function"];

        if (fn.contains("name") && !fn["name"].is_null()) {
            tcd.name = fn["name"].get<std::string>();
        } else {
            tcd.name = std::nullopt;
        }

        if (fn.contains("arguments") && !fn["arguments"].is_null()) {
            tcd.arguments_fragment = fn["arguments"].get<std::string>();
        } else {
            tcd.arguments_fragment = std::nullopt;
        }
    } else {
        tcd.name               = std::nullopt;
        tcd.arguments_fragment = std::nullopt;
    }
}

// ============================================================================
// StreamDelta
// ============================================================================

void to_json(Json& j, const StreamDelta& sd) {
    j = Json::object();

    if (sd.content.has_value()) {
        j["content"] = *sd.content;
    }
    if (sd.reasoning_content.has_value()) {
        j["reasoning_content"] = *sd.reasoning_content;
    }
    if (sd.tool_calls.has_value()) {
        j["tool_calls"] = *sd.tool_calls;
    }
    if (sd.finish_reason.has_value()) {
        j["finish_reason"] = *sd.finish_reason;
    }
    if (sd.usage.has_value()) {
        j["usage"] = *sd.usage;
    }
}

void from_json(const Json& j, StreamDelta& sd) {
    // content: may be absent, null, or a string
    if (j.contains("content") && j["content"].is_string()) {
        sd.content = j["content"].get<std::string>();
    } else {
        sd.content = std::nullopt;
    }

    // reasoning_content: chain-of-thought from reasoning models (Magistral,
    // Qwen3, DeepSeek).  Emitted before any content delta.  Not user-visible.
    if (j.contains("reasoning_content") && j["reasoning_content"].is_string()) {
        sd.reasoning_content = j["reasoning_content"].get<std::string>();
    } else {
        sd.reasoning_content = std::nullopt;
    }

    // tool_calls: array of ToolCallDelta fragments
    if (j.contains("tool_calls") && j["tool_calls"].is_array()
                                  && !j["tool_calls"].empty()) {
        sd.tool_calls = j["tool_calls"].get<std::vector<ToolCallDelta>>();
    } else {
        sd.tool_calls = std::nullopt;
    }

    // finish_reason: string or null
    if (j.contains("finish_reason") && j["finish_reason"].is_string()) {
        sd.finish_reason = j["finish_reason"].get<std::string>();
    } else {
        sd.finish_reason = std::nullopt;
    }

    // usage: present only in the final chunk when stream_options.include_usage
    if (j.contains("usage") && j["usage"].is_object()) {
        sd.usage = j["usage"].get<UsageDelta>();
    } else {
        sd.usage = std::nullopt;
    }
}

// ============================================================================
// ChatResponse
// ============================================================================

void to_json(Json& j, const ChatResponse& cr) {
    j = Json::object();
    j["id"]            = cr.id;
    j["model"]         = cr.model;
    j["finish_reason"] = cr.finish_reason;
    j["usage"]         = cr.usage;

    if (cr.content.has_value()) {
        j["content"] = *cr.content;
    } else {
        j["content"] = nullptr;
    }

    if (cr.tool_calls.has_value()) {
        j["tool_calls"] = *cr.tool_calls;
    }
}

void from_json(const Json& j, ChatResponse& cr) {
    j.at("id").get_to(cr.id);
    j.at("model").get_to(cr.model);

    // finish_reason is typically at choices[0].finish_reason in the OpenAI
    // response shape, but Client::chat() will already have extracted and
    // normalised it into the top-level field before deserialising into
    // ChatResponse.  If a caller passes the raw API body the field may be
    // absent — default to "stop".
    cr.finish_reason = j.value("finish_reason", std::string{"stop"});

    if (j.contains("usage") && j["usage"].is_object()) {
        cr.usage = j["usage"].get<UsageDelta>();
    }

    if (j.contains("content") && j["content"].is_string()) {
        cr.content = j["content"].get<std::string>();
    } else {
        cr.content = std::nullopt;
    }

    if (j.contains("tool_calls") && j["tool_calls"].is_array()
                                  && !j["tool_calls"].empty()) {
        cr.tool_calls = j["tool_calls"].get<std::vector<WireToolCall>>();
    } else {
        cr.tool_calls = std::nullopt;
    }
}

} // namespace batbox::inference
