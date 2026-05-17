// tests/unit/test_chat_wire_model.cpp
// ---------------------------------------------------------------------------
// doctest suite for ChatRequest.hpp + ChatResponse.hpp wire models.
//
// Coverage:
//   1. WireToolCall  — to_json / from_json round-trip
//   2. WireMessage   — all roles; optional fields; tool_calls in assistant msg
//   3. ToolDef       — serialise / deserialise including schema passthrough
//   4. ChatRequest   — full round-trip; optional fields omitted when absent;
//                      tool_choice variants (nullopt/"auto"/"none"/"function:<n>")
//                      stream_options emission logic
//   5. UsageDelta    — round-trip; cost_usd is never deserialised
//   6. ToolCallDelta — first-fragment (id+name present) and continuation fragment
//   7. StreamDelta   — content-only chunk; tool-call chunk; finish-reason chunk;
//                      final usage chunk
//   8. ChatResponse  — complete non-streaming response round-trip
//   9. Real SSE fixture — deserialise a real OpenAI streaming delta JSON
//
// Build + run (standalone, no CMake — from repo root):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_chat_wire_model.cpp src/inference/ChatRequest.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       -o /tmp/test_chat_wire && /tmp/test_chat_wire
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/inference/ChatRequest.hpp>
#include <batbox/inference/ChatResponse.hpp>

#include <cmath>
#include <string>
#include <vector>

using namespace batbox;
using namespace batbox::inference;

// ============================================================================
// TEST SUITE 1: WireToolCall
// ============================================================================
TEST_SUITE("WireToolCall") {

    TEST_CASE("round-trip basic tool call") {
        WireToolCall tc;
        tc.id = "call_abc123";
        tc.function.name = "read_file";
        tc.function.arguments = R"({"path":"/tmp/foo.txt"})";

        Json j = tc;
        WireToolCall tc2 = j.get<WireToolCall>();

        CHECK(tc2.id == "call_abc123");
        CHECK(tc2.function.name == "read_file");
        CHECK(tc2.function.arguments == R"({"path":"/tmp/foo.txt"})");
    }

    TEST_CASE("serialised shape matches OpenAI spec") {
        WireToolCall tc;
        tc.id = "call_xyz";
        tc.function.name = "bash";
        tc.function.arguments = R"({"command":"ls"})";

        Json j = tc;

        CHECK(j["id"] == "call_xyz");
        CHECK(j["type"] == "function");
        CHECK(j["function"]["name"] == "bash");
        CHECK(j["function"]["arguments"] == R"({"command":"ls"})");
    }

    TEST_CASE("empty arguments string survives round-trip") {
        WireToolCall tc;
        tc.id = "call_empty";
        tc.function.name = "no_args_tool";
        tc.function.arguments = "{}";

        Json j = tc;
        WireToolCall tc2 = j.get<WireToolCall>();

        CHECK(tc2.function.arguments == "{}");
    }
}

// ============================================================================
// TEST SUITE 2: WireMessage
// ============================================================================
TEST_SUITE("WireMessage") {

    TEST_CASE("system message round-trip") {
        WireMessage msg;
        msg.role = "system";
        msg.content = "You are a helpful assistant.";

        Json j = msg;
        WireMessage msg2 = j.get<WireMessage>();

        CHECK(msg2.role == "system");
        REQUIRE(msg2.content.has_value());
        CHECK(*msg2.content == "You are a helpful assistant.");
        CHECK_FALSE(msg2.tool_calls.has_value());
        CHECK_FALSE(msg2.tool_call_id.has_value());
        CHECK_FALSE(msg2.name.has_value());
    }

    TEST_CASE("user message round-trip") {
        WireMessage msg;
        msg.role = "user";
        msg.content = "What files are in /tmp?";

        Json j = msg;
        WireMessage msg2 = j.get<WireMessage>();

        CHECK(msg2.role == "user");
        REQUIRE(msg2.content.has_value());
        CHECK(*msg2.content == "What files are in /tmp?");
    }

    TEST_CASE("assistant message with content only") {
        WireMessage msg;
        msg.role = "assistant";
        msg.content = "I can help with that.";

        Json j = msg;
        WireMessage msg2 = j.get<WireMessage>();

        CHECK(msg2.role == "assistant");
        REQUIRE(msg2.content.has_value());
        CHECK(*msg2.content == "I can help with that.");
        CHECK_FALSE(msg2.tool_calls.has_value());
    }

    TEST_CASE("assistant message with tool_calls and null content") {
        WireMessage msg;
        msg.role = "assistant";
        // content is nullopt — model is doing tool use
        WireToolCall tc;
        tc.id = "call_001";
        tc.function.name = "glob";
        tc.function.arguments = R"({"pattern":"*.cpp"})";
        msg.tool_calls = std::vector<WireToolCall>{tc};

        Json j = msg;

        // null content must appear in the JSON
        CHECK(j["content"].is_null());
        REQUIRE(j["tool_calls"].is_array());
        CHECK(j["tool_calls"].size() == 1u);
        CHECK(j["tool_calls"][0]["id"] == "call_001");

        WireMessage msg2 = j.get<WireMessage>();
        CHECK_FALSE(msg2.content.has_value());
        REQUIRE(msg2.tool_calls.has_value());
        CHECK(msg2.tool_calls->size() == 1u);
        CHECK((*msg2.tool_calls)[0].id == "call_001");
    }

    TEST_CASE("tool result message round-trip") {
        WireMessage msg;
        msg.role = "tool";
        msg.content = "file1.cpp\nfile2.cpp";
        msg.tool_call_id = "call_001";
        msg.name = "glob";

        Json j = msg;
        WireMessage msg2 = j.get<WireMessage>();

        CHECK(msg2.role == "tool");
        REQUIRE(msg2.content.has_value());
        CHECK(*msg2.content == "file1.cpp\nfile2.cpp");
        REQUIRE(msg2.tool_call_id.has_value());
        CHECK(*msg2.tool_call_id == "call_001");
        REQUIRE(msg2.name.has_value());
        CHECK(*msg2.name == "glob");
    }

    TEST_CASE("optional fields absent when not set") {
        WireMessage msg;
        msg.role = "user";
        msg.content = "hello";

        Json j = msg;

        CHECK_FALSE(j.contains("tool_calls"));
        CHECK_FALSE(j.contains("tool_call_id"));
        CHECK_FALSE(j.contains("name"));
    }

    TEST_CASE("multiple tool_calls in assistant message") {
        WireMessage msg;
        msg.role = "assistant";

        WireToolCall tc1, tc2;
        tc1.id = "call_A"; tc1.function.name = "read"; tc1.function.arguments = R"({"path":"a"})";
        tc2.id = "call_B"; tc2.function.name = "read"; tc2.function.arguments = R"({"path":"b"})";
        msg.tool_calls = {tc1, tc2};

        Json j = msg;
        WireMessage msg2 = j.get<WireMessage>();

        REQUIRE(msg2.tool_calls.has_value());
        CHECK(msg2.tool_calls->size() == 2u);
        CHECK((*msg2.tool_calls)[0].id == "call_A");
        CHECK((*msg2.tool_calls)[1].id == "call_B");
    }
}

// ============================================================================
// TEST SUITE 3: ToolDef
// ============================================================================
TEST_SUITE("ToolDef") {

    TEST_CASE("round-trip basic tool definition") {
        ToolDef td;
        td.type = "function";
        td.name = "read_file";
        td.description = "Read the contents of a file.";
        td.schema = {
            {"type", "object"},
            {"properties", {
                {"path", {{"type", "string"}, {"description", "file path"}}}
            }},
            {"required", Json::array({"path"})}
        };

        Json j = td;
        ToolDef td2 = j.get<ToolDef>();

        CHECK(td2.type == "function");
        CHECK(td2.name == "read_file");
        CHECK(td2.description == "Read the contents of a file.");
        CHECK(td2.schema["type"] == "object");
        CHECK(td2.schema["required"][0] == "path");
    }

    TEST_CASE("serialised shape nests under function key") {
        ToolDef td;
        td.type = "function";
        td.name = "bash";
        td.description = "Run a shell command.";
        td.schema = Json::object();

        Json j = td;

        CHECK(j["type"] == "function");
        REQUIRE(j.contains("function"));
        CHECK(j["function"]["name"] == "bash");
        CHECK(j["function"]["description"] == "Run a shell command.");
        CHECK(j["function"].contains("parameters"));
    }

    TEST_CASE("schema passthrough — complex schema survives round-trip") {
        ToolDef td;
        td.type = "function";
        td.name = "complex";
        td.description = "Complex tool.";
        td.schema = {
            {"type", "object"},
            {"properties", {
                {"mode",   {{"type","string"},{"enum", Json::array({"a","b","c"})}}},
                {"count",  {{"type","integer"},{"minimum", 1}}},
                {"tags",   {{"type","array"},{"items",{{"type","string"}}}}}
            }},
            {"required", Json::array({"mode"})}
        };

        Json j = td;
        ToolDef td2 = j.get<ToolDef>();

        CHECK(td2.schema["properties"]["mode"]["enum"][1] == "b");
        CHECK(td2.schema["properties"]["count"]["minimum"] == 1);
    }
}

// ============================================================================
// TEST SUITE 4: ChatRequest
// ============================================================================
TEST_SUITE("ChatRequest") {

    TEST_CASE("minimal request round-trip") {
        ChatRequest req;
        req.model = "gpt-4o";
        req.messages.push_back({"user", "Hello!"});

        Json j = req;
        ChatRequest req2 = j.get<ChatRequest>();

        CHECK(req2.model == "gpt-4o");
        REQUIRE(req2.messages.size() == 1u);
        CHECK(req2.messages[0].role == "user");
        REQUIRE(req2.messages[0].content.has_value());
        CHECK(*req2.messages[0].content == "Hello!");
        CHECK(req2.stream == true);
    }

    TEST_CASE("optional fields absent from JSON when not set") {
        ChatRequest req;
        req.model = "gpt-4o-mini";
        req.messages.push_back({"user", "ping"});
        // Leave max_tokens, temperature, top_p, tool_choice unset

        Json j = req;

        CHECK_FALSE(j.contains("max_tokens"));
        CHECK_FALSE(j.contains("temperature"));
        CHECK_FALSE(j.contains("top_p"));
        CHECK_FALSE(j.contains("tool_choice"));
        CHECK_FALSE(j.contains("tools"));
    }

    TEST_CASE("optional fields present when set") {
        ChatRequest req;
        req.model = "gpt-4o";
        req.messages.push_back({"user", "hi"});
        req.max_tokens   = 512;
        req.temperature  = 0.7;
        req.top_p        = 0.9;

        Json j = req;

        CHECK(j["max_tokens"]  == 512);
        CHECK(std::abs(j["temperature"].get<double>() - 0.7) < 1e-9);
        CHECK(std::abs(j["top_p"].get<double>()        - 0.9) < 1e-9);
    }

    TEST_CASE("tool_choice nullopt omits the field") {
        ChatRequest req;
        req.model = "gpt-4o";
        req.messages.push_back({"user", "hi"});
        req.tool_choice = ChatRequest::tool_choice_auto();

        Json j = req;
        CHECK_FALSE(j.contains("tool_choice"));
    }

    TEST_CASE("tool_choice none serialises as string") {
        ChatRequest req;
        req.model = "gpt-4o";
        req.messages.push_back({"user", "hi"});
        req.tool_choice = ChatRequest::tool_choice_none();

        Json j = req;

        REQUIRE(j.contains("tool_choice"));
        CHECK(j["tool_choice"] == "none");

        ChatRequest req2 = j.get<ChatRequest>();
        REQUIRE(req2.tool_choice.has_value());
        CHECK(*req2.tool_choice == "none");
    }

    TEST_CASE("tool_choice auto string serialises as string") {
        ChatRequest req;
        req.model = "gpt-4o";
        req.messages.push_back({"user", "hi"});
        req.tool_choice = std::string{"auto"};

        Json j = req;

        REQUIRE(j.contains("tool_choice"));
        CHECK(j["tool_choice"] == "auto");
    }

    TEST_CASE("tool_choice function serialises as object") {
        ChatRequest req;
        req.model = "gpt-4o";
        req.messages.push_back({"user", "hi"});
        req.tool_choice = ChatRequest::tool_choice_function("read_file");

        Json j = req;

        REQUIRE(j.contains("tool_choice"));
        REQUIRE(j["tool_choice"].is_object());
        CHECK(j["tool_choice"]["type"] == "function");
        CHECK(j["tool_choice"]["function"]["name"] == "read_file");
    }

    TEST_CASE("tool_choice function round-trips through JSON") {
        ChatRequest req;
        req.model = "gpt-4o";
        req.messages.push_back({"user", "hi"});
        req.tool_choice = ChatRequest::tool_choice_function("bash");

        Json j = req;
        ChatRequest req2 = j.get<ChatRequest>();

        REQUIRE(req2.tool_choice.has_value());
        CHECK(*req2.tool_choice == "function:bash");
    }

    TEST_CASE("stream_options emitted when stream=true and include_usage=true") {
        ChatRequest req;
        req.model = "gpt-4o";
        req.messages.push_back({"user", "hi"});
        req.stream = true;
        req.stream_options_include_usage = true;

        Json j = req;

        REQUIRE(j.contains("stream_options"));
        CHECK(j["stream_options"]["include_usage"] == true);
    }

    TEST_CASE("stream_options NOT emitted when stream=false") {
        ChatRequest req;
        req.model = "gpt-4o";
        req.messages.push_back({"user", "hi"});
        req.stream = false;
        req.stream_options_include_usage = true;

        Json j = req;

        CHECK_FALSE(j.contains("stream_options"));
    }

    TEST_CASE("tools array serialised correctly") {
        ChatRequest req;
        req.model = "gpt-4o";
        req.messages.push_back({"user", "hi"});

        ToolDef td;
        td.type = "function";
        td.name = "glob";
        td.description = "Glob files.";
        td.schema = {{"type","object"},{"properties",{{"pattern",{{"type","string"}}}}}};
        req.tools.push_back(td);

        Json j = req;

        REQUIRE(j.contains("tools"));
        CHECK(j["tools"].size() == 1u);
        CHECK(j["tools"][0]["function"]["name"] == "glob");
    }

    TEST_CASE("full round-trip preserves all fields") {
        ChatRequest req;
        req.model = "claude-3-5-sonnet-20241022";
        req.messages.push_back({"system", "Be concise."});
        req.messages.push_back({"user", "list /tmp"});
        req.max_tokens   = 1024;
        req.temperature  = 0.3;
        req.stream       = true;
        req.stream_options_include_usage = true;
        req.tool_choice  = ChatRequest::tool_choice_none();

        ToolDef td;
        td.type = "function"; td.name = "bash";
        td.description = "Run a command.";
        td.schema = Json::object();
        req.tools.push_back(td);

        Json j = req;
        ChatRequest req2 = j.get<ChatRequest>();

        CHECK(req2.model == "claude-3-5-sonnet-20241022");
        CHECK(req2.messages.size() == 2u);
        CHECK(req2.messages[0].role == "system");
        CHECK(req2.max_tokens == 1024);
        CHECK(std::abs(*req2.temperature - 0.3) < 1e-9);
        CHECK(req2.stream == true);
        CHECK(req2.tools.size() == 1u);
        REQUIRE(req2.tool_choice.has_value());
        CHECK(*req2.tool_choice == "none");
    }
}

// ============================================================================
// TEST SUITE 5: UsageDelta
// ============================================================================
TEST_SUITE("UsageDelta") {

    TEST_CASE("round-trip basic usage") {
        UsageDelta u;
        u.prompt_tokens     = 150;
        u.completion_tokens = 50;
        u.total_tokens      = 200;
        u.cost_usd          = 0.0025;

        Json j = u;
        UsageDelta u2 = j.get<UsageDelta>();

        CHECK(u2.prompt_tokens     == 150);
        CHECK(u2.completion_tokens == 50);
        CHECK(u2.total_tokens      == 200);
        // cost_usd is locally computed; never read back from wire
        CHECK(u2.cost_usd == 0.0);
    }

    TEST_CASE("cost_usd not emitted on wire") {
        UsageDelta u;
        u.prompt_tokens = 10; u.completion_tokens = 5; u.total_tokens = 15;
        u.cost_usd = 9999.0;

        Json j = u;
        CHECK_FALSE(j.contains("cost_usd"));
    }

    TEST_CASE("missing fields default to zero") {
        Json j = Json::object();  // empty usage object
        UsageDelta u = j.get<UsageDelta>();

        CHECK(u.prompt_tokens == 0);
        CHECK(u.completion_tokens == 0);
        CHECK(u.total_tokens == 0);
    }
}

// ============================================================================
// TEST SUITE 6: ToolCallDelta
// ============================================================================
TEST_SUITE("ToolCallDelta") {

    TEST_CASE("first fragment — id, name, and initial arguments present") {
        // Simulate the first streaming delta for a tool call
        Json j = {
            {"index", 0},
            {"id",    "call_streamed_001"},
            {"type",  "function"},
            {"function", {
                {"name",      "read_file"},
                {"arguments", ""}
            }}
        };

        ToolCallDelta tcd = j.get<ToolCallDelta>();

        CHECK(tcd.index == 0);
        REQUIRE(tcd.id.has_value());
        CHECK(*tcd.id == "call_streamed_001");
        REQUIRE(tcd.name.has_value());
        CHECK(*tcd.name == "read_file");
        REQUIRE(tcd.arguments_fragment.has_value());
        CHECK(*tcd.arguments_fragment == "");
    }

    TEST_CASE("continuation fragment — only index and arguments_fragment") {
        Json j = {
            {"index", 0},
            {"type",  "function"},
            {"function", {
                {"arguments", "{\"path\":\"/tmp"}
            }}
        };

        ToolCallDelta tcd = j.get<ToolCallDelta>();

        CHECK(tcd.index == 0);
        CHECK_FALSE(tcd.id.has_value());
        CHECK_FALSE(tcd.name.has_value());
        REQUIRE(tcd.arguments_fragment.has_value());
        CHECK(*tcd.arguments_fragment == "{\"path\":\"/tmp");
    }

    TEST_CASE("round-trip preserves all optional fields") {
        ToolCallDelta tcd;
        tcd.index = 2;
        tcd.id    = "call_99";
        tcd.name  = "bash";
        tcd.arguments_fragment = "{\"cmd\":";

        Json j = tcd;
        ToolCallDelta tcd2 = j.get<ToolCallDelta>();

        CHECK(tcd2.index == 2);
        REQUIRE(tcd2.id.has_value());
        CHECK(*tcd2.id == "call_99");
        REQUIRE(tcd2.name.has_value());
        CHECK(*tcd2.name == "bash");
        REQUIRE(tcd2.arguments_fragment.has_value());
        CHECK(*tcd2.arguments_fragment == "{\"cmd\":");
    }

    TEST_CASE("multiple indices accumulation scenario") {
        // Two simultaneous tool calls arriving in the same chunk
        Json chunk = Json::array({
            {{"index",0},{"id","call_A"},{"type","function"},{"function",{{"name","glob"},{"arguments",""}}}},
            {{"index",1},{"id","call_B"},{"type","function"},{"function",{{"name","bash"},{"arguments",""}}}}
        });

        auto deltas = chunk.get<std::vector<ToolCallDelta>>();

        CHECK(deltas.size() == 2u);
        CHECK(deltas[0].index == 0);
        CHECK(*deltas[0].id == "call_A");
        CHECK(deltas[1].index == 1);
        CHECK(*deltas[1].id == "call_B");
    }
}

// ============================================================================
// TEST SUITE 7: StreamDelta
// ============================================================================
TEST_SUITE("StreamDelta") {

    TEST_CASE("content-only chunk") {
        Json j = {{"content", "Hello, world!"}};

        StreamDelta sd = j.get<StreamDelta>();

        REQUIRE(sd.content.has_value());
        CHECK(*sd.content == "Hello, world!");
        CHECK_FALSE(sd.tool_calls.has_value());
        CHECK_FALSE(sd.finish_reason.has_value());
        CHECK_FALSE(sd.usage.has_value());
    }

    TEST_CASE("empty content chunk (role announcement)") {
        Json j = {{"content", nullptr}, {"role", "assistant"}};

        StreamDelta sd = j.get<StreamDelta>();

        CHECK_FALSE(sd.content.has_value());
        CHECK_FALSE(sd.finish_reason.has_value());
    }

    TEST_CASE("finish_reason stop chunk — no content") {
        Json j = {{"content", nullptr}, {"finish_reason", "stop"}};

        StreamDelta sd = j.get<StreamDelta>();

        REQUIRE(sd.finish_reason.has_value());
        CHECK(*sd.finish_reason == "stop");
        CHECK_FALSE(sd.content.has_value());
    }

    TEST_CASE("finish_reason tool_calls chunk") {
        Json j = {{"finish_reason", "tool_calls"}};

        StreamDelta sd = j.get<StreamDelta>();

        REQUIRE(sd.finish_reason.has_value());
        CHECK(*sd.finish_reason == "tool_calls");
    }

    TEST_CASE("tool_calls streaming delta chunk") {
        Json j = {
            {"tool_calls", Json::array({
                {{"index",0},{"id","call_001"},{"type","function"},
                 {"function",{{"name","bash"},{"arguments","{\"cmd\":"}}}}
            })}
        };

        StreamDelta sd = j.get<StreamDelta>();

        REQUIRE(sd.tool_calls.has_value());
        CHECK(sd.tool_calls->size() == 1u);
        CHECK((*sd.tool_calls)[0].index == 0);
        REQUIRE((*sd.tool_calls)[0].id.has_value());
        CHECK(*(*sd.tool_calls)[0].id == "call_001");
        CHECK_FALSE(sd.content.has_value());
    }

    TEST_CASE("final usage chunk (stream_options.include_usage=true)") {
        Json j = {
            {"content", nullptr},
            {"finish_reason", nullptr},
            {"usage", {
                {"prompt_tokens", 120},
                {"completion_tokens", 35},
                {"total_tokens", 155}
            }}
        };

        StreamDelta sd = j.get<StreamDelta>();

        REQUIRE(sd.usage.has_value());
        CHECK(sd.usage->prompt_tokens == 120);
        CHECK(sd.usage->completion_tokens == 35);
        CHECK(sd.usage->total_tokens == 155);
    }

    TEST_CASE("round-trip: content chunk") {
        StreamDelta sd;
        sd.content = "partial text";

        Json j = sd;
        StreamDelta sd2 = j.get<StreamDelta>();

        REQUIRE(sd2.content.has_value());
        CHECK(*sd2.content == "partial text");
        CHECK_FALSE(sd2.tool_calls.has_value());
    }

    TEST_CASE("round-trip: finish_reason with usage") {
        StreamDelta sd;
        sd.finish_reason = "stop";
        sd.usage = UsageDelta{100, 40, 140, 0.0};

        Json j = sd;
        StreamDelta sd2 = j.get<StreamDelta>();

        REQUIRE(sd2.finish_reason.has_value());
        CHECK(*sd2.finish_reason == "stop");
        REQUIRE(sd2.usage.has_value());
        CHECK(sd2.usage->total_tokens == 140);
    }
}

// ============================================================================
// TEST SUITE 8: ChatResponse (non-streaming)
// ============================================================================
TEST_SUITE("ChatResponse") {

    TEST_CASE("content response round-trip") {
        ChatResponse cr;
        cr.id            = "chatcmpl-abc";
        cr.model         = "gpt-4o";
        cr.content       = "The answer is 42.";
        cr.finish_reason = "stop";
        cr.usage         = UsageDelta{80, 10, 90, 0.0};

        Json j = cr;
        ChatResponse cr2 = j.get<ChatResponse>();

        CHECK(cr2.id == "chatcmpl-abc");
        CHECK(cr2.model == "gpt-4o");
        REQUIRE(cr2.content.has_value());
        CHECK(*cr2.content == "The answer is 42.");
        CHECK(cr2.finish_reason == "stop");
        CHECK(cr2.usage.prompt_tokens == 80);
        CHECK(cr2.usage.completion_tokens == 10);
        CHECK_FALSE(cr2.tool_calls.has_value());
    }

    TEST_CASE("tool_calls response round-trip") {
        ChatResponse cr;
        cr.id            = "chatcmpl-xyz";
        cr.model         = "gpt-4o";
        cr.finish_reason = "tool_calls";
        cr.usage         = UsageDelta{200, 60, 260, 0.0};

        WireToolCall tc;
        tc.id = "call_001";
        tc.function.name = "read_file";
        tc.function.arguments = R"({"path":"/etc/hosts"})";
        cr.tool_calls = {tc};

        Json j = cr;
        ChatResponse cr2 = j.get<ChatResponse>();

        CHECK(cr2.finish_reason == "tool_calls");
        CHECK_FALSE(cr2.content.has_value());
        REQUIRE(cr2.tool_calls.has_value());
        CHECK(cr2.tool_calls->size() == 1u);
        CHECK((*cr2.tool_calls)[0].id == "call_001");
        CHECK((*cr2.tool_calls)[0].function.name == "read_file");
    }

    TEST_CASE("null content serialised and deserialised correctly") {
        ChatResponse cr;
        cr.id = "chatcmpl-null"; cr.model = "gpt-4o";
        cr.finish_reason = "tool_calls";
        cr.usage = UsageDelta{};

        Json j = cr;

        CHECK(j["content"].is_null());
        ChatResponse cr2 = j.get<ChatResponse>();
        CHECK_FALSE(cr2.content.has_value());
    }

    TEST_CASE("finish_reason defaults to stop when absent") {
        Json j = {
            {"id",    "chatcmpl-minimal"},
            {"model", "gpt-4o-mini"},
            {"content", "hi"},
            {"usage", {{"prompt_tokens",5},{"completion_tokens",2},{"total_tokens",7}}}
        };

        ChatResponse cr = j.get<ChatResponse>();
        CHECK(cr.finish_reason == "stop");
    }
}

// ============================================================================
// TEST SUITE 9: Real SSE fixture
// ============================================================================
TEST_SUITE("Real SSE fixture") {

    TEST_CASE("deserialise real OpenAI streaming content delta") {
        // This is the exact JSON extracted from a real OpenAI SSE response chunk
        // (data: prefix stripped, choices[0].delta extracted):
        const std::string raw = R"({
            "id": "chatcmpl-Atest001",
            "object": "chat.completion.chunk",
            "created": 1716123456,
            "model": "gpt-4o-2024-08-06",
            "choices": [
                {
                    "index": 0,
                    "delta": {"role": "assistant", "content": "Hello"},
                    "finish_reason": null
                }
            ]
        })";

        auto jres = parse(raw);
        REQUIRE(jres.has_value());

        const auto& chunk = jres.value();
        const auto& delta = chunk["choices"][0]["delta"];

        StreamDelta sd = delta.get<StreamDelta>();

        REQUIRE(sd.content.has_value());
        CHECK(*sd.content == "Hello");
        CHECK_FALSE(sd.finish_reason.has_value());
    }

    TEST_CASE("deserialise real OpenAI streaming tool_call first delta") {
        const std::string raw = R"({
            "id": "chatcmpl-Atest002",
            "object": "chat.completion.chunk",
            "created": 1716123457,
            "model": "gpt-4o-2024-08-06",
            "choices": [
                {
                    "index": 0,
                    "delta": {
                        "role": "assistant",
                        "content": null,
                        "tool_calls": [
                            {
                                "index": 0,
                                "id": "call_real001",
                                "type": "function",
                                "function": {
                                    "name": "bash",
                                    "arguments": ""
                                }
                            }
                        ]
                    },
                    "finish_reason": null
                }
            ]
        })";

        auto jres = parse(raw);
        REQUIRE(jres.has_value());

        const auto& delta = jres.value()["choices"][0]["delta"];
        StreamDelta sd = delta.get<StreamDelta>();

        CHECK_FALSE(sd.content.has_value());
        REQUIRE(sd.tool_calls.has_value());
        CHECK(sd.tool_calls->size() == 1u);
        CHECK((*sd.tool_calls)[0].index == 0);
        REQUIRE((*sd.tool_calls)[0].id.has_value());
        CHECK(*(*sd.tool_calls)[0].id == "call_real001");
        REQUIRE((*sd.tool_calls)[0].name.has_value());
        CHECK(*(*sd.tool_calls)[0].name == "bash");
    }

    TEST_CASE("deserialise real OpenAI streaming tool_call argument fragment") {
        const std::string raw = R"({
            "id": "chatcmpl-Atest003",
            "object": "chat.completion.chunk",
            "created": 1716123458,
            "model": "gpt-4o-2024-08-06",
            "choices": [
                {
                    "index": 0,
                    "delta": {
                        "tool_calls": [
                            {
                                "index": 0,
                                "type": "function",
                                "function": {
                                    "arguments": "{\"command\":\"ls -la\""
                                }
                            }
                        ]
                    },
                    "finish_reason": null
                }
            ]
        })";

        auto jres = parse(raw);
        REQUIRE(jres.has_value());

        const auto& delta = jres.value()["choices"][0]["delta"];
        StreamDelta sd = delta.get<StreamDelta>();

        REQUIRE(sd.tool_calls.has_value());
        const auto& tcd = (*sd.tool_calls)[0];
        CHECK(tcd.index == 0);
        CHECK_FALSE(tcd.id.has_value());
        CHECK_FALSE(tcd.name.has_value());
        REQUIRE(tcd.arguments_fragment.has_value());
        CHECK(*tcd.arguments_fragment == "{\"command\":\"ls -la\"");
    }

    TEST_CASE("deserialise real OpenAI finish_reason=stop delta") {
        const std::string raw = R"({
            "choices": [
                {
                    "index": 0,
                    "delta": {"content": null},
                    "finish_reason": "stop"
                }
            ]
        })";

        auto jres = parse(raw);
        REQUIRE(jres.has_value());

        // finish_reason is on the choice, not the delta — extract manually
        const auto& choice = jres.value()["choices"][0];
        StreamDelta sd = choice["delta"].get<StreamDelta>();

        // Simulate what the SSE parser does: pull finish_reason from choice level
        if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
            sd.finish_reason = choice["finish_reason"].get<std::string>();
        }

        REQUIRE(sd.finish_reason.has_value());
        CHECK(*sd.finish_reason == "stop");
    }

    TEST_CASE("deserialise usage-only final chunk") {
        const std::string raw = R"({
            "id": "chatcmpl-Afinal",
            "object": "chat.completion.chunk",
            "created": 1716123460,
            "model": "gpt-4o-2024-08-06",
            "choices": [],
            "usage": {
                "prompt_tokens": 253,
                "completion_tokens": 87,
                "total_tokens": 340
            }
        })";

        auto jres = parse(raw);
        REQUIRE(jres.has_value());

        // The SSE parser extracts usage from the top-level field into StreamDelta
        StreamDelta sd;
        if (jres.value().contains("usage") && jres.value()["usage"].is_object()) {
            sd.usage = jres.value()["usage"].get<UsageDelta>();
        }

        REQUIRE(sd.usage.has_value());
        CHECK(sd.usage->prompt_tokens == 253);
        CHECK(sd.usage->completion_tokens == 87);
        CHECK(sd.usage->total_tokens == 340);
    }
}
