// tests/unit/test_model_cmd_env_sourcing.cpp
//
// PEXT 2.1 regression test: ModelCmd reads models from ctx.cfg->api.models,
// NOT from std::getenv("BATBOX_MODELS") and NOT from a hardcoded fallback.
//
// Strategy
// --------
// Compile ModelCmd.cpp directly into this test executable (same pattern as
// test_commands_s1) to avoid pulling in all of batbox_commands and its
// transitive dependency chain.
//
// Each TEST_CASE constructs a fresh batbox::config::Config with specific
// api.models/api.default_model values, builds a CommandContext pointing at
// that Config, calls ModelCmd::execute(), and asserts the output.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/config/Config.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/repl/CommandContext.hpp>

#include <filesystem>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace batbox::commands;

// ============================================================================
// MockConversation — minimal ConversationHandle for testing
// ============================================================================

struct MockConversation final : ConversationHandle {
    void reset_messages() override {}
    void inject_user_message(std::string_view) override {}
    std::string last_assistant_message(std::size_t) const override { return {}; }
};

// ============================================================================
// Registration function declaration (defined in ModelCmd.cpp)
// ============================================================================

namespace batbox::commands {
    void register_model_cmd(SlashCommandRegistry& registry);
}

// ============================================================================
// TEST_CASE 1: ModelCmd lists models from cfg.api.models, not gpt-4o
// ============================================================================

TEST_CASE("ModelCmd lists models from cfg.api.models") {
    MockConversation conv;
    SlashCommandRegistry reg;
    register_model_cmd(reg);
    auto* cmd = reg.lookup("model");
    REQUIRE(cmd != nullptr);

    batbox::config::Config fake_cfg;
    fake_cfg.api.models        = {"alpha", "beta"};
    fake_cfg.api.default_model = "alpha";

    std::ostringstream out;
    std::istringstream in("\n");  // press Enter — keep current model

    CommandContext ctx{
        .output       = out,
        .input        = in,
        .conversation = conv,
        .registry     = reg,
        .cwd          = fs::temp_directory_path(),
        .config_dir   = fs::temp_directory_path() / "batbox_model_test_1",
        .cfg          = &fake_cfg,
        .cfg_mutex    = nullptr,
    };

    auto result = cmd->execute("", ctx);

    const std::string output = out.str();
    CHECK(output.find("alpha") != std::string::npos);
    CHECK(output.find("beta")  != std::string::npos);
    CHECK(output.find("gpt-4o") == std::string::npos);
    CHECK(static_cast<bool>(result));  // ok — Enter keeps current
}

// ============================================================================
// TEST_CASE 2: Empty models list returns configured-error message
// ============================================================================

TEST_CASE("ModelCmd returns error when cfg.api.models is empty") {
    MockConversation conv;
    SlashCommandRegistry reg;
    register_model_cmd(reg);
    auto* cmd = reg.lookup("model");
    REQUIRE(cmd != nullptr);

    batbox::config::Config fake_cfg;
    fake_cfg.api.models.clear();       // clear the default {"gpt-4o","gpt-4o-mini"}
    fake_cfg.api.default_model = "";

    std::ostringstream out;
    std::istringstream in("\n");

    CommandContext ctx{
        .output       = out,
        .input        = in,
        .conversation = conv,
        .registry     = reg,
        .cwd          = fs::temp_directory_path(),
        .config_dir   = fs::temp_directory_path() / "batbox_model_test_2",
        .cfg          = &fake_cfg,
        .cfg_mutex    = nullptr,
    };

    auto result = cmd->execute("", ctx);

    CHECK_FALSE(static_cast<bool>(result));
    CHECK(result.error().find("no models configured") != std::string::npos);
}

// ============================================================================
// TEST_CASE 3: nullptr cfg returns no-live-config error
// ============================================================================

TEST_CASE("ModelCmd returns error when ctx.cfg is nullptr") {
    MockConversation conv;
    SlashCommandRegistry reg;
    register_model_cmd(reg);
    auto* cmd = reg.lookup("model");
    REQUIRE(cmd != nullptr);

    std::ostringstream out;
    std::istringstream in("\n");

    CommandContext ctx{
        .output       = out,
        .input        = in,
        .conversation = conv,
        .registry     = reg,
        .cwd          = fs::temp_directory_path(),
        .config_dir   = fs::temp_directory_path() / "batbox_model_test_3",
        .cfg          = nullptr,
        .cfg_mutex    = nullptr,
    };

    auto result = cmd->execute("", ctx);
    CHECK_FALSE(static_cast<bool>(result));
    CHECK(result.error().find("no live config") != std::string::npos);
}

// ============================================================================
// TEST_CASE 4: Direct /model <name> switch succeeds from cfg list
// ============================================================================

TEST_CASE("ModelCmd direct switch uses cfg.api.models list") {
    MockConversation conv;
    SlashCommandRegistry reg;
    register_model_cmd(reg);
    auto* cmd = reg.lookup("model");
    REQUIRE(cmd != nullptr);

    batbox::config::Config fake_cfg;
    fake_cfg.api.models        = {"alpha", "beta"};
    fake_cfg.api.default_model = "beta";

    std::ostringstream out;
    std::istringstream in("");

    CommandContext ctx{
        .output       = out,
        .input        = in,
        .conversation = conv,
        .registry     = reg,
        .cwd          = fs::temp_directory_path(),
        .config_dir   = fs::temp_directory_path() / "batbox_model_test_4",
        .cfg          = &fake_cfg,
        .cfg_mutex    = nullptr,
    };

    auto result = cmd->execute("alpha", ctx);

    CHECK(static_cast<bool>(result));
    CHECK(out.str().find("Switched to model 'alpha'") != std::string::npos);
    CHECK(out.str().find("gpt-4o") == std::string::npos);
}
