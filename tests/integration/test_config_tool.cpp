// tests/integration/test_config_tool.cpp
// ---------------------------------------------------------------------------
// Integration tests for batbox::tools::ConfigTool.
//
// Acceptance criteria (from task CPP 5.23):
//   AC1 — get returns current value (redacted)
//   AC2 — set persists the value and reload picks it up
//   AC3 — list returns all fields with current values (secrets redacted)
//   AC4 — Unknown key: error
//   AC5 — Read-only key set attempt: error
//   AC6 — reload action fires ConfigReloadBus
//   AC7 — set in Plan mode is rejected
//   AC8 — reload in Plan mode is rejected
//   AC9 — api_key never appears in get/list output (always "****")
//
// Build standalone (adapt ROOT and build paths):
//   ROOT=/path/to/batbox
//   c++ -std=c++20 \
//       -I $ROOT/include \
//       -I $ROOT/build/vcpkg_installed/arm64-osx/include \
//       $ROOT/tests/integration/test_config_tool.cpp \
//       $ROOT/src/tools/ConfigTool.cpp \
//       $ROOT/src/config/Config.cpp \
//       $ROOT/src/config/ConfigReload.cpp \
//       $ROOT/src/config/EnvLoader.cpp \
//       $ROOT/src/config/SettingsLoader.cpp \
//       $ROOT/src/core/CancelToken.cpp \
//       $ROOT/src/core/Logging.cpp \
//       $ROOT/src/core/Paths.cpp \
//       $ROOT/src/core/Json.cpp \
//       $ROOT/build/vcpkg_installed/arm64-osx/lib/libfmt.a \
//       $ROOT/build/vcpkg_installed/arm64-osx/lib/libspdlog.a \
//       $ROOT/build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       -o /tmp/test_config_tool && /tmp/test_config_tool
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/config/Config.hpp>
#include <batbox/config/ConfigReload.hpp>
#include <batbox/config/EnvLoader.hpp>
#include <batbox/config/SettingsLoader.hpp>
#include <batbox/tools/ConfigTool.hpp>
#include <batbox/tools/ToolContext.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace batbox;
using namespace batbox::config;
using namespace batbox::tools;

// ============================================================================
// Test fixture helpers
// ============================================================================
namespace {

/// RAII temporary directory.
struct TempDir {
    fs::path path;

    TempDir() {
        path = fs::temp_directory_path() /
               ("batbox_test_config_tool_" +
                std::to_string(
                    std::hash<std::thread::id>{}(std::this_thread::get_id()) ^
                    static_cast<std::size_t>(
                        std::chrono::steady_clock::now()
                            .time_since_epoch()
                            .count())));
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;

    void write_env(const std::string& content) const {
        std::ofstream f(path / ".env");
        f << content;
    }
};

/// Build a minimal ToolContext with the given permission mode.
ToolContext make_ctx(permissions::PermissionMode mode =
                     permissions::PermissionMode::Default) {
    ToolContext ctx;
    ctx.mode       = mode;
    ctx.cwd        = fs::current_path();
    ctx.session_id = "test-session";
    ctx.agent_id   = "";
    return ctx;
}

/// Build a ConfigTool + Config for a given TempDir.
/// Writes a minimal .env to the dir so reload works.
struct Fixture {
    TempDir          dir;
    Config           cfg;
    std::mutex       mtx;
    ConfigTool       tool;

    Fixture()
        : cfg(Config::load_default())
        , tool(cfg, mtx, dir.path)
    {
        // Write a minimal .env so EnvLoader finds the file.
        dir.write_env("# test env\n");
    }

    /// Run the tool and return the ToolResult.
    ToolResult run(const Json& args,
                   permissions::PermissionMode mode =
                       permissions::PermissionMode::Default) {
        auto ctx = make_ctx(mode);
        return tool.run(args, ctx);
    }
};

} // anonymous namespace

// ============================================================================
// AC1 — get returns current value (redacted)
// ============================================================================
TEST_CASE("ConfigTool::get returns current value for a known key") {
    Fixture f;

    Json args = Json{{"action", "get"}, {"key", "BATBOX_DEFAULT_MODEL"}};
    const ToolResult result = f.run(args);

    REQUIRE_FALSE(result.is_error);
    // Body should contain the key and the value.
    CHECK(result.body.find("BATBOX_DEFAULT_MODEL") != std::string::npos);
    // The structured payload should have "key" and "value" fields.
    REQUIRE(result.structured_payload.has_value());
    const Json& payload = *result.structured_payload;
    CHECK(payload.contains("key"));
    CHECK(payload["key"].get<std::string>() == "BATBOX_DEFAULT_MODEL");
    CHECK(payload.contains("value"));
}

TEST_CASE("ConfigTool::get accepts dot-path short form") {
    Fixture f;

    Json args = Json{{"action", "get"}, {"key", "api.default_model"}};
    const ToolResult result = f.run(args);

    REQUIRE_FALSE(result.is_error);
    REQUIRE(result.structured_payload.has_value());
    CHECK((*result.structured_payload)["key"].get<std::string>() == "BATBOX_DEFAULT_MODEL");
}

// ============================================================================
// AC3 — list returns all fields with current values (secrets redacted)
// ============================================================================
TEST_CASE("ConfigTool::list returns all keys") {
    Fixture f;

    Json args = Json{{"action", "list"}};
    const ToolResult result = f.run(args);

    REQUIRE_FALSE(result.is_error);
    // Body should contain multiple BATBOX_* keys.
    CHECK(result.body.find("BATBOX_DEFAULT_MODEL") != std::string::npos);
    CHECK(result.body.find("BATBOX_THEME") != std::string::npos);
    CHECK(result.body.find("BATBOX_LOG_LEVEL") != std::string::npos);
}

// ============================================================================
// AC9 — api_key never appears in output (always "****")
// ============================================================================
TEST_CASE("ConfigTool::get api_key returns redacted value") {
    Fixture f;
    // Set a fake api_key directly in cfg for testing.
    f.cfg.api.api_key = "sk-secret12345";

    Json args = Json{{"action", "get"}, {"key", "BATBOX_API_KEY"}};
    const ToolResult result = f.run(args);

    REQUIRE_FALSE(result.is_error);
    // The actual secret value must NOT appear in the output.
    CHECK(result.body.find("sk-secret12345") == std::string::npos);
    // The redacted placeholder must appear.
    CHECK(result.body.find("****") != std::string::npos);
}

TEST_CASE("ConfigTool::list api_key is always redacted") {
    Fixture f;
    f.cfg.api.api_key = "sk-anothersecret";

    Json args = Json{{"action", "list"}};
    const ToolResult result = f.run(args);

    REQUIRE_FALSE(result.is_error);
    CHECK(result.body.find("sk-anothersecret") == std::string::npos);
    CHECK(result.body.find("****") != std::string::npos);
}

// ============================================================================
// AC4 — Unknown key: error
// ============================================================================
TEST_CASE("ConfigTool::get unknown key returns error") {
    Fixture f;

    Json args = Json{{"action", "get"}, {"key", "BATBOX_TOTALLY_UNKNOWN_KEY_XYZ"}};
    const ToolResult result = f.run(args);

    CHECK(result.is_error);
    CHECK(result.body.find("unknown key") != std::string::npos);
}

TEST_CASE("ConfigTool::set unknown key returns error") {
    Fixture f;

    Json args = Json{{"action", "set"}, {"key", "BATBOX_DOES_NOT_EXIST"}, {"value", "foo"}};
    const ToolResult result = f.run(args);

    CHECK(result.is_error);
    CHECK(result.body.find("unknown key") != std::string::npos);
}

// ============================================================================
// AC5 — Read-only key set attempt: error
// ============================================================================
TEST_CASE("ConfigTool::set read-only key returns error") {
    Fixture f;

    Json args = Json{{"action", "set"}, {"key", "BATBOX_API_KEY"}, {"value", "new-secret"}};
    const ToolResult result = f.run(args);

    CHECK(result.is_error);
    CHECK(result.body.find("read-only") != std::string::npos);
}

TEST_CASE("ConfigTool::set read-only key via dot-path returns error") {
    Fixture f;

    Json args = Json{{"action", "set"}, {"key", "api.api_key"}, {"value", "hacked"}};
    const ToolResult result = f.run(args);

    CHECK(result.is_error);
    CHECK(result.body.find("read-only") != std::string::npos);
}

// ============================================================================
// AC2 — set persists value and reload picks it up
// ============================================================================
TEST_CASE("ConfigTool::set BATBOX_THEME persists and reloads") {
    Fixture f;

    // Set theme to "monochrome".
    Json args = Json{{"action", "set"}, {"key", "BATBOX_THEME"}, {"value", "monochrome"}};
    const ToolResult result = f.run(args);

    REQUIRE_FALSE(result.is_error);
    CHECK(result.body.find("Set BATBOX_THEME") != std::string::npos);

    // Verify the .env file was updated.
    std::ifstream env_file(f.dir.path / ".env");
    std::string env_content((std::istreambuf_iterator<char>(env_file)),
                             std::istreambuf_iterator<char>());
    CHECK(env_content.find("BATBOX_THEME=monochrome") != std::string::npos);
}

// ============================================================================
// AC7 — set in Plan mode is rejected
// ============================================================================
TEST_CASE("ConfigTool::set in Plan mode returns error") {
    Fixture f;

    Json args = Json{{"action", "set"}, {"key", "BATBOX_THEME"}, {"value", "classic"}};
    const ToolResult result = f.run(args, permissions::PermissionMode::Plan);

    CHECK(result.is_error);
    CHECK(result.body.find("Plan mode") != std::string::npos);
}

// ============================================================================
// AC8 — reload in Plan mode is rejected
// ============================================================================
TEST_CASE("ConfigTool::reload in Plan mode returns error") {
    Fixture f;

    Json args = Json{{"action", "reload"}};
    const ToolResult result = f.run(args, permissions::PermissionMode::Plan);

    CHECK(result.is_error);
    CHECK(result.body.find("Plan mode") != std::string::npos);
}

// ============================================================================
// AC6 — reload action fires ConfigReloadBus
// ============================================================================
TEST_CASE("ConfigTool::reload fires ConfigReloadBus subscriber") {
    Fixture f;

    bool subscriber_called = false;
    auto handle = ConfigReloadBus::instance().subscribe(
        [&](const Config&, const ReloadReport&) {
            subscriber_called = true;
        });

    Json args = Json{{"action", "reload"}};
    const ToolResult result = f.run(args);

    REQUIRE_FALSE(result.is_error);
    CHECK(subscriber_called);
}

// ============================================================================
// reload action — basic success
// ============================================================================
TEST_CASE("ConfigTool::reload returns ok result") {
    Fixture f;

    Json args = Json{{"action", "reload"}};
    const ToolResult result = f.run(args);

    REQUIRE_FALSE(result.is_error);
    CHECK(result.body.find("reloaded") != std::string::npos);
    REQUIRE(result.structured_payload.has_value());
    const Json& payload = *result.structured_payload;
    CHECK(payload.contains("action"));
    CHECK(payload["action"].get<std::string>() == "reload");
    CHECK(payload.contains("changed_fields"));
    CHECK(payload.contains("is_unchanged"));
}

// ============================================================================
// get — missing key argument
// ============================================================================
TEST_CASE("ConfigTool::get missing key argument returns error") {
    Fixture f;

    Json args = Json{{"action", "get"}};
    const ToolResult result = f.run(args);

    CHECK(result.is_error);
    CHECK(result.body.find("'key'") != std::string::npos);
}

// ============================================================================
// set — missing value argument
// ============================================================================
TEST_CASE("ConfigTool::set missing value argument returns error") {
    Fixture f;

    Json args = Json{{"action", "set"}, {"key", "BATBOX_THEME"}};
    const ToolResult result = f.run(args);

    CHECK(result.is_error);
    CHECK(result.body.find("'value'") != std::string::npos);
}

// ============================================================================
// unknown action
// ============================================================================
TEST_CASE("ConfigTool::run unknown action returns error") {
    Fixture f;

    Json args = Json{{"action", "frob"}};
    const ToolResult result = f.run(args);

    CHECK(result.is_error);
    CHECK(result.body.find("frob") != std::string::npos);
}

// ============================================================================
// missing action
// ============================================================================
TEST_CASE("ConfigTool::run missing action argument returns error") {
    Fixture f;

    Json args = Json{{"key", "BATBOX_THEME"}};
    const ToolResult result = f.run(args);

    CHECK(result.is_error);
    CHECK(result.body.find("action") != std::string::npos);
}

// ============================================================================
// Tool identity
// ============================================================================
TEST_CASE("ConfigTool::name returns 'Config'") {
    Config cfg = Config::load_default();
    std::mutex mtx;
    ConfigTool tool(cfg, mtx, fs::temp_directory_path());

    CHECK(tool.name() == "Config");
}

TEST_CASE("ConfigTool is not read_only (set/reload have side effects)") {
    Config cfg = Config::load_default();
    std::mutex mtx;
    ConfigTool tool(cfg, mtx, fs::temp_directory_path());

    CHECK_FALSE(tool.is_read_only());
    CHECK(tool.requires_confirmation());
}

TEST_CASE("ConfigTool::schema_json returns valid structure") {
    Config cfg = Config::load_default();
    std::mutex mtx;
    ConfigTool tool(cfg, mtx, fs::temp_directory_path());

    const Json schema = tool.schema_json();
    REQUIRE(schema.is_object());
    CHECK(schema["name"].get<std::string>() == "Config");
    CHECK(schema.contains("description"));
    CHECK(schema.contains("parameters"));

    const Json& params = schema["parameters"];
    CHECK(params.contains("properties"));
    CHECK(params["properties"].contains("action"));
    CHECK(params["properties"].contains("key"));
    CHECK(params["properties"].contains("value"));
}
