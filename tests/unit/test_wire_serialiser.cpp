// tests/unit/test_wire_serialiser.cpp
// ---------------------------------------------------------------------------
// PEXT2 4.1 — D-3: hand-rolled wire serialiser property tests + benchmark.
//
// Test suites:
//   1. ByteIdentical  — to_wire_string(req, out) == nlohmann::json(req).dump()
//      for a range of representative ChatRequest payloads including:
//        - minimal request (model + messages only, stream=true)
//        - with max_tokens
//        - with temperature + top_p
//        - with tools (simple schema)
//        - with tool_calls in message history
//        - with tool_call_id (tool result messages)
//        - with tool_choice variants (none / auto / function:<n>)
//        - stream_options emit/suppress
//        - full production-shape payload (all fields)
//        - string escaping edge cases (backslash, quote, newline, tab,
//          carriage-return, backspace, formfeed, control char, non-ASCII UTF-8)
//        - streaming=false (stream_options must not be emitted)
//
//   2. RoundTrip      — to_wire_string → nlohmann::parse → nlohmann::dump
//      must equal nlohmann::json(req).dump() (indirect round-trip verification)
//
//   3. Benchmark      — to_wire_string vs nlohmann::json(req).dump() on a
//      representative 50-message, 40-tool request.  Passes when
//      to_wire_string throughput ≥ 1.5× nlohmann dump throughput.
//
// Build (standalone, from repo root):
//   c++ -std=c++20 -O2 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_wire_serialiser.cpp src/inference/ChatRequest.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       -o /tmp/test_wire_serialiser && /tmp/test_wire_serialiser
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/inference/ChatRequest.hpp>
#include <batbox/inference/ChatResponse.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace batbox;
using namespace batbox::inference;

// ---------------------------------------------------------------------------
// Helper: build a nlohmann dump string for a ChatRequest
// ---------------------------------------------------------------------------
static std::string nlohmann_dump(const ChatRequest& req) {
    Json j = req;
    return j.dump();
}

// ---------------------------------------------------------------------------
// Helper: build to_wire_string output
// ---------------------------------------------------------------------------
static std::string wire_str(const ChatRequest& req) {
    std::string out;
    out.reserve(4096);
    to_wire_string(req, out);
    return out;
}

// ---------------------------------------------------------------------------
// Helper: build a simple ToolDef
// ---------------------------------------------------------------------------
static ToolDef make_tool(const std::string& name, const std::string& desc) {
    ToolDef td;
    td.type        = "function";
    td.name        = name;
    td.description = desc;
    td.schema      = {
        {"type", "object"},
        {"properties", {
            {"path", {{"type","string"},{"description","File path"}}}
        }},
        {"required", Json::array({"path"})}
    };
    return td;
}

// ============================================================================
// TEST SUITE 1: ByteIdentical
// ============================================================================
TEST_SUITE("ByteIdentical") {

    TEST_CASE("minimal request — model + one user message + stream=true") {
        ChatRequest req;
        req.model = "gpt-4o";
        req.stream = true;
        req.stream_options_include_usage = std::nullopt;  // omit
        req.messages.push_back({"user", "Hello!"});

        CHECK(wire_str(req) == nlohmann_dump(req));
    }

    TEST_CASE("stream_options emitted when stream=true and include_usage=true") {
        ChatRequest req;
        req.model = "gpt-4o";
        req.stream = true;
        req.stream_options_include_usage = true;
        req.messages.push_back({"user", "hi"});

        CHECK(wire_str(req) == nlohmann_dump(req));
    }

    TEST_CASE("stream_options NOT emitted when stream=false") {
        ChatRequest req;
        req.model = "gpt-4o-mini";
        req.stream = false;
        req.stream_options_include_usage = true;
        req.messages.push_back({"user", "ping"});

        const std::string got = wire_str(req);
        const std::string expected = nlohmann_dump(req);
        CHECK(got == expected);
        // Confirm stream_options is absent
        CHECK(got.find("stream_options") == std::string::npos);
    }

    TEST_CASE("with max_tokens") {
        ChatRequest req;
        req.model = "claude-3-5-sonnet-20241022";
        req.max_tokens = 1024;
        req.stream = true;
        req.stream_options_include_usage = true;
        req.messages.push_back({"user", "count to ten"});

        CHECK(wire_str(req) == nlohmann_dump(req));
    }

    TEST_CASE("with temperature and top_p") {
        ChatRequest req;
        req.model = "gpt-4o";
        req.temperature = 0.7;
        req.top_p = 0.9;
        req.stream = true;
        req.stream_options_include_usage = true;
        req.messages.push_back({"system", "You are helpful."});
        req.messages.push_back({"user", "tell me a joke"});

        CHECK(wire_str(req) == nlohmann_dump(req));
    }

    TEST_CASE("temperature = 1.0 (whole-number double must have decimal point)") {
        ChatRequest req;
        req.model = "gpt-4o";
        req.temperature = 1.0;
        req.stream = true;
        req.stream_options_include_usage = std::nullopt;
        req.messages.push_back({"user", "hi"});

        const std::string got = wire_str(req);
        const std::string expected = nlohmann_dump(req);
        CHECK(got == expected);
        // nlohmann emits "1.0" not "1" for whole-number doubles
        CHECK(got.find("\"temperature\":1.0") != std::string::npos);
    }

    TEST_CASE("with one tool") {
        ChatRequest req;
        req.model = "gpt-4o";
        req.stream = true;
        req.stream_options_include_usage = true;
        req.messages.push_back({"system", "You can read files."});
        req.messages.push_back({"user", "read /etc/hosts"});
        req.tools.push_back(make_tool("read_file", "Read a file."));
        req.tool_choice = std::string{"auto"};

        CHECK(wire_str(req) == nlohmann_dump(req));
    }

    TEST_CASE("with multiple tools") {
        ChatRequest req;
        req.model = "gpt-4o";
        req.stream = true;
        req.stream_options_include_usage = true;
        req.messages.push_back({"user", "list files then read one"});
        req.tools.push_back(make_tool("read_file",  "Read a file."));
        req.tools.push_back(make_tool("glob",       "Glob files."));
        req.tools.push_back(make_tool("bash",       "Run a command."));
        req.tool_choice = std::string{"auto"};

        CHECK(wire_str(req) == nlohmann_dump(req));
    }

    TEST_CASE("tool_choice none") {
        ChatRequest req;
        req.model = "gpt-4o";
        req.stream = true;
        req.stream_options_include_usage = std::nullopt;
        req.messages.push_back({"user", "answer without tools"});
        req.tools.push_back(make_tool("bash", "Run bash."));
        req.tool_choice = ChatRequest::tool_choice_none();

        CHECK(wire_str(req) == nlohmann_dump(req));
    }

    TEST_CASE("tool_choice function:<name>") {
        ChatRequest req;
        req.model = "gpt-4o";
        req.stream = true;
        req.stream_options_include_usage = std::nullopt;
        req.messages.push_back({"user", "force read"});
        req.tools.push_back(make_tool("read_file", "Read."));
        req.tool_choice = ChatRequest::tool_choice_function("read_file");

        CHECK(wire_str(req) == nlohmann_dump(req));
    }

    TEST_CASE("assistant message with tool_calls in history") {
        // Build a history that contains a tool-call assistant message and a
        // tool-result message to verify WireToolCall + tool_call_id serialisation.
        ChatRequest req;
        req.model = "gpt-4o";
        req.stream = true;
        req.stream_options_include_usage = true;
        req.tools.push_back(make_tool("bash", "Run bash."));
        req.tool_choice = std::string{"auto"};

        // System + user
        req.messages.push_back({"system", "You can run bash."});
        req.messages.push_back({"user", "list /tmp"});

        // Assistant message with tool_calls (null content)
        WireMessage asst_tc;
        asst_tc.role = "assistant";
        // content is nullopt — model is doing tool use

        WireToolCall tc;
        tc.id = "call_001";
        tc.function.name = "bash";
        tc.function.arguments = R"({"command":"ls /tmp"})";
        asst_tc.tool_calls = {tc};
        req.messages.push_back(std::move(asst_tc));

        // Tool result message
        WireMessage tool_result;
        tool_result.role = "tool";
        tool_result.content = "a.txt\nb.txt\nc.txt";
        tool_result.tool_call_id = "call_001";
        tool_result.name = "bash";
        req.messages.push_back(std::move(tool_result));

        // Follow-up user message
        req.messages.push_back({"user", "which is the largest?"});

        CHECK(wire_str(req) == nlohmann_dump(req));
    }

    TEST_CASE("WireMessage with non-empty name field") {
        ChatRequest req;
        req.model = "gpt-4o";
        req.stream = true;
        req.stream_options_include_usage = std::nullopt;

        WireMessage msg;
        msg.role = "user";
        msg.content = "hello from agent";
        msg.name = "sub_agent_1";
        req.messages.push_back(std::move(msg));

        CHECK(wire_str(req) == nlohmann_dump(req));
    }

    TEST_CASE("string escaping — backslash and double quote") {
        ChatRequest req;
        req.model = "gpt-4o";
        req.stream = true;
        req.stream_options_include_usage = std::nullopt;

        WireMessage msg;
        msg.role = "user";
        msg.content = R"(path is "C:\Users\foo" ok?)";
        req.messages.push_back(std::move(msg));

        CHECK(wire_str(req) == nlohmann_dump(req));
    }

    TEST_CASE("string escaping — newline, tab, carriage-return") {
        ChatRequest req;
        req.model = "gpt-4o";
        req.stream = true;
        req.stream_options_include_usage = std::nullopt;

        WireMessage msg;
        msg.role = "user";
        msg.content = std::string("line1\nline2\ttab\rCR");
        req.messages.push_back(std::move(msg));

        CHECK(wire_str(req) == nlohmann_dump(req));
    }

    TEST_CASE("string escaping — backspace (0x08) and formfeed (0x0C)") {
        ChatRequest req;
        req.model = "gpt-4o";
        req.stream = true;
        req.stream_options_include_usage = std::nullopt;

        std::string s = "a";
        s += '\x08';  // backspace
        s += "b";
        s += '\x0c';  // formfeed
        s += "c";

        WireMessage msg;
        msg.role = "user";
        msg.content = s;
        req.messages.push_back(std::move(msg));

        CHECK(wire_str(req) == nlohmann_dump(req));
    }

    TEST_CASE("string escaping — control character 0x01 as \\u0001") {
        ChatRequest req;
        req.model = "gpt-4o";
        req.stream = true;
        req.stream_options_include_usage = std::nullopt;

        std::string s = "start";
        s += '\x01';
        s += "end";

        WireMessage msg;
        msg.role = "user";
        msg.content = s;
        req.messages.push_back(std::move(msg));

        const std::string got = wire_str(req);
        const std::string expected = nlohmann_dump(req);
        CHECK(got == expected);
        CHECK(got.find("\\u0001") != std::string::npos);
    }

    TEST_CASE("string escaping — multi-byte UTF-8 passed through verbatim") {
        ChatRequest req;
        req.model = "gpt-4o";
        req.stream = true;
        req.stream_options_include_usage = std::nullopt;

        // U+201C LEFT DOUBLE QUOTATION MARK, U+201D RIGHT DOUBLE QUOTATION MARK
        WireMessage msg;
        msg.role = "user";
        msg.content = "\xe2\x80\x9chello\xe2\x80\x9d";
        req.messages.push_back(std::move(msg));

        CHECK(wire_str(req) == nlohmann_dump(req));
    }

    TEST_CASE("empty messages array") {
        ChatRequest req;
        req.model = "gpt-4o";
        req.stream = true;
        req.stream_options_include_usage = std::nullopt;
        // messages is empty

        CHECK(wire_str(req) == nlohmann_dump(req));
    }

    TEST_CASE("full production-shape payload — all optional fields set") {
        ChatRequest req;
        req.model = "claude-3-5-sonnet-20241022";
        req.max_tokens   = 4096;
        req.temperature  = 0.3;
        req.top_p        = 0.95;
        req.stream       = true;
        req.stream_options_include_usage = true;
        req.tools.push_back(make_tool("read_file",  "Read a file."));
        req.tools.push_back(make_tool("bash",       "Run bash."));
        req.tool_choice  = std::string{"auto"};

        req.messages.push_back({"system", "You are a helpful coding assistant."});
        req.messages.push_back({"user",   "refactor the auth module"});

        WireMessage asst;
        asst.role = "assistant";
        asst.content = "I'll start by reading the auth module.";
        req.messages.push_back(std::move(asst));

        WireMessage asst_tc;
        asst_tc.role = "assistant";
        WireToolCall tc;
        tc.id = "call_auth_read";
        tc.function.name = "read_file";
        tc.function.arguments = R"({"path":"src/auth/auth.py"})";
        asst_tc.tool_calls = {tc};
        req.messages.push_back(std::move(asst_tc));

        WireMessage tool_res;
        tool_res.role = "tool";
        tool_res.content = "def authenticate(user, pw):\n    return check_hash(pw, user.hash)";
        tool_res.tool_call_id = "call_auth_read";
        tool_res.name = "read_file";
        req.messages.push_back(std::move(tool_res));

        req.messages.push_back({"user", "looks good — now add logging"});

        CHECK(wire_str(req) == nlohmann_dump(req));
    }

    TEST_CASE("WireToolCall with special characters in arguments (JSON passthrough)") {
        // The arguments field is already a JSON string — it should be JSON-escaped
        // as a string value (i.e. the inner quotes get escaped).
        ChatRequest req;
        req.model = "gpt-4o";
        req.stream = true;
        req.stream_options_include_usage = std::nullopt;

        WireMessage asst_tc;
        asst_tc.role = "assistant";
        WireToolCall tc;
        tc.id = "call_complex";
        tc.function.name = "bash";
        tc.function.arguments = R"({"command":"echo \"hello world\"\nls -la"})";
        asst_tc.tool_calls = {tc};
        req.messages.push_back(std::move(asst_tc));

        CHECK(wire_str(req) == nlohmann_dump(req));
    }

    TEST_CASE("model name with special chars (hyphen, dot, digit)") {
        ChatRequest req;
        req.model = "claude-3.5-sonnet-20241022";
        req.stream = true;
        req.stream_options_include_usage = std::nullopt;
        req.messages.push_back({"user", "hi"});

        CHECK(wire_str(req) == nlohmann_dump(req));
    }
}

// ============================================================================
// TEST SUITE 2: RoundTrip
//
// Indirect verification: to_wire_string → nlohmann::parse → nlohmann::dump
// must equal nlohmann::json(req).dump().
//
// This catches any structural difference even if the raw string comparison
// would miss it due to whitespace or key-order issues.
// ============================================================================
TEST_SUITE("RoundTrip") {

    TEST_CASE("minimal request round-trips correctly") {
        ChatRequest req;
        req.model = "gpt-4o";
        req.stream = true;
        req.stream_options_include_usage = true;
        req.messages.push_back({"user", "Hello!"});

        const std::string wire = wire_str(req);

        // Parse the wire string with nlohmann
        auto parsed = Json::parse(wire);
        CHECK(parsed.dump() == nlohmann_dump(req));
    }

    TEST_CASE("full-payload request round-trips correctly") {
        ChatRequest req;
        req.model = "gpt-4o";
        req.max_tokens = 512;
        req.temperature = 0.7;
        req.top_p = 0.9;
        req.stream = true;
        req.stream_options_include_usage = true;
        req.tools.push_back(make_tool("bash", "Run bash."));
        req.tool_choice = std::string{"auto"};

        req.messages.push_back({"system", "Be helpful."});
        req.messages.push_back({"user", "run ls"});

        WireMessage asst_tc;
        asst_tc.role = "assistant";
        WireToolCall tc;
        tc.id = "call_ls";
        tc.function.name = "bash";
        tc.function.arguments = R"({"command":"ls"})";
        asst_tc.tool_calls = {tc};
        req.messages.push_back(std::move(asst_tc));

        WireMessage tool_res;
        tool_res.role = "tool";
        tool_res.content = "README.md\nsrc/\n";
        tool_res.tool_call_id = "call_ls";
        tool_res.name = "bash";
        req.messages.push_back(std::move(tool_res));

        const std::string wire = wire_str(req);
        auto parsed = Json::parse(wire);
        CHECK(parsed.dump() == nlohmann_dump(req));
    }

    TEST_CASE("parsed wire string deserialises to equivalent ChatRequest") {
        ChatRequest req;
        req.model = "claude-3-5-sonnet-20241022";
        req.max_tokens = 1024;
        req.temperature = 0.3;
        req.stream = true;
        req.stream_options_include_usage = true;
        req.messages.push_back({"system", "You are a coder."});
        req.messages.push_back({"user", "write a fibonacci function"});

        const std::string wire = wire_str(req);
        auto j = Json::parse(wire);
        ChatRequest req2 = j.get<ChatRequest>();

        CHECK(req2.model == req.model);
        CHECK(req2.max_tokens == req.max_tokens);
        CHECK(req2.temperature == req.temperature);
        CHECK(req2.stream == req.stream);
        CHECK(req2.messages.size() == req.messages.size());
        CHECK(*req2.messages[0].content == *req.messages[0].content);
        CHECK(*req2.messages[1].content == *req.messages[1].content);
    }
}

// ============================================================================
// TEST SUITE 3: Benchmark
//
// Compares to_wire_string throughput vs nlohmann::json(req).dump() on a
// representative large payload (50 messages, 40 tools, all optional fields set).
//
// Requirement: to_wire_string must be ≥ 1.5× faster.  The benchmark uses wall
// clock time over a fixed iteration count and prints timings to stdout.
// ============================================================================
TEST_SUITE("Benchmark") {

    // Build a large representative ChatRequest
    static ChatRequest make_large_request() {
        ChatRequest req;
        req.model = "claude-3-5-sonnet-20241022";
        req.max_tokens  = 4096;
        req.temperature = 0.7;
        req.top_p       = 0.95;
        req.stream      = true;
        req.stream_options_include_usage = true;
        req.tool_choice = std::string{"auto"};

        // 40 tools with realistic schemas
        for (int i = 0; i < 40; ++i) {
            ToolDef td;
            td.type        = "function";
            td.name        = "tool_" + std::to_string(i);
            td.description = "Tool number " + std::to_string(i) + " does something useful with files.";
            td.schema      = {
                {"type", "object"},
                {"properties", {
                    {"path",    {{"type","string"},{"description","File path to operate on"}}},
                    {"mode",    {{"type","string"},{"enum", Json::array({"read","write","append"})}}},
                    {"limit",   {{"type","integer"},{"minimum", 1},{"maximum", 65536}}}
                }},
                {"required", Json::array({"path"})}
            };
            req.tools.push_back(std::move(td));
        }

        // 50-message history alternating user / assistant / tool turns
        req.messages.push_back({"system",
            "You are a helpful AI coding assistant with access to 40 tools. "
            "Use them efficiently and always verify your work."});

        for (int i = 0; i < 16; ++i) {
            WireMessage user_msg;
            user_msg.role    = "user";
            user_msg.content = "Please process file number " + std::to_string(i) +
                               " and tell me what it contains. "
                               "The file is located at /data/files/file_" +
                               std::to_string(i) + ".txt and may contain binary or text data.";
            req.messages.push_back(std::move(user_msg));

            WireMessage asst_tc;
            asst_tc.role = "assistant";
            WireToolCall tc;
            tc.id = "call_" + std::to_string(i);
            tc.function.name = "tool_0";
            tc.function.arguments = R"({"path":"/data/files/file_)" +
                                    std::to_string(i) + R"(.txt","mode":"read"})";
            asst_tc.tool_calls = {tc};
            req.messages.push_back(std::move(asst_tc));

            WireMessage tool_res;
            tool_res.role        = "tool";
            tool_res.content     = "Line 1 of file " + std::to_string(i) + "\n"
                                   "Line 2 of file " + std::to_string(i) + "\n"
                                   "Line 3 of file " + std::to_string(i) + "\n";
            tool_res.tool_call_id = "call_" + std::to_string(i);
            tool_res.name         = "tool_0";
            req.messages.push_back(std::move(tool_res));
        }

        return req;
    }

    TEST_CASE("to_wire_string is >= 1.5x faster than nlohmann dump on large payload") {
        const ChatRequest req = make_large_request();

        // First verify correctness on this large payload
        CHECK(wire_str(req) == nlohmann_dump(req));

        // Warm-up
        for (int i = 0; i < 10; ++i) {
            volatile auto _ = wire_str(req).size();
            volatile auto __ = nlohmann_dump(req).size();
            (void)_; (void)__;
        }

        constexpr int kIter = 500;

        // Benchmark nlohmann dump
        auto t0_nloh = std::chrono::steady_clock::now();
        std::size_t nloh_bytes = 0;
        for (int i = 0; i < kIter; ++i) {
            nloh_bytes += nlohmann_dump(req).size();
        }
        auto t1_nloh = std::chrono::steady_clock::now();
        const double nloh_us = static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                t1_nloh - t0_nloh).count());

        // Benchmark to_wire_string
        auto t0_wire = std::chrono::steady_clock::now();
        std::size_t wire_bytes = 0;
        for (int i = 0; i < kIter; ++i) {
            wire_bytes += wire_str(req).size();
        }
        auto t1_wire = std::chrono::steady_clock::now();
        const double wire_us = static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                t1_wire - t0_wire).count());

        // Throughput: bytes/us (higher is better)
        const double nloh_tput = static_cast<double>(nloh_bytes) / nloh_us;
        const double wire_tput = static_cast<double>(wire_bytes) / wire_us;
        const double speedup   = wire_tput / nloh_tput;

        printf("\n[bench] payload_size=%zu bytes\n", wire_bytes / kIter);
        printf("[bench] nlohmann dump: %.1f us/iter (%.2f MB/s)\n",
               nloh_us / kIter, nloh_tput);
        printf("[bench] to_wire_string: %.1f us/iter (%.2f MB/s)\n",
               wire_us / kIter, wire_tput);
        printf("[bench] speedup: %.2fx\n", speedup);

        // Verify the output sizes match (they should, since byte-identical)
        CHECK(wire_bytes == nloh_bytes);

        // Requirement: to_wire_string must be >= 1.5x faster.
        // If this fails, the optimization didn't ship — the D-3 savings are gone.
        CHECK_MESSAGE(speedup >= 1.5,
            "to_wire_string must be >=1.5x faster than nlohmann dump. "
            "Got " << speedup << "x. "
            "Check that BATBOX_FAST_WIRE_JSON is ON and optimisations are enabled (-O2).");
    }
}
