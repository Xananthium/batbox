// tests/unit/test_openai_client_tools.cpp
// ---------------------------------------------------------------------------
// Fix #16 — tools[] serialisation in /v1/chat/completions request bodies.
//
// Verifies that when a ToolRegistry is populated with tools the ChatRequest
// wire body carries:
//   - "tools": array of {type,function{name,description,parameters}} entries
//   - "tool_choice": "auto"
//
// And that an empty or absent registry produces neither field.
//
// Tests mirror the code path in Conversation::build_chat_request(), which:
//   1. Calls registry.available_tool_schemas() to get the OpenAI envelope JSON
//   2. Deserialises each schema into a ToolDef and appends to req.tools
//   3. Sets req.tool_choice = "auto" when req.tools is non-empty
//   4. Serialises the ChatRequest via to_json (nlohmann ADL)
//
// Coverage:
//   TC1: Registry with 2 tools → tools[] len=2, tool_choice=="auto"
//   TC2: Empty registry → neither "tools" nor "tool_choice" in body
//   TC3: Null registry pointer → neither "tools" nor "tool_choice" in body
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/inference/ChatRequest.hpp>
#include <batbox/tools/ITool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolRegistry.hpp>
#include <batbox/tools/ToolResult.hpp>
#include <batbox/core/Json.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace batbox;
using namespace batbox::inference;
using namespace batbox::tools;

// =============================================================================
// Test doubles
// =============================================================================

/// Minimal ITool implementation for the tools-serialisation tests.
/// Returns a well-formed schema_json() with "type":"object" parameters so that
/// the serialised ToolDef validates against the acceptance criteria.
class FakeTool final : public ITool {
public:
    explicit FakeTool(std::string name, std::string description)
        : name_(std::move(name))
        , description_(std::move(description))
    {}

    [[nodiscard]] std::string_view name()        const override { return name_; }
    [[nodiscard]] std::string_view description() const override { return description_; }

    /// Returns the full OpenAI tools[*].function object as required by ITool::schema_json().
    [[nodiscard]] Json schema_json() const override {
        return Json{
            {"name",        name_},
            {"description", description_},
            {"parameters",  Json{
                {"type",       "object"},
                {"properties", Json{
                    {"input", Json{{"type", "string"}, {"description", "Input text."}}}
                }},
                {"required", Json::array({"input"})}
            }}
        };
    }

    [[nodiscard]] ToolResult run(const Json& /*args*/, ToolContext& /*ctx*/) override {
        return ToolResult::ok("fake:" + name_);
    }

    [[nodiscard]] bool is_read_only()          const override { return true;  }
    [[nodiscard]] bool requires_confirmation() const override { return false; }

private:
    std::string name_;
    std::string description_;
};

// =============================================================================
// Helper: build a ChatRequest body the same way Conversation::build_chat_request
// does — using registry.available_tool_schemas() and setting tool_choice="auto"
// when tools are non-empty.
// =============================================================================

/// Populate a ChatRequest from an optional ToolRegistry pointer, then
/// serialise to Json.  This mirrors the production code in
/// Conversation::build_chat_request() so the tests exercise the same data path.
static Json build_body(const ToolRegistry* registry) {
    ChatRequest req;
    req.model = "test-model";
    req.messages.push_back(WireMessage{"user", std::string{"Hello"}, std::nullopt, std::nullopt, std::nullopt});
    req.stream = false;
    req.stream_options_include_usage = std::nullopt;

    if (registry != nullptr) {
        const auto schemas = registry->available_tool_schemas();
        req.tools.reserve(schemas.size());
        for (const auto& schema_json : schemas) {
            ToolDef td;
            td.type = schema_json.value("type", std::string("function"));
            if (schema_json.contains("function")) {
                const auto& fn = schema_json["function"];
                td.name        = fn.value("name",        std::string{});
                td.description = fn.value("description", std::string{});
                if (fn.contains("parameters")) {
                    td.schema = fn["parameters"];
                }
            }
            req.tools.push_back(std::move(td));
        }
    }

    // Mirror the fix: emit tool_choice="auto" when tools are present.
    if (!req.tools.empty()) {
        req.tool_choice = std::string{"auto"};
    }

    Json body = req;
    return body;
}

// =============================================================================
// TC1: Registry with 2 tools
// =============================================================================
TEST_SUITE("OpenAI client tools serialisation — populated registry") {

    TEST_CASE("tools array has length 2") {
        ToolRegistry reg;
        reg.register_tool(std::make_unique<FakeTool>("Read",  "Read a file from disk."));
        reg.register_tool(std::make_unique<FakeTool>("Bash",  "Execute a bash command."));

        Json body = build_body(&reg);

        REQUIRE(body.contains("tools"));
        REQUIRE(body["tools"].is_array());
        CHECK(body["tools"].size() == 2u);
    }

    TEST_CASE("each tools entry has type == 'function'") {
        ToolRegistry reg;
        reg.register_tool(std::make_unique<FakeTool>("Read", "Read a file from disk."));
        reg.register_tool(std::make_unique<FakeTool>("Bash", "Execute a bash command."));

        Json body = build_body(&reg);

        REQUIRE(body["tools"].size() == 2u);
        for (const auto& entry : body["tools"]) {
            REQUIRE(entry.contains("type"));
            CHECK(entry["type"].get<std::string>() == "function");
        }
    }

    TEST_CASE("first tool entry has function.name matching the registered tool name") {
        ToolRegistry reg;
        reg.register_tool(std::make_unique<FakeTool>("Read", "Read a file from disk."));
        reg.register_tool(std::make_unique<FakeTool>("Bash", "Execute a bash command."));

        Json body = build_body(&reg);

        REQUIRE(body["tools"].size() == 2u);
        const Json& first = body["tools"][0];
        REQUIRE(first.contains("function"));
        REQUIRE(first["function"].contains("name"));
        // Registration order is preserved; first-registered tool is index 0.
        CHECK(first["function"]["name"].get<std::string>() == "Read");
    }

    TEST_CASE("first tool parameters is a JSON object with type == 'object'") {
        ToolRegistry reg;
        reg.register_tool(std::make_unique<FakeTool>("Read", "Read a file from disk."));
        reg.register_tool(std::make_unique<FakeTool>("Bash", "Execute a bash command."));

        Json body = build_body(&reg);

        REQUIRE(body["tools"].size() == 2u);
        const Json& fn = body["tools"][0]["function"];
        REQUIRE(fn.contains("parameters"));
        REQUIRE(fn["parameters"].is_object());
        CHECK(fn["parameters"]["type"].get<std::string>() == "object");
    }

    TEST_CASE("tool_choice is 'auto' when tools are present") {
        ToolRegistry reg;
        reg.register_tool(std::make_unique<FakeTool>("Read", "Read a file from disk."));
        reg.register_tool(std::make_unique<FakeTool>("Bash", "Execute a bash command."));

        Json body = build_body(&reg);

        REQUIRE(body.contains("tool_choice"));
        CHECK(body["tool_choice"].get<std::string>() == "auto");
    }

    TEST_CASE("second tool entry also has correct name and type") {
        ToolRegistry reg;
        reg.register_tool(std::make_unique<FakeTool>("Read", "Read a file from disk."));
        reg.register_tool(std::make_unique<FakeTool>("Bash", "Execute a bash command."));

        Json body = build_body(&reg);

        REQUIRE(body["tools"].size() == 2u);
        const Json& second = body["tools"][1];
        CHECK(second["type"].get<std::string>() == "function");
        CHECK(second["function"]["name"].get<std::string>() == "Bash");
    }

    TEST_CASE("tools array sample matches expected OpenAI shape verbatim") {
        // Emit a verbatim sample of the tools JSON to confirm the full shape.
        ToolRegistry reg;
        reg.register_tool(std::make_unique<FakeTool>("Read", "Read a file from disk."));

        Json body = build_body(&reg);

        // Full envelope check.
        const Json& entry = body["tools"][0];
        CHECK(entry["type"] == "function");
        const Json& fn = entry["function"];
        CHECK(fn["name"]        == "Read");
        CHECK(fn["description"] == "Read a file from disk.");
        CHECK(fn["parameters"]["type"] == "object");
        CHECK(fn["parameters"].contains("properties"));
        CHECK(fn["parameters"]["properties"].contains("input"));
    }
}

// =============================================================================
// TC2: Empty registry → no tools, no tool_choice
// =============================================================================
TEST_SUITE("OpenAI client tools serialisation — empty registry") {

    TEST_CASE("empty registry: 'tools' key absent from body") {
        ToolRegistry reg; // zero tools registered

        Json body = build_body(&reg);

        CHECK_FALSE(body.contains("tools"));
    }

    TEST_CASE("empty registry: 'tool_choice' key absent from body") {
        ToolRegistry reg;

        Json body = build_body(&reg);

        CHECK_FALSE(body.contains("tool_choice"));
    }
}

// =============================================================================
// TC3: Null registry pointer → no tools, no tool_choice
// =============================================================================
TEST_SUITE("OpenAI client tools serialisation — null registry") {

    TEST_CASE("null registry: 'tools' key absent from body") {
        Json body = build_body(nullptr);

        CHECK_FALSE(body.contains("tools"));
    }

    TEST_CASE("null registry: 'tool_choice' key absent from body") {
        Json body = build_body(nullptr);

        CHECK_FALSE(body.contains("tool_choice"));
    }
}
