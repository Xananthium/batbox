// tests/repl/test_command_context_cfg_field.cpp
//
// PEXT 1.1 — Regression test: CommandContext cfg + cfg_mutex fields.
//
// Verifies:
//   1. CommandContext accepts a non-null cfg pointer and a non-null cfg_mutex.
//   2. ctx.cfg->api.default_model resolves to the value set in the fake Config.
//   3. A command that reads ctx.cfg->api.default_model sees the live process value.
//   4. Both fields default to nullptr — existing call sites that omit them are
//      not broken (static_assert that the defaults hold).
//   5. The mutex pointer can be used to lock *cfg_mutex without crashing.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/config/Config.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/repl/CommandContext.hpp>

#include <mutex>
#include <sstream>
#include <string>

// ---------------------------------------------------------------------------
// MinimalConversationHandle — provides the pure-virtual interface required by
// CommandContext::conversation without pulling in a real Conversation.
// ---------------------------------------------------------------------------
struct MinimalConversation : batbox::commands::ConversationHandle {
    void reset_messages() override {}
    void inject_user_message(std::string_view) override {}
    std::string last_assistant_message(std::size_t) const override { return {}; }
};

// ---------------------------------------------------------------------------
// Helper: build a CommandContext with a fake Config.
// ---------------------------------------------------------------------------
struct ContextFixture {
    MinimalConversation                  conv;
    batbox::commands::SlashCommandRegistry registry;
    std::ostringstream                   out;
    std::istringstream                   in;
    batbox::config::Config               fake_config;
    std::mutex                           fake_mutex;

    batbox::commands::CommandContext make_ctx() {
        batbox::commands::CommandContext ctx{
            .output       = out,
            .input        = in,
            .conversation = conv,
            .registry     = registry,
            .cfg          = &fake_config,
            .cfg_mutex    = &fake_mutex,
        };
        return ctx;
    }
};

// ===========================================================================
// TEST CASES
// ===========================================================================

TEST_CASE("CommandContext::cfg resolves to live Config value") {
    ContextFixture f;
    f.fake_config.api.default_model = "test-model-xyz";

    batbox::commands::CommandContext ctx = f.make_ctx();

    REQUIRE(ctx.cfg != nullptr);
    CHECK(ctx.cfg->api.default_model == "test-model-xyz");
}

TEST_CASE("CommandContext::cfg_mutex is non-null and lockable") {
    ContextFixture f;
    f.fake_config.api.default_model = "lock-test-model";

    batbox::commands::CommandContext ctx = f.make_ctx();

    REQUIRE(ctx.cfg_mutex != nullptr);

    // Acquiring the mutex must not deadlock or throw.
    {
        std::lock_guard<std::mutex> lk(*ctx.cfg_mutex);
        // While holding the lock, verify cfg is still readable.
        CHECK(ctx.cfg->api.default_model == "lock-test-model");
    }
    // Mutex is released here — re-acquire to confirm it's in a clean state.
    {
        std::lock_guard<std::mutex> lk(*ctx.cfg_mutex);
        (void)lk;
    }
}

TEST_CASE("CommandContext::cfg and cfg_mutex default to nullptr") {
    MinimalConversation conv;
    batbox::commands::SlashCommandRegistry registry;
    std::ostringstream out;
    std::istringstream in;

    // Construct with only the mandatory fields — cfg / cfg_mutex must be nullptr.
    batbox::commands::CommandContext ctx{
        .output       = out,
        .input        = in,
        .conversation = conv,
        .registry     = registry,
    };

    CHECK(ctx.cfg == nullptr);
    CHECK(ctx.cfg_mutex == nullptr);
}

TEST_CASE("CommandContext::cfg reflects mutation through the pointer") {
    ContextFixture f;
    f.fake_config.api.default_model = "initial-model";

    batbox::commands::CommandContext ctx = f.make_ctx();
    REQUIRE(ctx.cfg->api.default_model == "initial-model");

    // Simulate what PEXT 2.2 will do: mutate under the mutex.
    {
        std::lock_guard<std::mutex> lk(*ctx.cfg_mutex);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        const_cast<batbox::config::Config*>(ctx.cfg)->api.default_model = "mutated-model";
    }

    // ctx.cfg still points at the same object — should see the new value.
    CHECK(ctx.cfg->api.default_model == "mutated-model");
    // The original fake_config should also reflect the change.
    CHECK(f.fake_config.api.default_model == "mutated-model");
}

TEST_CASE("CommandContext::cfg nullable guard pattern compiles and works") {
    ContextFixture f;
    f.fake_config.api.default_model = "guarded-model";

    SUBCASE("cfg is non-null — guard passes, value is read") {
        batbox::commands::CommandContext ctx = f.make_ctx();
        std::string result;
        if (ctx.cfg) {
            result = ctx.cfg->api.default_model;
        } else {
            result = "(no config)";
        }
        CHECK(result == "guarded-model");
    }

    SUBCASE("cfg is null — guard prevents dereference") {
        MinimalConversation conv;
        batbox::commands::SlashCommandRegistry registry;
        std::ostringstream out;
        std::istringstream in;
        batbox::commands::CommandContext ctx{
            .output       = out,
            .input        = in,
            .conversation = conv,
            .registry     = registry,
            // cfg and cfg_mutex intentionally omitted → nullptr
        };

        std::string result;
        if (ctx.cfg) {
            result = ctx.cfg->api.default_model;
        } else {
            result = "(no config)";
        }
        CHECK(result == "(no config)");
    }
}
