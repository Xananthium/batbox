// tests/unit/test_slash_command_pod.cpp
//
// doctest suite for PEXT3 2.1 — SlashCommand POD + PodCommandAdapter shim.
//
// Coverage:
//   - POD-style command registers via register_command(const SlashCommand*)
//   - POD command dispatches correctly (free function actually called)
//   - ISlashCommand command and POD command coexist in the same registry
//   - lookup() finds both by primary name
//   - Dispatch of ISlashCommand command still works alongside POD command
//   - POD with aliases registered; lookup by alias works
//   - Duplicate POD primary name returns error (collision detection)
//   - Duplicate alias between POD and ISlashCommand command returns error
//   - Null POD pointer returns error
//   - POD with null execute pointer returns error
//   - POD command appears in all() and names() output
//   - fuzzy_find scores POD commands identically to ISlashCommand commands

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/repl/CommandContext.hpp>

#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace batbox::commands;

// ============================================================================
// Minimal ConversationHandle stub — required to construct CommandContext
// ============================================================================

struct NullConversation final : ConversationHandle {
    void reset_messages() override {}
    void inject_user_message(std::string_view) override {}
    [[nodiscard]] std::string last_assistant_message(std::size_t) const override {
        return {};
    }
};

// ============================================================================
// Helpers
// ============================================================================

/// Build a CommandContext for dispatch tests.
/// Holds the sidecar objects that CommandContext members reference.
struct TestCtx {
    NullConversation    conversation;
    std::ostringstream  output_buf;
    std::istringstream  input_buf;
    SlashCommandRegistry registry;

    CommandContext make() {
        return CommandContext{
            .output       = output_buf,
            .input        = input_buf,
            .conversation = conversation,
            .registry     = registry,
        };
    }
};

// ============================================================================
// File-scope free functions used as POD execute targets
// ============================================================================

static bool g_ping_executed = false;

static batbox::Result<void> execute_ping(std::string_view /*args*/,
                                          CommandContext& /*ctx*/) {
    g_ping_executed = true;
    return {};
}

static batbox::Result<void> execute_future(std::string_view /*args*/,
                                            CommandContext& /*ctx*/) {
    return {};
}

static batbox::Result<void> execute_noop(std::string_view /*args*/,
                                          CommandContext& /*ctx*/) {
    return {};
}

static batbox::Result<void> execute_ping_dup(std::string_view /*args*/,
                                              CommandContext& /*ctx*/) {
    return {};
}

// ============================================================================
// File-scope SlashCommand POD records (constexpr requires C++20 constant
// expression; function pointers are constant expressions in C++20).
// Note: non-constexpr static is fine — lifetime is static.
// ============================================================================

static const std::string_view kPingAliases[] = { "p", "pong" };

static const SlashCommand kPingCmd = {
    .name          = "ping",
    .description   = "Ping the registry to verify POD dispatch.",
    .usage         = "/ping",
    .aliases       = kPingAliases,
    .requires_args = false,
    .phase         = CommandPhase::Phase1,
    .execute       = &execute_ping,
};

static const SlashCommand kFutureCmd = {
    .name        = "future",
    .description = "Coming soon placeholder.",
    .usage       = "/future",
    .phase       = CommandPhase::Phase2,
    .execute     = &execute_future,
};

static const SlashCommand kNoAliasCmd = {
    .name        = "noop",
    .description = "Does nothing.",
    .usage       = "/noop",
    // aliases defaults to empty span
    .execute     = &execute_noop,
};

static const SlashCommand kPingDupCmd = {
    .name        = "ping",
    .description = "Duplicate ping — must fail registration.",
    .usage       = "/ping",
    .execute     = &execute_ping_dup,
};

static const SlashCommand kNullExecuteCmd = {
    .name        = "bad",
    .description = "Bad command — null execute.",
    .usage       = "/bad",
    .execute     = nullptr,
};

// ============================================================================
// Minimal ISlashCommand stub — mirrors existing test_slash_command_registry pattern
// ============================================================================

struct StubCommand final : ISlashCommand {
    std::string name_;
    std::string description_;
    std::string usage_;
    std::vector<std::string> aliases_;
    bool requires_args_ = false;
    CommandPhase phase_  = CommandPhase::Phase1;
    bool executed        = false;

    StubCommand(std::string n,
                std::string desc  = "stub description",
                std::string usg   = "/stub",
                std::vector<std::string> als = {})
        : name_(std::move(n))
        , description_(std::move(desc))
        , usage_(std::move(usg))
        , aliases_(std::move(als))
    {}

    [[nodiscard]] std::string_view name()        const noexcept override { return name_; }
    [[nodiscard]] std::string_view description() const noexcept override { return description_; }
    [[nodiscard]] std::string_view usage()       const noexcept override { return usage_; }
    [[nodiscard]] std::vector<std::string> aliases() const override      { return aliases_; }
    [[nodiscard]] bool         requires_args()   const noexcept override { return requires_args_; }
    [[nodiscard]] CommandPhase phase()           const noexcept override { return phase_; }

    [[nodiscard]] batbox::Result<void> execute(std::string_view, CommandContext&) override {
        executed = true;
        return {};
    }
};

static std::shared_ptr<StubCommand> make_stub(
    std::string name,
    std::string desc    = "stub description",
    std::string usage   = "/stub",
    std::vector<std::string> aliases = {})
{
    return std::make_shared<StubCommand>(
        std::move(name), std::move(desc), std::move(usage), std::move(aliases));
}

// ============================================================================
// TEST SUITE 1: Basic POD registration and dispatch
// ============================================================================
TEST_SUITE("SlashCommand POD — registration & dispatch") {

    TEST_CASE("POD command registers successfully") {
        SlashCommandRegistry reg;
        auto result = reg.register_command(&kPingCmd);
        REQUIRE(result.has_value());
        CHECK(reg.size() == 1);
    }

    TEST_CASE("POD command found by primary name") {
        SlashCommandRegistry reg;
        REQUIRE(reg.register_command(&kPingCmd).has_value());

        ISlashCommand* found = reg.lookup("ping");
        REQUIRE(found != nullptr);
        CHECK(found->name() == "ping");
    }

    TEST_CASE("POD command description forwarded correctly") {
        SlashCommandRegistry reg;
        REQUIRE(reg.register_command(&kPingCmd).has_value());

        ISlashCommand* found = reg.lookup("ping");
        REQUIRE(found != nullptr);
        CHECK(found->description() == "Ping the registry to verify POD dispatch.");
    }

    TEST_CASE("POD command usage forwarded correctly") {
        SlashCommandRegistry reg;
        REQUIRE(reg.register_command(&kPingCmd).has_value());

        ISlashCommand* found = reg.lookup("ping");
        REQUIRE(found != nullptr);
        CHECK(found->usage() == "/ping");
    }

    TEST_CASE("POD command requires_args forwarded correctly") {
        SlashCommandRegistry reg;
        REQUIRE(reg.register_command(&kPingCmd).has_value());

        ISlashCommand* found = reg.lookup("ping");
        REQUIRE(found != nullptr);
        CHECK(found->requires_args() == false);
    }

    TEST_CASE("POD command phase forwarded correctly — Phase1") {
        SlashCommandRegistry reg;
        REQUIRE(reg.register_command(&kPingCmd).has_value());

        ISlashCommand* found = reg.lookup("ping");
        REQUIRE(found != nullptr);
        CHECK(found->phase() == CommandPhase::Phase1);
    }

    TEST_CASE("POD command phase forwarded correctly — Phase2") {
        SlashCommandRegistry reg;
        REQUIRE(reg.register_command(&kFutureCmd).has_value());

        ISlashCommand* found = reg.lookup("future");
        REQUIRE(found != nullptr);
        CHECK(found->phase() == CommandPhase::Phase2);
    }

    TEST_CASE("POD command dispatch invokes the free function") {
        g_ping_executed = false;

        SlashCommandRegistry reg;
        REQUIRE(reg.register_command(&kPingCmd).has_value());

        ISlashCommand* found = reg.lookup("ping");
        REQUIRE(found != nullptr);

        TestCtx tc;
        auto ctx = tc.make();
        auto res = found->execute("", ctx);

        CHECK(res.has_value());
        CHECK(g_ping_executed);
    }

    TEST_CASE("POD command aliases forwarded — lookup by alias succeeds") {
        SlashCommandRegistry reg;
        REQUIRE(reg.register_command(&kPingCmd).has_value());

        ISlashCommand* by_p    = reg.lookup("p");
        ISlashCommand* by_pong = reg.lookup("pong");

        REQUIRE(by_p != nullptr);
        CHECK(by_p->name() == "ping");

        REQUIRE(by_pong != nullptr);
        CHECK(by_pong->name() == "ping");
    }

    TEST_CASE("POD command aliases() returns correct vector") {
        SlashCommandRegistry reg;
        REQUIRE(reg.register_command(&kPingCmd).has_value());

        ISlashCommand* found = reg.lookup("ping");
        REQUIRE(found != nullptr);

        auto aliases = found->aliases();
        REQUIRE(aliases.size() == 2);
        // Order follows the span order: "p" before "pong".
        CHECK(aliases[0] == "p");
        CHECK(aliases[1] == "pong");
    }

    TEST_CASE("POD with empty aliases span registers and dispatches") {
        SlashCommandRegistry reg;
        REQUIRE(reg.register_command(&kNoAliasCmd).has_value());

        ISlashCommand* found = reg.lookup("noop");
        REQUIRE(found != nullptr);
        CHECK(found->aliases().empty());

        TestCtx tc;
        auto ctx = tc.make();
        CHECK(found->execute("", ctx).has_value());
    }
}

// ============================================================================
// TEST SUITE 2: Coexistence of POD and ISlashCommand in the same registry
// ============================================================================
TEST_SUITE("SlashCommand POD — coexistence with ISlashCommand") {

    TEST_CASE("POD and ISlashCommand commands coexist — both found by lookup") {
        SlashCommandRegistry reg;
        REQUIRE(reg.register_command(&kPingCmd).has_value());
        REQUIRE(reg.register_command(make_stub("help", "Show help", "/help")).has_value());

        CHECK(reg.size() == 2);
        CHECK(reg.lookup("ping") != nullptr);
        CHECK(reg.lookup("help") != nullptr);
    }

    TEST_CASE("ISlashCommand dispatch works alongside POD command") {
        SlashCommandRegistry reg;
        REQUIRE(reg.register_command(&kPingCmd).has_value());

        auto help_stub = make_stub("help", "Show help", "/help");
        REQUIRE(reg.register_command(help_stub).has_value());

        ISlashCommand* found = reg.lookup("help");
        REQUIRE(found != nullptr);

        TestCtx tc;
        auto ctx = tc.make();
        auto res = found->execute("", ctx);

        CHECK(res.has_value());
        CHECK(help_stub->executed);
    }

    TEST_CASE("POD dispatch still works when ISlashCommand also registered") {
        g_ping_executed = false;

        SlashCommandRegistry reg;
        REQUIRE(reg.register_command(make_stub("help", "Show help", "/help")).has_value());
        REQUIRE(reg.register_command(&kPingCmd).has_value());

        ISlashCommand* found = reg.lookup("ping");
        REQUIRE(found != nullptr);

        TestCtx tc;
        auto ctx = tc.make();
        auto res = found->execute("", ctx);

        CHECK(res.has_value());
        CHECK(g_ping_executed);
    }

    TEST_CASE("all() includes both POD and ISlashCommand entries sorted by name") {
        SlashCommandRegistry reg;
        REQUIRE(reg.register_command(&kPingCmd).has_value());
        REQUIRE(reg.register_command(make_stub("help")).has_value());

        auto cmds = reg.all();
        REQUIRE(cmds.size() == 2);

        // all() is sorted by primary name: help < ping
        CHECK(cmds[0]->name() == "help");
        CHECK(cmds[1]->name() == "ping");
    }

    TEST_CASE("names() includes both POD and ISlashCommand entries sorted") {
        SlashCommandRegistry reg;
        REQUIRE(reg.register_command(&kPingCmd).has_value());
        REQUIRE(reg.register_command(make_stub("alpha")).has_value());

        auto ns = reg.names();
        REQUIRE(ns.size() == 2);
        CHECK(ns[0] == "alpha");
        CHECK(ns[1] == "ping");
    }

    TEST_CASE("fuzzy_find returns POD command alongside ISlashCommand") {
        SlashCommandRegistry reg;
        REQUIRE(reg.register_command(&kPingCmd).has_value());
        REQUIRE(reg.register_command(make_stub("pivot", "Pivot something")).has_value());

        // Both "ping" and "pivot" start with "pi" — score 80 each.
        auto results = reg.fuzzy_find("pi");
        REQUIRE(results.size() == 2);

        std::vector<std::string_view> found_names;
        for (auto* p : results) found_names.push_back(p->name());

        bool has_ping  = std::find(found_names.begin(), found_names.end(), "ping")  != found_names.end();
        bool has_pivot = std::find(found_names.begin(), found_names.end(), "pivot") != found_names.end();
        CHECK(has_ping);
        CHECK(has_pivot);
    }
}

// ============================================================================
// TEST SUITE 3: Collision detection via the POD overload
// ============================================================================
TEST_SUITE("SlashCommand POD — collision detection") {

    TEST_CASE("null POD pointer returns error") {
        SlashCommandRegistry reg;
        auto result = reg.register_command(static_cast<const SlashCommand*>(nullptr));
        CHECK_FALSE(result.has_value());
    }

    TEST_CASE("POD with null execute pointer returns error") {
        SlashCommandRegistry reg;
        auto result = reg.register_command(&kNullExecuteCmd);
        CHECK_FALSE(result.has_value());
    }

    TEST_CASE("duplicate POD primary name returns error") {
        SlashCommandRegistry reg;
        REQUIRE(reg.register_command(&kPingCmd).has_value());

        // kPingDupCmd has the same primary name "ping".
        auto result = reg.register_command(&kPingDupCmd);
        CHECK_FALSE(result.has_value());

        // Original must still be in place.
        CHECK(reg.size() == 1);
        CHECK(reg.lookup("ping") != nullptr);
    }

    TEST_CASE("POD primary name collides with existing ISlashCommand name") {
        SlashCommandRegistry reg;
        REQUIRE(reg.register_command(make_stub("ping")).has_value());
        auto result = reg.register_command(&kPingCmd);
        CHECK_FALSE(result.has_value());
        CHECK(reg.size() == 1);
    }

    TEST_CASE("ISlashCommand primary name collides with existing POD name") {
        SlashCommandRegistry reg;
        REQUIRE(reg.register_command(&kPingCmd).has_value());
        auto result = reg.register_command(make_stub("ping"));
        CHECK_FALSE(result.has_value());
        CHECK(reg.size() == 1);
    }

    TEST_CASE("POD alias collides with existing ISlashCommand alias") {
        // kPingCmd has aliases "p" and "pong".
        SlashCommandRegistry reg;
        REQUIRE(reg.register_command(&kPingCmd).has_value());
        // Register a stub whose alias collides with "p".
        auto result = reg.register_command(
            make_stub("poke", "Poke something", "/poke", {"p"}));
        CHECK_FALSE(result.has_value());
        CHECK(reg.size() == 1);
    }

    TEST_CASE("ISlashCommand alias collides with existing POD alias") {
        // kPingCmd has aliases "p" and "pong".
        SlashCommandRegistry reg;
        REQUIRE(reg.register_command(&kPingCmd).has_value());
        // Register a stub whose alias collides with "pong".
        auto result = reg.register_command(
            make_stub("poke2", "Another poke", "/poke2", {"pong"}));
        CHECK_FALSE(result.has_value());
        CHECK(reg.size() == 1);
    }
}
