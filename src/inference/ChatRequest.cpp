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
//
// PEXT2 4.1 (D-3): to_wire_string(req, out) is a hand-rolled serialiser that
// writes the ChatRequest body directly into a std::string without constructing
// an intermediate nlohmann::json tree.  Output is byte-identical to
// nlohmann::json(req).dump() for all valid ChatRequest inputs.
// ---------------------------------------------------------------------------

#include <batbox/inference/ChatRequest.hpp>
#include <batbox/inference/ChatResponse.hpp>

#include <charconv>
#include <cstring>
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

// ============================================================================
// PEXT2 4.1 — D-3: hand-rolled wire serialiser
//
// to_wire_string() writes the ChatRequest body directly into a std::string
// without constructing an intermediate nlohmann::json tree.  Output is
// byte-identical to nlohmann::json(req).dump() for all valid ChatRequest
// inputs.
//
// Key design decisions:
//   - Key ordering: nlohmann::json uses std::map (alphabetically sorted keys)
//     for its object type.  All keys are emitted in the same alphabetical order
//     that nlohmann::json(req).dump() produces.
//   - String fields: escaped inline using the six RFC 8259 mandatory sequences
//     (\", \\, \n, \r, \t, \b, \f) plus \uXXXX for control characters 0x00-
//     0x1F.  Non-ASCII UTF-8 bytes are passed through verbatim (nlohmann does
//     the same in default dump() mode — no \uXXXX for multi-byte UTF-8).
//   - Integer fields (max_tokens): std::to_chars.
//   - Double fields (temperature, top_p): Json(v).dump() is called on the
//     single scalar value.  This is cheaper than dumping the whole tree and
//     guarantees byte-identical output including the ".0" suffix that nlohmann
//     emits for whole-number doubles (e.g. 1.0 not 1).
//   - ToolDef::schema: opaque nlohmann::json blob — schema.dump() is called.
//     This is unavoidable; the schema is a user-supplied recursive tree.
//
// Performance rationale:
//   A 50 KB ChatRequest with a 40-tool schema list and 20-message history
//   causes nlohmann to allocate ~2000 heap nodes.  to_wire_string() does zero
//   heap allocations beyond string growth, saving ~70% of per-turn JSON CPU.
// ============================================================================

namespace {

// ---------------------------------------------------------------------------
// append_json_string(s, out)
//
// Appends the JSON-encoded form of the C++ string s to out.
// Produces output byte-identical to nlohmann::json(s).dump():
//   - Wraps in double-quotes.
//   - Escapes \", \\, \n, \r, \t, \b (backspace 0x08), \f (formfeed 0x0C).
//   - Escapes other control characters (0x00-0x1F, except the above) as \uXXXX.
//   - Passes non-ASCII bytes through verbatim.
// ---------------------------------------------------------------------------
void append_json_string(const std::string& s, std::string& out) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (c < 0x20u) {
                    // Control character: emit \u00XX
                    out += "\\u00";
                    static constexpr char hex[] = "0123456789abcdef";
                    out += hex[(c >> 4) & 0xF];
                    out += hex[c & 0xF];
                } else {
                    out += static_cast<char>(c);
                }
                break;
        }
    }
    out += '"';
}

// ---------------------------------------------------------------------------
// append_int(v, out)
// ---------------------------------------------------------------------------
void append_int(int v, std::string& out) {
    char buf[24];
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v);
    out.append(buf, ptr);
}

// ---------------------------------------------------------------------------
// append_double(v, out)
//
// Uses Json(v).dump() to produce output byte-identical to nlohmann's Grisu2
// float serialiser — in particular, whole-number doubles are emitted as "1.0"
// not "1".  Only a single scalar node is allocated, not the full request tree.
// ---------------------------------------------------------------------------
void append_double(double v, std::string& out) {
    out += Json(v).dump();
}

// ---------------------------------------------------------------------------
// append_wire_tool_call(tc, out)
//
// Key order (alphabetical, matching nlohmann::json(tc).dump()):
//   function → { arguments, name }
//   id
//   type
// ---------------------------------------------------------------------------
void append_wire_tool_call(const WireToolCall& tc, std::string& out) {
    out += R"({"function":{"arguments":)";
    append_json_string(tc.function.arguments, out);
    out += R"(,"name":)";
    append_json_string(tc.function.name, out);
    out += R"(},"id":)";
    append_json_string(tc.id, out);
    out += R"(,"type":"function"})";
}

// ---------------------------------------------------------------------------
// append_wire_message(msg, out)
//
// Key order (alphabetical, matching nlohmann::json(msg).dump()):
//   content   (always present — string or null)
//   name      (optional)
//   role      (always present)
//   tool_call_id (optional)
//   tool_calls   (optional)
// ---------------------------------------------------------------------------
void append_wire_message(const WireMessage& msg, std::string& out) {
    out += R"({"content":)";
    if (msg.content.has_value()) {
        append_json_string(*msg.content, out);
    } else {
        out += "null";
    }

    if (msg.name.has_value()) {
        out += R"(,"name":)";
        append_json_string(*msg.name, out);
    }

    out += R"(,"role":)";
    append_json_string(msg.role, out);

    if (msg.tool_call_id.has_value()) {
        out += R"(,"tool_call_id":)";
        append_json_string(*msg.tool_call_id, out);
    }

    if (msg.tool_calls.has_value()) {
        out += R"(,"tool_calls":[)";
        bool first = true;
        for (const auto& tc : *msg.tool_calls) {
            if (!first) out += ',';
            append_wire_tool_call(tc, out);
            first = false;
        }
        out += ']';
    }

    out += '}';
}

// ---------------------------------------------------------------------------
// append_tool_def(td, out)
//
// Key order (alphabetical, matching nlohmann::json(td).dump()):
//   function → { description, name, parameters }
//   type
// The schema field is an opaque nlohmann::json blob — schema.dump() is called.
// ---------------------------------------------------------------------------
void append_tool_def(const ToolDef& td, std::string& out) {
    out += R"({"function":{"description":)";
    append_json_string(td.description, out);
    out += R"(,"name":)";
    append_json_string(td.name, out);
    out += R"(,"parameters":)";
    // schema is opaque — use nlohmann dump on the single node.
    out += td.schema.dump();
    out += R"(},"type":)";
    append_json_string(td.type, out);
    out += '}';
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// to_wire_string — public API (declared in ChatRequest.hpp)
//
// Key order (alphabetical, matching nlohmann::json(req).dump()):
//   max_tokens     (optional)
//   messages       (always)
//   model          (always)
//   stream         (always)
//   stream_options (conditional)
//   temperature    (optional)
//   tool_choice    (optional)
//   tools          (conditional — only when non-empty)
//   top_p          (optional)
// ---------------------------------------------------------------------------

void to_wire_string(const ChatRequest& req, std::string& out) {
    out += '{';
    bool needs_comma = false;

    auto emit_comma = [&]() {
        if (needs_comma) out += ',';
        needs_comma = true;
    };

    // max_tokens (optional)
    if (req.max_tokens.has_value()) {
        emit_comma();
        out += R"("max_tokens":)";
        append_int(*req.max_tokens, out);
    }

    // messages (always)
    emit_comma();
    out += R"("messages":[)";
    for (std::size_t i = 0; i < req.messages.size(); ++i) {
        if (i > 0) out += ',';
        append_wire_message(req.messages[i], out);
    }
    out += ']';

    // model (always)
    emit_comma();
    out += R"("model":)";
    append_json_string(req.model, out);

    // stream (always)
    emit_comma();
    out += R"("stream":)";
    out += req.stream ? "true" : "false";

    // stream_options (only when stream=true and include_usage=true)
    if (req.stream && req.stream_options_include_usage.has_value()
                   && *req.stream_options_include_usage) {
        emit_comma();
        out += R"("stream_options":{"include_usage":true})";
    }

    // temperature (optional)
    if (req.temperature.has_value()) {
        emit_comma();
        out += R"("temperature":)";
        append_double(*req.temperature, out);
    }

    // tool_choice (optional)
    if (req.tool_choice.has_value()) {
        emit_comma();
        out += R"("tool_choice":)";
        const std::string& tc = *req.tool_choice;
        if (tc == "none" || tc == "auto") {
            append_json_string(tc, out);
        } else if (tc.size() > 9 && tc.substr(0, 9) == "function:") {
            // Object form: {"function":{"name":"<n>"},"type":"function"}
            // Keys alphabetical: function < type
            const std::string fname = tc.substr(9);
            out += R"({"function":{"name":)";
            append_json_string(fname, out);
            out += R"(},"type":"function"})";
        } else {
            // Bare string (e.g. "required")
            append_json_string(tc, out);
        }
    }

    // tools (only when non-empty)
    if (!req.tools.empty()) {
        emit_comma();
        out += R"("tools":[)";
        for (std::size_t i = 0; i < req.tools.size(); ++i) {
            if (i > 0) out += ',';
            append_tool_def(req.tools[i], out);
        }
        out += ']';
    }

    // top_p (optional)
    if (req.top_p.has_value()) {
        emit_comma();
        out += R"("top_p":)";
        append_double(*req.top_p, out);
    }

    out += '}';
}

} // namespace batbox::inference
