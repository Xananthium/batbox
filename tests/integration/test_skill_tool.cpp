// tests/integration/test_skill_tool.cpp
// =============================================================================
// Integration tests for batbox::tools::SkillTool.
//
// Tests exercise the full pipeline: SkillLoader::scan_dir → SkillTool::run,
// verifying that:
//   - Known skills produce the correct body.
//   - $ARGS substitution works for skills that use it.
//   - Unknown skill names return ToolResult::error.
//   - allowed_tools from the skill's frontmatter appear in the structured_payload.
//   - model override from the skill's frontmatter appears in the structured_payload.
//   - Cancellation is honoured.
//   - Missing or empty "name" argument returns an error.
//   - "input" argument is optional; absent input substitutes empty string.
//
// Build standalone (use absolute source paths so __FILE__ resolves correctly):
//   ROOT=/path/to/batbox
//   c++ -std=c++20 \
//       -I $ROOT/include \
//       -I $ROOT/build/vcpkg_installed/arm64-osx/include \
//       $ROOT/tests/integration/test_skill_tool.cpp \
//       $ROOT/src/tools/SkillTool.cpp \
//       $ROOT/src/plugins/FrontmatterParser.cpp \
//       $ROOT/src/plugins/SkillLoader.cpp \
//       $ROOT/src/core/Logging.cpp \
//       $ROOT/src/core/Paths.cpp \
//       $ROOT/src/core/Json.cpp \
//       $ROOT/build/vcpkg_installed/arm64-osx/lib/libfmt.a \
//       $ROOT/build/vcpkg_installed/arm64-osx/lib/libspdlog.a \
//       $ROOT/build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       -o /tmp/test_skill_tool && /tmp/test_skill_tool
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/plugins/SkillLoader.hpp>
#include <batbox/tools/SkillTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/core/CancelToken.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace batbox;
using namespace batbox::plugins;
using namespace batbox::tools;

namespace {

// ---------------------------------------------------------------------------
// project_root() — derive the repo root from this file's compile-time path.
// __FILE__ is absolute when compiled via CMake; walk up 3 levels:
//   tests/integration/test_skill_tool.cpp → tests/integration → tests → root
// ---------------------------------------------------------------------------
[[nodiscard]] std::filesystem::path project_root() {
    namespace fs = std::filesystem;
    return fs::path(__FILE__)
               .parent_path()  // tests/integration
               .parent_path()  // tests
               .parent_path(); // project root
}

// ---------------------------------------------------------------------------
// TempSkillDir — RAII helper: creates a temp dir, populates .md files,
// removes everything on destruction.  (Mirrors the helper in test_skill_loader.)
// ---------------------------------------------------------------------------
struct TempSkillDir {
    std::filesystem::path path;

    explicit TempSkillDir() {
        namespace fs = std::filesystem;
        path = fs::temp_directory_path() / ("batbox_test_skilltool_" +
               std::to_string(std::hash<std::string>{}(__FILE__)));
        fs::create_directories(path);
    }

    ~TempSkillDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    void write(const std::string& filename, const std::string& content) {
        std::ofstream f(path / filename);
        f << content;
    }

    TempSkillDir(const TempSkillDir&) = delete;
    TempSkillDir& operator=(const TempSkillDir&) = delete;
};

// ---------------------------------------------------------------------------
// make_args — convenience helper to build a Json args object.
// ---------------------------------------------------------------------------
[[nodiscard]] Json make_args(const std::string& name,
                              const std::string* input = nullptr) {
    Json j = Json::object();
    j["name"] = name;
    if (input) j["input"] = *input;
    return j;
}

// ---------------------------------------------------------------------------
// default_ctx — build a ToolContext with sensible defaults for tests.
// ---------------------------------------------------------------------------
[[nodiscard]] ToolContext default_ctx() {
    ToolContext ctx;
    ctx.cwd = std::filesystem::temp_directory_path();
    return ctx;
}

} // anonymous namespace

// =============================================================================
// TEST SUITE
// =============================================================================

TEST_SUITE("SkillTool") {

// ---------------------------------------------------------------------------
// AC1: Unknown skill name → ToolResult::error containing the name
// ---------------------------------------------------------------------------
TEST_CASE("unknown skill name returns error with skill name in body") {
    SkillLoader loader;
    SkillTool   tool(loader);
    ToolContext  ctx = default_ctx();

    auto result = tool.run(make_args("no-such-skill"), ctx);
    CHECK(result.is_error);
    CHECK(result.body.find("no-such-skill") != std::string::npos);
}

// ---------------------------------------------------------------------------
// AC2: Known skill — body returned as-is (no $ARGS in body, no input)
// ---------------------------------------------------------------------------
TEST_CASE("known skill returns prompt body in ToolResult::ok") {
    TempSkillDir tmp;
    tmp.write("greet.md",
        "---\nname: greet\ndescription: greeting\n---\nHello, world!\n");

    SkillLoader loader;
    loader.scan_dir(tmp.path, "user-dir");

    SkillTool  tool(loader);
    ToolContext ctx = default_ctx();

    auto result = tool.run(make_args("greet"), ctx);
    CHECK(!result.is_error);
    CHECK(result.body == "Hello, world!\n");
}

// ---------------------------------------------------------------------------
// AC3: $ARGS substitution — input replaces $ARGS in body
// ---------------------------------------------------------------------------
TEST_CASE("$ARGS in body is replaced with input argument") {
    TempSkillDir tmp;
    tmp.write("greet_args.md",
        "---\nname: greet-args\ndescription: greeting with args\n---\n"
        "Hello, $ARGS!\n");

    SkillLoader loader;
    loader.scan_dir(tmp.path, "user-dir");

    SkillTool  tool(loader);
    ToolContext ctx = default_ctx();

    const std::string input_val = "Alice";
    auto result = tool.run(make_args("greet-args", &input_val), ctx);
    CHECK(!result.is_error);
    CHECK(result.body == "Hello, Alice!\n");
}

// ---------------------------------------------------------------------------
// AC4: $ARGS substitution — absent input replaces $ARGS with empty string
// ---------------------------------------------------------------------------
TEST_CASE("absent input substitutes empty string for $ARGS") {
    TempSkillDir tmp;
    tmp.write("needs_args.md",
        "---\nname: needs-args\ndescription: needs args\n---\n"
        "Value: [$ARGS]\n");

    SkillLoader loader;
    loader.scan_dir(tmp.path, "user-dir");

    SkillTool  tool(loader);
    ToolContext ctx = default_ctx();

    // No "input" key in args.
    auto result = tool.run(make_args("needs-args"), ctx);
    CHECK(!result.is_error);
    CHECK(result.body == "Value: []\n");
}

// ---------------------------------------------------------------------------
// AC5: Multiple $ARGS occurrences — all are replaced
// ---------------------------------------------------------------------------
TEST_CASE("all $ARGS occurrences are replaced") {
    TempSkillDir tmp;
    tmp.write("multi.md",
        "---\nname: multi-args\ndescription: multi\n---\n"
        "$ARGS plus $ARGS again\n");

    SkillLoader loader;
    loader.scan_dir(tmp.path, "user-dir");

    SkillTool  tool(loader);
    ToolContext ctx = default_ctx();

    const std::string input_val = "X";
    auto result = tool.run(make_args("multi-args", &input_val), ctx);
    CHECK(!result.is_error);
    CHECK(result.body == "X plus X again\n");
}

// ---------------------------------------------------------------------------
// AC6: allowed_tools in structured_payload when skill declares tool list
// ---------------------------------------------------------------------------
TEST_CASE("allowed_tools from skill frontmatter appears in structured_payload") {
    TempSkillDir tmp;
    tmp.write("restricted.md",
        "---\nname: restricted\ndescription: restricted tools\n"
        "allowed_tools: [Read, Bash]\n---\nBody\n");

    SkillLoader loader;
    loader.scan_dir(tmp.path, "user-dir");

    SkillTool  tool(loader);
    ToolContext ctx = default_ctx();

    auto result = tool.run(make_args("restricted"), ctx);
    REQUIRE(!result.is_error);
    REQUIRE(result.structured_payload.has_value());

    const Json& payload = *result.structured_payload;
    REQUIRE(payload.contains("allowed_tools"));
    REQUIRE(payload["allowed_tools"].is_array());

    std::vector<std::string> tools_got;
    for (const auto& t : payload["allowed_tools"]) {
        REQUIRE(t.is_string());
        tools_got.push_back(t.get<std::string>());
    }
    CHECK(std::find(tools_got.begin(), tools_got.end(), "Read") != tools_got.end());
    CHECK(std::find(tools_got.begin(), tools_got.end(), "Bash") != tools_got.end());
}

// ---------------------------------------------------------------------------
// AC7: allowed_tools is null in payload when skill has no tool list
// ---------------------------------------------------------------------------
TEST_CASE("allowed_tools is null in payload when skill has no tool list") {
    TempSkillDir tmp;
    tmp.write("open.md",
        "---\nname: open-skill\ndescription: unrestricted\n---\nBody\n");

    SkillLoader loader;
    loader.scan_dir(tmp.path, "user-dir");

    SkillTool  tool(loader);
    ToolContext ctx = default_ctx();

    auto result = tool.run(make_args("open-skill"), ctx);
    REQUIRE(!result.is_error);
    REQUIRE(result.structured_payload.has_value());

    const Json& payload = *result.structured_payload;
    REQUIRE(payload.contains("allowed_tools"));
    CHECK(payload["allowed_tools"].is_null());
}

// ---------------------------------------------------------------------------
// AC8: model in structured_payload when skill declares model override
// ---------------------------------------------------------------------------
TEST_CASE("model override from skill frontmatter appears in structured_payload") {
    TempSkillDir tmp;
    tmp.write("modeled.md",
        "---\nname: modeled\ndescription: with model\n"
        "model: claude-opus-4-5\n---\nBody\n");

    SkillLoader loader;
    loader.scan_dir(tmp.path, "user-dir");

    SkillTool  tool(loader);
    ToolContext ctx = default_ctx();

    auto result = tool.run(make_args("modeled"), ctx);
    REQUIRE(!result.is_error);
    REQUIRE(result.structured_payload.has_value());

    const Json& payload = *result.structured_payload;
    REQUIRE(payload.contains("model"));
    REQUIRE(payload["model"].is_string());
    CHECK(payload["model"].get<std::string>() == "claude-opus-4-5");
}

// ---------------------------------------------------------------------------
// AC9: model is null in payload when skill has no model override
// ---------------------------------------------------------------------------
TEST_CASE("model is null in payload when skill has no model override") {
    TempSkillDir tmp;
    tmp.write("no_model.md",
        "---\nname: no-model\ndescription: no model override\n---\nBody\n");

    SkillLoader loader;
    loader.scan_dir(tmp.path, "user-dir");

    SkillTool  tool(loader);
    ToolContext ctx = default_ctx();

    auto result = tool.run(make_args("no-model"), ctx);
    REQUIRE(!result.is_error);
    REQUIRE(result.structured_payload.has_value());

    const Json& payload = *result.structured_payload;
    REQUIRE(payload.contains("model"));
    CHECK(payload["model"].is_null());
}

// ---------------------------------------------------------------------------
// AC10: structured_payload always contains skill_name
// ---------------------------------------------------------------------------
TEST_CASE("structured_payload contains skill_name field") {
    TempSkillDir tmp;
    tmp.write("named.md",
        "---\nname: my-skill\ndescription: test\n---\nBody\n");

    SkillLoader loader;
    loader.scan_dir(tmp.path, "user-dir");

    SkillTool  tool(loader);
    ToolContext ctx = default_ctx();

    auto result = tool.run(make_args("my-skill"), ctx);
    REQUIRE(!result.is_error);
    REQUIRE(result.structured_payload.has_value());

    const Json& payload = *result.structured_payload;
    REQUIRE(payload.contains("skill_name"));
    CHECK(payload["skill_name"].get<std::string>() == "my-skill");
}

// ---------------------------------------------------------------------------
// AC11: Missing "name" argument → error
// ---------------------------------------------------------------------------
TEST_CASE("missing name argument returns error") {
    SkillLoader loader;
    SkillTool   tool(loader);
    ToolContext  ctx = default_ctx();

    Json args = Json::object(); // no "name" key
    auto result = tool.run(args, ctx);
    CHECK(result.is_error);
    CHECK(result.body.find("name") != std::string::npos);
}

// ---------------------------------------------------------------------------
// AC12: Empty "name" string → error
// ---------------------------------------------------------------------------
TEST_CASE("empty name string returns error") {
    SkillLoader loader;
    SkillTool   tool(loader);
    ToolContext  ctx = default_ctx();

    Json args = Json::object();
    args["name"] = "";
    auto result = tool.run(args, ctx);
    CHECK(result.is_error);
}

// ---------------------------------------------------------------------------
// AC13: Cancellation is honoured before execution
// ---------------------------------------------------------------------------
TEST_CASE("cancelled context returns error immediately") {
    TempSkillDir tmp;
    tmp.write("greet.md",
        "---\nname: greet\ndescription: greeting\n---\nHello\n");

    SkillLoader loader;
    loader.scan_dir(tmp.path, "user-dir");

    SkillTool  tool(loader);
    ToolContext ctx = default_ctx();
    auto [src, tok] = CancelToken::make_root();
    src.request_stop();
    ctx.cancel_token = std::move(tok);

    auto result = tool.run(make_args("greet"), ctx);
    CHECK(result.is_error);
    CHECK(result.body == "cancelled");
}

// ---------------------------------------------------------------------------
// AC14: Integration against real fixture skills (real_world_remember.md)
// ---------------------------------------------------------------------------
TEST_CASE("real fixture skill real_world_remember loads and runs correctly") {
    namespace fs = std::filesystem;
    const fs::path fixtures = project_root() / "tests" / "fixtures" / "skills";
    REQUIRE(fs::exists(fixtures));

    SkillLoader loader;
    loader.scan_dir(fixtures, "user-dir");

    const Skill* s = loader.find("remember");
    REQUIRE(s != nullptr);

    SkillTool  tool(loader);
    ToolContext ctx = default_ctx();

    auto result = tool.run(make_args("remember"), ctx);
    REQUIRE(!result.is_error);
    // Body must be non-empty.
    CHECK(!result.body.empty());

    // Payload: allowed_tools must be a non-null array (remember.md has them).
    REQUIRE(result.structured_payload.has_value());
    const Json& payload = *result.structured_payload;
    CHECK(payload["skill_name"].get<std::string>() == "remember");
    CHECK(payload["allowed_tools"].is_array());
    CHECK(!payload["allowed_tools"].empty());

    // model override (claude-opus-4-5) must be set.
    REQUIRE(payload["model"].is_string());
    CHECK(payload["model"].get<std::string>() == "claude-opus-4-5");
}

// ---------------------------------------------------------------------------
// AC15: Integration against real fixture skill real_world_debug.md
// ---------------------------------------------------------------------------
TEST_CASE("real fixture skill real_world_debug loads and runs correctly") {
    namespace fs = std::filesystem;
    const fs::path fixtures = project_root() / "tests" / "fixtures" / "skills";
    REQUIRE(fs::exists(fixtures));

    SkillLoader loader;
    loader.scan_dir(fixtures, "user-dir");

    SkillTool  tool(loader);
    ToolContext ctx = default_ctx();

    const std::string input_val = "failing test suite";
    auto result = tool.run(make_args("debug", &input_val), ctx);
    REQUIRE(!result.is_error);
    CHECK(!result.body.empty());

    REQUIRE(result.structured_payload.has_value());
    const Json& payload = *result.structured_payload;
    CHECK(payload["skill_name"].get<std::string>() == "debug");
    // debug.md has 6 allowed_tools.
    CHECK(payload["allowed_tools"].is_array());
    CHECK(payload["allowed_tools"].size() == 6);
}

// ---------------------------------------------------------------------------
// AC16: name() returns "Skill"
// ---------------------------------------------------------------------------
TEST_CASE("tool name() returns Skill") {
    SkillLoader loader;
    SkillTool   tool(loader);
    CHECK(tool.name() == "Skill");
}

// ---------------------------------------------------------------------------
// AC17: is_read_only() returns false, requires_confirmation() returns false
// ---------------------------------------------------------------------------
TEST_CASE("permission gate hooks: not read-only, no confirmation required") {
    SkillLoader loader;
    SkillTool   tool(loader);
    CHECK(!tool.is_read_only());
    CHECK(!tool.requires_confirmation());
}

// ---------------------------------------------------------------------------
// AC18: schema_json() has expected structure
// ---------------------------------------------------------------------------
TEST_CASE("schema_json() has required name, description, parameters shape") {
    SkillLoader loader;
    SkillTool   tool(loader);
    const Json schema = tool.schema_json();

    REQUIRE(schema.contains("name"));
    CHECK(schema["name"].get<std::string>() == "Skill");

    REQUIRE(schema.contains("description"));
    CHECK(!schema["description"].get<std::string>().empty());

    REQUIRE(schema.contains("parameters"));
    const Json& params = schema["parameters"];
    REQUIRE(params.contains("properties"));
    CHECK(params["properties"].contains("name"));
    CHECK(params["properties"].contains("input"));

    REQUIRE(params.contains("required"));
    REQUIRE(params["required"].is_array());
    bool name_required = false;
    for (const auto& r : params["required"]) {
        if (r.is_string() && r.get<std::string>() == "name") {
            name_required = true;
        }
    }
    CHECK(name_required);
}

} // TEST_SUITE("SkillTool")
