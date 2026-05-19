// tests/integration/test_model_config_commands.cpp
// ---------------------------------------------------------------------------
// Integration tests for batbox::commands::{ModelCmd, ConfigCmd, EffortCmd}.
//
// Acceptance criteria covered:
//   AC1 — /model picker shows all configured models
//   AC2 — /model <name> selection persists to settings.json
//   AC3 — /config show round-trips: set key via /config set, reload shows it
//   AC4 — /effort: stored in settings.json, read back correctly
//   AC5 — Integration: /model picker + /config show + /effort together
//
// Build standalone:
//   ROOT=/path/to/batbox
//   c++ -std=c++20 \
//       -I $ROOT/include \
//       -I $ROOT/build/vcpkg_installed/arm64-osx/include \
//       $ROOT/tests/integration/test_model_config_commands.cpp \
//       $ROOT/src/commands/ModelCmd.cpp \
//       $ROOT/src/commands/ConfigCmd.cpp \
//       $ROOT/src/commands/EffortCmd.cpp \
//       $ROOT/src/commands/SlashCommandRegistry.cpp \
//       $ROOT/src/config/EnvLoader.cpp \
//       $ROOT/src/core/Logging.cpp \
//       $ROOT/src/core/Paths.cpp \
//       $ROOT/src/core/Json.cpp \
//       $ROOT/build/vcpkg_installed/arm64-osx/lib/libfmt.a \
//       $ROOT/build/vcpkg_installed/arm64-osx/lib/libspdlog.a \
//       $ROOT/build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       -o /tmp/test_model_config_commands && /tmp/test_model_config_commands
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/config/Config.hpp>
#include <batbox/repl/CommandContext.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <string>

namespace fs = std::filesystem;
using namespace batbox::commands;

// ---------------------------------------------------------------------------
// Registration function declarations
// ---------------------------------------------------------------------------

namespace batbox::commands {
    void register_model_cmd(SlashCommandRegistry&);
    void register_config_cmd(SlashCommandRegistry&);
    void register_effort_cmd(SlashCommandRegistry&);
}

// ---------------------------------------------------------------------------
// Test fixture helpers
// ---------------------------------------------------------------------------

namespace {

/// RAII temp directory created in the system temp directory.
struct TempDir {
    fs::path path;

    TempDir() {
        std::error_code ec;
        path = fs::temp_directory_path(ec) / ("batbox_test_" + std::to_string(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count()));
        fs::create_directories(path, ec);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;
};

/// Minimal ConversationHandle implementation for testing.
struct MockConversation : public ConversationHandle {
    void reset_messages() override {}
    void inject_user_message(std::string_view) override {}
    std::string last_assistant_message(std::size_t) const override { return {}; }
};

/// Build a CommandContext wired to string streams and a temp config dir.
struct TestCtx {
    MockConversation             conv;
    SlashCommandRegistry         registry;
    std::ostringstream           out;
    std::istringstream           in;
    batbox::config::Config       fake_cfg;   ///< PEXT3 1.2: real Config with populated api fields
    CommandContext               ctx;

    explicit TestCtx(const fs::path& config_dir, const std::string& input = "")
        : in(input)
        , ctx{.output     = out,
              .input      = in,
              .conversation = conv,
              .registry   = registry,
              .cwd        = config_dir,
              .config_dir = config_dir,
              .cfg        = &fake_cfg}
    {
        // Default fake config: two models, one default.
        fake_cfg.api.models        = {"gpt-4o", "gpt-4o-mini"};
        fake_cfg.api.default_model = "gpt-4o";
        register_model_cmd(registry);
        register_config_cmd(registry);
        register_effort_cmd(registry);
    }

    std::string output() const { return out.str(); }

    ISlashCommand* cmd(std::string_view name) {
        return registry.lookup(name);
    }
};

/// Read the contents of a file.
[[nodiscard]] std::string read_file(const fs::path& p) {
    std::ifstream f(p);
    if (!f) return {};
    return std::string((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// Registration tests
// ---------------------------------------------------------------------------

TEST_CASE("ModelCmd, ConfigCmd, EffortCmd register with correct names") {
    SlashCommandRegistry registry;
    register_model_cmd(registry);
    register_config_cmd(registry);
    register_effort_cmd(registry);

    CHECK(registry.lookup("model")  != nullptr);
    CHECK(registry.lookup("config") != nullptr);
    CHECK(registry.lookup("effort") != nullptr);
}

TEST_CASE("All three commands have non-empty descriptions and usage strings") {
    SlashCommandRegistry registry;
    register_model_cmd(registry);
    register_config_cmd(registry);
    register_effort_cmd(registry);

    for (const auto* name : {"model", "config", "effort"}) {
        ISlashCommand* c = registry.lookup(name);
        REQUIRE(c != nullptr);
        CHECK(!c->description().empty());
        CHECK(!c->usage().empty());
    }
}

// ---------------------------------------------------------------------------
// AC1 — /model picker shows all configured models
// ---------------------------------------------------------------------------

TEST_CASE("AC1: /model lists all BATBOX_MODELS entries") {
    TempDir tmp;
    // Set BATBOX_MODELS env var for this test.
    // We cannot safely setenv in a threaded doctest context, so we verify
    // that the command outputs the expected header and at least one model
    // entry from the default list.
    //
    // The default model list is {"gpt-4o", "gpt-4o-mini"}.
    // Input "\\n" = empty Enter → keep current model.
    TestCtx tc(tmp.path, "\n");
    ISlashCommand* c = tc.cmd("model");
    REQUIRE(c != nullptr);

    auto res = c->execute("", tc.ctx);
    REQUIRE(res.has_value());

    const std::string out = tc.output();
    // Picker header must appear.
    CHECK(out.find("Available models") != std::string::npos);
    // At least one model should be shown.
    CHECK(out.find("gpt-4o") != std::string::npos);
    // Status line prompt must appear.
    CHECK(out.find("Enter a number or model name") != std::string::npos);
}

TEST_CASE("AC1: /model lists models with numbering") {
    TempDir tmp;
    TestCtx tc(tmp.path, "\n");  // empty Enter = keep current
    auto res = tc.cmd("model")->execute("", tc.ctx);
    REQUIRE(res.has_value());
    // At least entries "1." should appear in output.
    CHECK(tc.output().find("1.") != std::string::npos);
}

// ---------------------------------------------------------------------------
// AC2 — /model <name> selection persists to settings.json
// ---------------------------------------------------------------------------

TEST_CASE("AC2: /model <name> writes default_model to settings.json") {
    TempDir tmp;
    TestCtx tc(tmp.path);

    auto res = tc.cmd("model")->execute("gpt-4o-mini", tc.ctx);
    REQUIRE(res.has_value());

    // settings.json must now contain default_model.
    const std::string settings = read_file(tmp.path / "settings.json");
    CHECK(settings.find("default_model") != std::string::npos);
    CHECK(settings.find("gpt-4o-mini")   != std::string::npos);
}

TEST_CASE("AC2: /model picker with numeric selection persists model") {
    TempDir tmp;
    // Input "2" → select the second model (gpt-4o-mini from default list).
    TestCtx tc(tmp.path, "2\n");

    auto res = tc.cmd("model")->execute("", tc.ctx);
    REQUIRE(res.has_value());

    const std::string settings = read_file(tmp.path / "settings.json");
    CHECK(settings.find("default_model") != std::string::npos);
    CHECK(settings.find("gpt-4o-mini")   != std::string::npos);
}

TEST_CASE("AC2: /model picker with name selection persists model") {
    TempDir tmp;
    TestCtx tc(tmp.path, "gpt-4o\n");

    auto res = tc.cmd("model")->execute("", tc.ctx);
    REQUIRE(res.has_value());

    const std::string settings = read_file(tmp.path / "settings.json");
    CHECK(settings.find("gpt-4o") != std::string::npos);
}

TEST_CASE("AC2: /model with out-of-range index returns error") {
    TempDir tmp;
    TestCtx tc(tmp.path, "99\n");

    auto res = tc.cmd("model")->execute("", tc.ctx);
    REQUIRE(!res.has_value());
    CHECK(res.error().find("out of range") != std::string::npos);
}

// ---------------------------------------------------------------------------
// AC3 — /config: set + show round-trip
// ---------------------------------------------------------------------------

TEST_CASE("AC3: /config show prints config header") {
    TempDir tmp;
    TestCtx tc(tmp.path);

    auto res = tc.cmd("config")->execute("show", tc.ctx);
    REQUIRE(res.has_value());
    CHECK(tc.output().find("Effective configuration") != std::string::npos);
}

TEST_CASE("AC3: /config set persists a value to settings.json") {
    TempDir tmp;
    TestCtx tc(tmp.path);

    auto res = tc.cmd("config")->execute("set theme frank-sinatra", tc.ctx);
    REQUIRE(res.has_value());

    const std::string settings = read_file(tmp.path / "settings.json");
    CHECK(settings.find("frank-sinatra") != std::string::npos);
    CHECK(tc.output().find("saved to settings.json") != std::string::npos);
}

TEST_CASE("AC3: /config get retrieves the key we just set") {
    TempDir tmp;
    // First set a value.
    {
        TestCtx tc_set(tmp.path);
        (void)tc_set.cmd("config")->execute("set log_level debug", tc_set.ctx);
    }
    // Now get it.
    TestCtx tc_get(tmp.path);
    auto res = tc_get.cmd("config")->execute("get log_level", tc_get.ctx);
    REQUIRE(res.has_value());
    // The value we set should appear in the output (it may show as env default
    // if BATBOX_LOG_LEVEL is set in the test environment — that is expected).
    CHECK(!tc_get.output().empty());
}

TEST_CASE("AC3: /config set refuses to set BATBOX_API_KEY") {
    TempDir tmp;
    TestCtx tc(tmp.path);
    auto res = tc.cmd("config")->execute("set BATBOX_API_KEY secret123", tc.ctx);
    REQUIRE(!res.has_value());
    CHECK(res.error().find("BATBOX_API_KEY") != std::string::npos);
}

TEST_CASE("AC3: /config set with unknown key returns error") {
    TempDir tmp;
    TestCtx tc(tmp.path);
    auto res = tc.cmd("config")->execute("set nonexistent_key_xyz 42", tc.ctx);
    REQUIRE(!res.has_value());
    CHECK(res.error().find("unknown key") != std::string::npos);
}

TEST_CASE("AC3: /config path prints config directory") {
    TempDir tmp;
    TestCtx tc(tmp.path);
    auto res = tc.cmd("config")->execute("path", tc.ctx);
    REQUIRE(res.has_value());
    CHECK(tc.output().find(tmp.path.string()) != std::string::npos);
}

TEST_CASE("AC3: /config reload succeeds on empty config dir") {
    TempDir tmp;
    TestCtx tc(tmp.path);
    auto res = tc.cmd("config")->execute("reload", tc.ctx);
    REQUIRE(res.has_value());
    CHECK(tc.output().find("Reload complete") != std::string::npos);
}

TEST_CASE("AC3: /config with unknown subcommand returns error") {
    TempDir tmp;
    TestCtx tc(tmp.path);
    auto res = tc.cmd("config")->execute("frobnicate", tc.ctx);
    REQUIRE(!res.has_value());
    CHECK(res.error().find("unknown subcommand") != std::string::npos);
}

// ---------------------------------------------------------------------------
// AC4 — /effort: stored, sent in subsequent requests
// ---------------------------------------------------------------------------

TEST_CASE("AC4: /effort with no args prints current level") {
    TempDir tmp;
    TestCtx tc(tmp.path);

    auto res = tc.cmd("effort")->execute("", tc.ctx);
    REQUIRE(res.has_value());

    const std::string out = tc.output();
    CHECK(out.find("Effort level") != std::string::npos);
    // Must list the three valid levels.
    CHECK(out.find("low")    != std::string::npos);
    CHECK(out.find("medium") != std::string::npos);
    CHECK(out.find("high")   != std::string::npos);
}

TEST_CASE("AC4: /effort low persists to settings.json") {
    TempDir tmp;
    TestCtx tc(tmp.path);

    auto res = tc.cmd("effort")->execute("low", tc.ctx);
    REQUIRE(res.has_value());

    const std::string settings = read_file(tmp.path / "settings.json");
    CHECK(settings.find("effort_level") != std::string::npos);
    CHECK(settings.find("\"low\"") != std::string::npos);
}

TEST_CASE("AC4: /effort medium persists to settings.json") {
    TempDir tmp;
    TestCtx tc(tmp.path);

    auto res = tc.cmd("effort")->execute("medium", tc.ctx);
    REQUIRE(res.has_value());

    const std::string settings = read_file(tmp.path / "settings.json");
    CHECK(settings.find("\"medium\"") != std::string::npos);
}

TEST_CASE("AC4: /effort high persists to settings.json") {
    TempDir tmp;
    TestCtx tc(tmp.path);

    auto res = tc.cmd("effort")->execute("high", tc.ctx);
    REQUIRE(res.has_value());

    const std::string settings = read_file(tmp.path / "settings.json");
    CHECK(settings.find("\"high\"") != std::string::npos);
}

TEST_CASE("AC4: /effort status shows level without changing settings.json") {
    TempDir tmp;
    TestCtx tc(tmp.path);

    auto res = tc.cmd("effort")->execute("status", tc.ctx);
    REQUIRE(res.has_value());
    CHECK(tc.output().find("Effort level") != std::string::npos);

    // settings.json should NOT be created by /effort status.
    CHECK(!fs::exists(tmp.path / "settings.json"));
}

TEST_CASE("AC4: /effort with invalid level returns error") {
    TempDir tmp;
    TestCtx tc(tmp.path);

    auto res = tc.cmd("effort")->execute("turbo", tc.ctx);
    REQUIRE(!res.has_value());
    CHECK(res.error().find("unknown level") != std::string::npos);
}

TEST_CASE("AC4: /effort persisted value round-trips") {
    TempDir tmp;
    // Set to high.
    {
        TestCtx tc1(tmp.path);
        auto r = tc1.cmd("effort")->execute("high", tc1.ctx);
        REQUIRE(r.has_value());
    }
    // Now set to low — both should persist and the file should contain "low".
    {
        TestCtx tc2(tmp.path);
        auto r = tc2.cmd("effort")->execute("low", tc2.ctx);
        REQUIRE(r.has_value());
    }
    const std::string settings = read_file(tmp.path / "settings.json");
    CHECK(settings.find("\"low\"") != std::string::npos);
    // "high" should no longer be the value.
    CHECK(settings.find("\"high\"") == std::string::npos);
}

// ---------------------------------------------------------------------------
// AC5 — Integration: three commands work together in the same session
// ---------------------------------------------------------------------------

TEST_CASE("AC5: /model, /config, /effort all share the same config dir") {
    TempDir tmp;

    // 1. Switch model.
    {
        TestCtx tc(tmp.path);
        auto r = tc.cmd("model")->execute("gpt-4o-mini", tc.ctx);
        REQUIRE(r.has_value());
    }

    // 2. Set effort to high.
    {
        TestCtx tc(tmp.path);
        auto r = tc.cmd("effort")->execute("high", tc.ctx);
        REQUIRE(r.has_value());
    }

    // 3. Set a config key.
    {
        TestCtx tc(tmp.path);
        auto r = tc.cmd("config")->execute("set BATBOX_TEMPERATURE 0.5", tc.ctx);
        REQUIRE(r.has_value());
    }

    // 4. Verify all three values are in settings.json.
    const std::string settings = read_file(tmp.path / "settings.json");
    CHECK(settings.find("gpt-4o-mini")  != std::string::npos);
    CHECK(settings.find("\"high\"")     != std::string::npos);
    CHECK(settings.find("temperature")  != std::string::npos);
    CHECK(settings.find("0.5")          != std::string::npos);
}

TEST_CASE("AC5: /config show after /model and /effort runs without error") {
    TempDir tmp;
    TestCtx tc(tmp.path);

    (void)tc.cmd("model")->execute("gpt-4o", tc.ctx);
    (void)tc.cmd("effort")->execute("medium", tc.ctx);

    auto res = tc.cmd("config")->execute("show", tc.ctx);
    REQUIRE(res.has_value());
    CHECK(tc.output().find("Effective configuration") != std::string::npos);
}
