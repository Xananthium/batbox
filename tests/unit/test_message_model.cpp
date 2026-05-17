// =============================================================================
// tests/unit/test_message_model.cpp — doctest suite for Message model (CPP 3.1)
//
// Coverage:
//   1.  Message default-constructor assigns non-empty UUID and non-epoch ts
//   2.  Two default-constructed Messages get distinct UUIDs
//   3.  to_wire_role — all four roles map to the correct strings
//   4.  role_from_string — round-trip for all four wire strings
//   5.  role_from_string — throws on unknown input
//   6.  to_json / from_json round-trip — user message (no optional fields)
//   7.  to_json / from_json round-trip — assistant message with tool_calls + usage
//   8.  to_json / from_json round-trip — tool result message (tool_call_id, tool_name, is_error)
//   9.  to_json / from_json round-trip — system message
//   10. text() helper returns content by string_view
//   11. is_tool_call() true for assistant + tool_calls present; false otherwise
//   12. is_tool_result() true for Role::Tool only
//   13. from_json throws on missing 'id' field
//   14. from_json throws on missing 'role' field
//   15. from_json throws on unknown role string
//   16. Timestamp round-trip preserves millisecond precision
//   17. ToolCall arguments round-trip as arbitrary JSON objects
//   18. is_error=false round-trips correctly
//   19. Empty tool_calls vector not serialised as present field
//   20. Usage fields round-trip with floating-point fidelity
//
// Build standalone (no CMake, from repo root):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_message_model.cpp \
//       src/conversation/Message.cpp \
//       src/core/Uuid.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       -o /tmp/test_message_model && /tmp/test_message_model
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/conversation/Message.hpp>
#include <batbox/core/Json.hpp>

#include <chrono>
#include <cmath>
#include <stdexcept>
#include <string>
#include <thread>

using namespace batbox::conversation;
using batbox::Json;

// ---------------------------------------------------------------------------
// Helper: check that a string looks like a UUID (36 chars, dashes in right places)
// ---------------------------------------------------------------------------
static bool is_uuid_shaped(const std::string& s) {
    if (s.size() != 36) return false;
    if (s[8]  != '-') return false;
    if (s[13] != '-') return false;
    if (s[18] != '-') return false;
    if (s[23] != '-') return false;
    return true;
}

// =============================================================================
// 1. Default constructor — UUID + timestamp
// =============================================================================

TEST_CASE("Message default-constructor assigns a non-empty UUID v4") {
    Message m;
    CHECK_FALSE(m.id.empty());
    CHECK(is_uuid_shaped(m.id));
}

TEST_CASE("Message default-constructor sets ts to approximately now") {
    using Clock = std::chrono::system_clock;
    auto before = Clock::now();
    Message m;
    auto after = Clock::now();
    CHECK(m.ts >= before);
    CHECK(m.ts <= after);
}

// =============================================================================
// 2. Two Messages get distinct UUIDs
// =============================================================================

TEST_CASE("Two default-constructed Messages have distinct ids") {
    Message a;
    Message b;
    CHECK(a.id != b.id);
}

// =============================================================================
// 3. to_wire_role — all four roles
// =============================================================================

TEST_CASE("to_wire_role returns correct strings for all roles") {
    CHECK(to_wire_role(Role::System)    == "system");
    CHECK(to_wire_role(Role::User)      == "user");
    CHECK(to_wire_role(Role::Assistant) == "assistant");
    CHECK(to_wire_role(Role::Tool)      == "tool");
}

// =============================================================================
// 4. role_from_string round-trip
// =============================================================================

TEST_CASE("role_from_string round-trips all four wire strings") {
    CHECK(role_from_string("system")    == Role::System);
    CHECK(role_from_string("user")      == Role::User);
    CHECK(role_from_string("assistant") == Role::Assistant);
    CHECK(role_from_string("tool")      == Role::Tool);
}

// =============================================================================
// 5. role_from_string throws on unknown input
// =============================================================================

TEST_CASE("role_from_string throws std::invalid_argument on unknown role") {
    CHECK_THROWS_AS(role_from_string("unknown"),   std::invalid_argument);
    CHECK_THROWS_AS(role_from_string(""),          std::invalid_argument);
    CHECK_THROWS_AS(role_from_string("ASSISTANT"), std::invalid_argument); // case-sensitive
}

// =============================================================================
// 6. to_json / from_json round-trip — simple user message
// =============================================================================

TEST_CASE("to_json / from_json round-trip: user message with no optional fields") {
    Message original;
    original.role    = Role::User;
    original.content = "Hello, world!";

    Json j = to_json(original);

    // Verify JSON structure
    CHECK(j["id"].get<std::string>() == original.id);
    CHECK(j["role"].get<std::string>() == "user");
    CHECK(j["content"].get<std::string>() == "Hello, world!");
    CHECK(j.contains("ts"));
    CHECK_FALSE(j.contains("tool_calls"));
    CHECK_FALSE(j.contains("tool_call_id"));
    CHECK_FALSE(j.contains("tool_name"));
    CHECK_FALSE(j.contains("is_error"));
    CHECK_FALSE(j.contains("usage"));

    // Deserialise and compare
    Message restored = from_json(j);
    CHECK(restored.id      == original.id);
    CHECK(restored.role    == Role::User);
    CHECK(restored.content == "Hello, world!");
    CHECK_FALSE(restored.tool_calls.has_value());
    CHECK_FALSE(restored.tool_call_id.has_value());
    CHECK_FALSE(restored.tool_name.has_value());
    CHECK_FALSE(restored.is_error.has_value());
    CHECK_FALSE(restored.usage.has_value());
}

// =============================================================================
// 7. to_json / from_json round-trip — assistant message with tool_calls + usage
// =============================================================================

TEST_CASE("to_json / from_json round-trip: assistant message with tool_calls and usage") {
    Message original;
    original.role    = Role::Assistant;
    original.content = "I will read that file for you.";

    // Two tool calls
    ToolCall tc1;
    tc1.id        = "call_001";
    tc1.name      = "ReadTool";
    tc1.arguments = {{"path", "/tmp/foo.txt"}};

    ToolCall tc2;
    tc2.id        = "call_002";
    tc2.name      = "GlobTool";
    tc2.arguments = {{"pattern", "*.cpp"}, {"recursive", true}};

    original.tool_calls = std::vector<ToolCall>{tc1, tc2};

    // Usage
    UsageDelta ud;
    ud.prompt_tokens     = 150;
    ud.completion_tokens = 42;
    ud.total_tokens      = 192;
    ud.cost_usd          = 0.000576;
    original.usage = ud;

    Json j = to_json(original);

    // Verify JSON structure
    CHECK(j["role"].get<std::string>() == "assistant");
    REQUIRE(j.contains("tool_calls"));
    REQUIRE(j["tool_calls"].is_array());
    CHECK(j["tool_calls"].size() == 2u);

    CHECK(j["tool_calls"][0]["id"].get<std::string>()   == "call_001");
    CHECK(j["tool_calls"][0]["name"].get<std::string>() == "ReadTool");
    CHECK(j["tool_calls"][0]["arguments"]["path"]       == "/tmp/foo.txt");

    CHECK(j["tool_calls"][1]["id"].get<std::string>()   == "call_002");
    CHECK(j["tool_calls"][1]["name"].get<std::string>() == "GlobTool");
    CHECK(j["tool_calls"][1]["arguments"]["recursive"]  == true);

    REQUIRE(j.contains("usage"));
    CHECK(j["usage"]["prompt_tokens"].get<int>()     == 150);
    CHECK(j["usage"]["completion_tokens"].get<int>() == 42);
    CHECK(j["usage"]["total_tokens"].get<int>()      == 192);

    // Deserialise
    Message restored = from_json(j);
    CHECK(restored.id      == original.id);
    CHECK(restored.role    == Role::Assistant);
    CHECK(restored.content == "I will read that file for you.");

    REQUIRE(restored.tool_calls.has_value());
    REQUIRE(restored.tool_calls->size() == 2u);
    CHECK((*restored.tool_calls)[0].id   == "call_001");
    CHECK((*restored.tool_calls)[0].name == "ReadTool");
    CHECK((*restored.tool_calls)[0].arguments["path"] == "/tmp/foo.txt");
    CHECK((*restored.tool_calls)[1].id   == "call_002");
    CHECK((*restored.tool_calls)[1].name == "GlobTool");

    REQUIRE(restored.usage.has_value());
    CHECK(restored.usage->prompt_tokens     == 150);
    CHECK(restored.usage->completion_tokens == 42);
    CHECK(restored.usage->total_tokens      == 192);
    CHECK(std::abs(restored.usage->cost_usd - 0.000576) < 1e-9);
}

// =============================================================================
// 8. to_json / from_json round-trip — tool result message
// =============================================================================

TEST_CASE("to_json / from_json round-trip: tool result message") {
    Message original;
    original.role         = Role::Tool;
    original.content      = "file contents here";
    original.tool_call_id = std::string("call_001");
    original.tool_name    = std::string("ReadTool");
    original.is_error     = false;

    Json j = to_json(original);
    CHECK(j["role"].get<std::string>() == "tool");
    CHECK(j["tool_call_id"].get<std::string>() == "call_001");
    CHECK(j["tool_name"].get<std::string>()    == "ReadTool");
    CHECK(j["is_error"].get<bool>()            == false);

    Message restored = from_json(j);
    CHECK(restored.role    == Role::Tool);
    CHECK(restored.content == "file contents here");
    REQUIRE(restored.tool_call_id.has_value());
    CHECK(*restored.tool_call_id == "call_001");
    REQUIRE(restored.tool_name.has_value());
    CHECK(*restored.tool_name == "ReadTool");
    REQUIRE(restored.is_error.has_value());
    CHECK(*restored.is_error == false);
}

// =============================================================================
// 8b. Tool result with is_error = true
// =============================================================================

TEST_CASE("to_json / from_json round-trip: tool result with is_error=true") {
    Message original;
    original.role         = Role::Tool;
    original.content      = "Error: file not found";
    original.tool_call_id = std::string("call_err");
    original.tool_name    = std::string("ReadTool");
    original.is_error     = true;

    Message restored = from_json(to_json(original));
    REQUIRE(restored.is_error.has_value());
    CHECK(*restored.is_error == true);
    CHECK(restored.content == "Error: file not found");
}

// =============================================================================
// 9. to_json / from_json round-trip — system message
// =============================================================================

TEST_CASE("to_json / from_json round-trip: system message") {
    Message original;
    original.role    = Role::System;
    original.content = "You are a helpful assistant.";

    Message restored = from_json(to_json(original));
    CHECK(restored.role    == Role::System);
    CHECK(restored.content == "You are a helpful assistant.");
    CHECK(restored.id      == original.id);
    CHECK_FALSE(restored.tool_calls.has_value());
}

// =============================================================================
// 10. text() helper
// =============================================================================

TEST_CASE("text() returns a string_view over content") {
    Message m;
    m.content = "some content here";
    std::string_view sv = m.text();
    CHECK(sv == "some content here");
    CHECK(sv.data() == m.content.data());  // same underlying storage
}

TEST_CASE("text() on empty content returns empty string_view") {
    Message m;
    m.content = "";
    CHECK(m.text().empty());
}

// =============================================================================
// 11. is_tool_call()
// =============================================================================

TEST_CASE("is_tool_call() true: assistant role + non-empty tool_calls") {
    Message m;
    m.role = Role::Assistant;
    ToolCall tc;
    tc.id   = "call_x";
    tc.name = "Bash";
    tc.arguments = {{"cmd", "ls"}};
    m.tool_calls = std::vector<ToolCall>{tc};
    CHECK(m.is_tool_call());
}

TEST_CASE("is_tool_call() false: user role even with tool_calls field set") {
    Message m;
    m.role = Role::User;
    ToolCall tc;
    tc.id   = "call_x";
    tc.name = "Bash";
    m.tool_calls = std::vector<ToolCall>{tc};
    CHECK_FALSE(m.is_tool_call());
}

TEST_CASE("is_tool_call() false: assistant role but no tool_calls") {
    Message m;
    m.role = Role::Assistant;
    CHECK_FALSE(m.is_tool_call());
}

TEST_CASE("is_tool_call() false: assistant role with empty tool_calls vector") {
    Message m;
    m.role       = Role::Assistant;
    m.tool_calls = std::vector<ToolCall>{};
    CHECK_FALSE(m.is_tool_call());
}

// =============================================================================
// 12. is_tool_result()
// =============================================================================

TEST_CASE("is_tool_result() true only for Role::Tool") {
    {
        Message m;
        m.role = Role::Tool;
        CHECK(m.is_tool_result());
    }
    {
        Message m;
        m.role = Role::User;
        CHECK_FALSE(m.is_tool_result());
    }
    {
        Message m;
        m.role = Role::Assistant;
        CHECK_FALSE(m.is_tool_result());
    }
    {
        Message m;
        m.role = Role::System;
        CHECK_FALSE(m.is_tool_result());
    }
}

// =============================================================================
// 13. from_json throws on missing 'id'
// =============================================================================

TEST_CASE("from_json throws std::invalid_argument when 'id' is missing") {
    Json j;
    j["role"]    = "user";
    j["content"] = "hello";
    j["ts"]      = 0;
    CHECK_THROWS_AS(from_json(j), std::invalid_argument);
}

// =============================================================================
// 14. from_json throws on missing 'role'
// =============================================================================

TEST_CASE("from_json throws std::invalid_argument when 'role' is missing") {
    Json j;
    j["id"]      = "f47ac10b-58cc-4372-a567-0e02b2c3d479";
    j["content"] = "hello";
    j["ts"]      = 0;
    CHECK_THROWS_AS(from_json(j), std::invalid_argument);
}

// =============================================================================
// 15. from_json throws on unknown role string
// =============================================================================

TEST_CASE("from_json throws std::invalid_argument for unknown role string") {
    Json j;
    j["id"]      = "f47ac10b-58cc-4372-a567-0e02b2c3d479";
    j["role"]    = "banana";
    j["content"] = "hello";
    j["ts"]      = 0;
    CHECK_THROWS_AS(from_json(j), std::invalid_argument);
}

// =============================================================================
// 16. Timestamp round-trip — millisecond precision
// =============================================================================

TEST_CASE("Timestamp round-trips with millisecond precision") {
    Message original;
    original.role    = Role::User;
    original.content = "ts test";

    // Truncate to millisecond to match storage precision
    auto ms_since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(
        original.ts.time_since_epoch());
    auto truncated_ts = std::chrono::system_clock::time_point(ms_since_epoch);

    Message restored = from_json(to_json(original));
    CHECK(restored.ts == truncated_ts);
}

// =============================================================================
// 17. ToolCall arguments round-trip as arbitrary JSON objects
// =============================================================================

TEST_CASE("ToolCall arguments round-trip: nested JSON object preserved") {
    Message original;
    original.role    = Role::Assistant;
    original.content = "";

    ToolCall tc;
    tc.id   = "call_nested";
    tc.name = "EditTool";
    tc.arguments = {
        {"path", "/src/main.cpp"},
        {"edits", Json::array({
            {{"line", 10}, {"text", "// comment"}},
            {{"line", 20}, {"text", "int x = 42;"}}
        })},
        {"create_if_missing", false}
    };
    original.tool_calls = std::vector<ToolCall>{tc};

    Message restored = from_json(to_json(original));
    REQUIRE(restored.tool_calls.has_value());
    REQUIRE(restored.tool_calls->size() == 1u);

    const auto& rtc = (*restored.tool_calls)[0];
    CHECK(rtc.id   == "call_nested");
    CHECK(rtc.name == "EditTool");
    CHECK(rtc.arguments["path"]                 == "/src/main.cpp");
    CHECK(rtc.arguments["edits"].is_array());
    CHECK(rtc.arguments["edits"].size()         == 2u);
    CHECK(rtc.arguments["edits"][0]["line"]     == 10);
    CHECK(rtc.arguments["edits"][1]["text"]     == "int x = 42;");
    CHECK(rtc.arguments["create_if_missing"]    == false);
}

// =============================================================================
// 19. Empty optional tool_calls not serialised
// =============================================================================

TEST_CASE("to_json does not include 'tool_calls' key when field is nullopt") {
    Message m;
    m.role    = Role::Assistant;
    m.content = "no tools";
    // tool_calls not set (nullopt)
    Json j = to_json(m);
    CHECK_FALSE(j.contains("tool_calls"));
}

// =============================================================================
// 20. Usage fields round-trip with floating-point fidelity
// =============================================================================

TEST_CASE("UsageDelta cost_usd round-trips with sufficient precision") {
    Message original;
    original.role    = Role::Assistant;
    original.content = "";

    UsageDelta ud;
    ud.prompt_tokens     = 4096;
    ud.completion_tokens = 512;
    ud.total_tokens      = 4608;
    ud.cost_usd          = 0.01382400;
    original.usage = ud;

    Message restored = from_json(to_json(original));
    REQUIRE(restored.usage.has_value());
    CHECK(restored.usage->prompt_tokens     == 4096);
    CHECK(restored.usage->completion_tokens == 512);
    CHECK(restored.usage->total_tokens      == 4608);
    CHECK(std::abs(restored.usage->cost_usd - 0.01382400) < 1e-9);
}
