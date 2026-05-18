// tests/unit/test_model_cmd_live_mutation.cpp
//
// PEXT 2.2 regression test: ModelCmd mutates ctx.cfg->api.default_model under
// cfg_mutex after a successful persist_model(), so the new model takes effect
// immediately in the running session without requiring a restart.
//
// Strategy
// --------
// Compile ModelCmd.cpp directly into this test executable (same pattern as
// test_model_cmd_env_sourcing) to avoid pulling in all of batbox_commands and
// its transitive dependency chain.
//
// Each TEST_CASE constructs a batbox::config::Config with specific fields,
// builds a CommandContext with cfg = &fake_cfg and cfg_mutex = &mtx, calls
// ModelCmd::execute(), and asserts that fake_cfg.api.default_model reflects
// the new model immediately after the call — no restart required.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/config/Config.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/repl/CommandContext.hpp>

#include <filesystem>
#include <mutex>
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
// TEST_CASE 1: Direct /model <name> switch mutates cfg->api.default_model
// ============================================================================

TEST_CASE("ModelCmd direct switch mutates cfg->api.default_model immediately") {
    MockConversation conv;
    SlashCommandRegistry reg;
    register_model_cmd(reg);
    auto* cmd = reg.lookup("model");
    REQUIRE(cmd != nullptr);

    batbox::config::Config fake_cfg;
    fake_cfg.api.models        = {"m1", "m2"};
    fake_cfg.api.default_model = "m1";

    std::mutex mtx;
    std::ostringstream out;
    std::istringstream in("");

    CommandContext ctx{
        .output       = out,
        .input        = in,
        .conversation = conv,
        .registry     = reg,
        .cwd          = fs::temp_directory_path(),
        .config_dir   = fs::temp_directory_path() / "batbox_pext22_test_1",
        .cfg          = &fake_cfg,
        .cfg_mutex    = &mtx,
    };

    auto result = cmd->execute("m2", ctx);

    CHECK(static_cast<bool>(result));
    // PEXT 2.2: the live Config field must reflect the new model without restart.
    CHECK(fake_cfg.api.default_model == "m2");
    CHECK(out.str().find("Switched to model 'm2'") != std::string::npos);
}

// ============================================================================
// TEST_CASE 2: Interactive numeric selection mutates cfg->api.default_model
// ============================================================================

TEST_CASE("ModelCmd interactive numeric pick mutates cfg->api.default_model immediately") {
    MockConversation conv;
    SlashCommandRegistry reg;
    register_model_cmd(reg);
    auto* cmd = reg.lookup("model");
    REQUIRE(cmd != nullptr);

    batbox::config::Config fake_cfg;
    fake_cfg.api.models        = {"alpha", "beta"};
    fake_cfg.api.default_model = "alpha";

    std::mutex mtx;
    std::ostringstream out;
    // Input "2" selects "beta" (1-based index).
    std::istringstream in("2\n");

    CommandContext ctx{
        .output       = out,
        .input        = in,
        .conversation = conv,
        .registry     = reg,
        .cwd          = fs::temp_directory_path(),
        .config_dir   = fs::temp_directory_path() / "batbox_pext22_test_2",
        .cfg          = &fake_cfg,
        .cfg_mutex    = &mtx,
    };

    auto result = cmd->execute("", ctx);

    CHECK(static_cast<bool>(result));
    // PEXT 2.2: numeric interactive pick must also mutate the running Config.
    CHECK(fake_cfg.api.default_model == "beta");
    CHECK(out.str().find("Switched to model 'beta'") != std::string::npos);
}

// ============================================================================
// TEST_CASE 3: Interactive name selection mutates cfg->api.default_model
// ============================================================================

TEST_CASE("ModelCmd interactive name pick mutates cfg->api.default_model immediately") {
    MockConversation conv;
    SlashCommandRegistry reg;
    register_model_cmd(reg);
    auto* cmd = reg.lookup("model");
    REQUIRE(cmd != nullptr);

    batbox::config::Config fake_cfg;
    fake_cfg.api.models        = {"alpha", "beta"};
    fake_cfg.api.default_model = "alpha";

    std::mutex mtx;
    std::ostringstream out;
    // Input "beta" selects by exact name.
    std::istringstream in("beta\n");

    CommandContext ctx{
        .output       = out,
        .input        = in,
        .conversation = conv,
        .registry     = reg,
        .cwd          = fs::temp_directory_path(),
        .config_dir   = fs::temp_directory_path() / "batbox_pext22_test_3",
        .cfg          = &fake_cfg,
        .cfg_mutex    = &mtx,
    };

    auto result = cmd->execute("", ctx);

    CHECK(static_cast<bool>(result));
    // PEXT 2.2: name-based interactive pick must also mutate the running Config.
    CHECK(fake_cfg.api.default_model == "beta");
    CHECK(out.str().find("Switched to model 'beta'") != std::string::npos);
}

// ============================================================================
// TEST_CASE 4: nullptr cfg_mutex falls back gracefully (no crash, result ok)
// ============================================================================

TEST_CASE("ModelCmd with nullptr cfg_mutex does not crash") {
    MockConversation conv;
    SlashCommandRegistry reg;
    register_model_cmd(reg);
    auto* cmd = reg.lookup("model");
    REQUIRE(cmd != nullptr);

    batbox::config::Config fake_cfg;
    fake_cfg.api.models        = {"m1", "m2"};
    fake_cfg.api.default_model = "m1";

    std::ostringstream out;
    std::istringstream in("");

    CommandContext ctx{
        .output       = out,
        .input        = in,
        .conversation = conv,
        .registry     = reg,
        .cwd          = fs::temp_directory_path(),
        .config_dir   = fs::temp_directory_path() / "batbox_pext22_test_4",
        .cfg          = &fake_cfg,
        .cfg_mutex    = nullptr,  // no mutex — mutation is silently skipped
    };

    // Must not crash or throw; result must be ok.
    auto result = cmd->execute("m2", ctx);
    CHECK(static_cast<bool>(result));
    CHECK(out.str().find("Switched to model 'm2'") != std::string::npos);
}

// ============================================================================
// TEST_CASE 5: Out-of-list model still mutates cfg->api.default_model
// ============================================================================

TEST_CASE("ModelCmd out-of-list direct switch mutates cfg->api.default_model") {
    MockConversation conv;
    SlashCommandRegistry reg;
    register_model_cmd(reg);
    auto* cmd = reg.lookup("model");
    REQUIRE(cmd != nullptr);

    batbox::config::Config fake_cfg;
    fake_cfg.api.models        = {"alpha"};
    fake_cfg.api.default_model = "alpha";

    std::mutex mtx;
    std::ostringstream out;
    std::istringstream in("");

    CommandContext ctx{
        .output       = out,
        .input        = in,
        .conversation = conv,
        .registry     = reg,
        .cwd          = fs::temp_directory_path(),
        .config_dir   = fs::temp_directory_path() / "batbox_pext22_test_5",
        .cfg          = &fake_cfg,
        .cfg_mutex    = &mtx,
    };

    // "unknown-model" is not in the configured list; ModelCmd accepts it with a warning.
    auto result = cmd->execute("unknown-model", ctx);

    CHECK(static_cast<bool>(result));
    // PEXT 2.2: out-of-list switch must still mutate the running Config.
    CHECK(fake_cfg.api.default_model == "unknown-model");
    CHECK(out.str().find("Switched to model 'unknown-model'") != std::string::npos);
    CHECK(out.str().find("not in configured BATBOX_MODELS list") != std::string::npos);
}
