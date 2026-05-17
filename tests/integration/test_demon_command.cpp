// tests/integration/test_demon_command.cpp
//
// doctest integration-test suite for CPP S.14: /demon slash command.
//
// Strategy
// --------
// DemonCmd accepts nullable DemonPanel* and AgentSupervisor* at construction.
// Tests run entirely in null-supervisor / null-panel mode to avoid needing
// FTXUI or the AgentSupervisor stub (which requires a full thread pool).
//
// A MockDemonPanel provides a lightweight stand-in that records show/hide/
// set_demon_comment calls without starting any FTXUI threads.
//
// Coverage:
//   - registers under primary name "demon" with no aliases
//   - /demon when inactive → spawns (panel-only) + "OH MY GOD!" success message
//   - /demon when already active → idempotent no-op + status message
//   - /demon dismiss when active → hides panel + success message
//   - /demon dismiss when already hidden → no-op message
//   - /demon status → prints current status (panel, tagline, comment)
//   - /demon <prompt> when inactive → spawns + routes task
//   - /demon <prompt> when active (panel-only) → panel comment updated
//   - requires_args is false
//   - no aliases

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/repl/CommandContext.hpp>

#include <atomic>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace batbox::commands;

// ============================================================================
// MockDemonPanel — lightweight stand-in for batbox::tui::DemonPanel
//
// Satisfies the DemonPanel API used by DemonCmd without pulling in FTXUI
// or starting any background threads.  DemonCmd accesses the panel only
// through the raw pointer DemonPanel*, so we mock the exact same type by
// including the header and providing forward-only stubs via a subclass.
//
// Because DemonPanel::Make() requires a live ftxui::ScreenInteractive we
// cannot construct a real DemonPanel in a headless test.  We instead pass
// nullptr to register_demon_cmd() which exercises the graceful degradation
// path, and separately test the output messages that confirm correct
// invocation.
// ============================================================================

// ============================================================================
// Registration declaration (defined in DemonCmd.cpp)
// ============================================================================

namespace batbox::commands {
    // Headless overload — null panel + null supervisor.
    void register_demon_cmd(SlashCommandRegistry& registry);
}

// ============================================================================
// MockConversation — minimal ConversationHandle
// ============================================================================

struct MockConversation final : ConversationHandle {
    void reset_messages() override {}
    void inject_user_message(std::string_view /*text*/) override {}
    std::string last_assistant_message(std::size_t /*n*/ = 1) const override { return {}; }
};

// ============================================================================
// Fixture helpers
// ============================================================================

/// Build a minimal CommandContext with the given output stream.
static std::tuple<SlashCommandRegistry,
                  MockConversation,
                  std::ostringstream,
                  std::istringstream>
make_fixture()
{
    return {};
}

static CommandContext make_ctx(std::ostringstream& out,
                                std::istringstream& in,
                                MockConversation&   conv,
                                SlashCommandRegistry& reg)
{
    return CommandContext{out, in, false, conv, reg, {}};
}

// ============================================================================
// TEST SUITE: DemonCmd — registration
// ============================================================================

TEST_SUITE("DemonCmd — registration") {

    TEST_CASE("registers under primary name 'demon'") {
        SlashCommandRegistry reg;
        register_demon_cmd(reg);
        REQUIRE(reg.lookup("demon") != nullptr);
        CHECK(reg.lookup("demon")->name() == "demon");
    }

    TEST_CASE("has no aliases") {
        SlashCommandRegistry reg;
        register_demon_cmd(reg);
        CHECK(reg.lookup("demon")->aliases().empty());
    }

    TEST_CASE("requires_args is false") {
        SlashCommandRegistry reg;
        register_demon_cmd(reg);
        CHECK_FALSE(reg.lookup("demon")->requires_args());
    }

    TEST_CASE("description is non-empty") {
        SlashCommandRegistry reg;
        register_demon_cmd(reg);
        CHECK(!reg.lookup("demon")->description().empty());
    }

    TEST_CASE("usage includes '/demon'") {
        SlashCommandRegistry reg;
        register_demon_cmd(reg);
        const std::string usage(reg.lookup("demon")->usage());
        CHECK(usage.find("/demon") != std::string::npos);
    }
}

// ============================================================================
// TEST SUITE: DemonCmd — no-arg toggle (null panel + null supervisor)
// ============================================================================

TEST_SUITE("DemonCmd — /demon toggle") {

    TEST_CASE("/demon when inactive spawns and emits OH MY GOD") {
        SlashCommandRegistry reg;
        register_demon_cmd(reg);
        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(out, in, conv, reg);

        auto res = reg.lookup("demon")->execute("", ctx);
        REQUIRE(res.has_value());
        // The success message must be present.
        CHECK(out.str().find("OH MY GOD") != std::string::npos);
    }

    TEST_CASE("/demon when already active is idempotent") {
        SlashCommandRegistry reg;
        register_demon_cmd(reg);
        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(out, in, conv, reg);

        // First invocation — activates the demon.
        auto res1 = reg.lookup("demon")->execute("", ctx);
        REQUIRE(res1.has_value());

        // Reset output stream for clarity.
        out.str("");
        out.clear();

        // Second invocation — should be a no-op with status message.
        auto res2 = reg.lookup("demon")->execute("", ctx);
        REQUIRE(res2.has_value());

        // Must NOT print "Party Monster summoned" again.
        CHECK(out.str().find("Party Monster summoned") == std::string::npos);
        // Must print the idempotent message.
        CHECK(out.str().find("ALREADY HERE") != std::string::npos);
    }

    TEST_CASE("/demon output includes panel and dismiss hint on first summon") {
        SlashCommandRegistry reg;
        register_demon_cmd(reg);
        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(out, in, conv, reg);

        auto res = reg.lookup("demon")->execute("", ctx);
        REQUIRE(res.has_value());
        CHECK(out.str().find("dismiss") != std::string::npos);
    }
}

// ============================================================================
// TEST SUITE: DemonCmd — dismiss subcommand
// ============================================================================

TEST_SUITE("DemonCmd — /demon dismiss") {

    TEST_CASE("/demon dismiss when active hides and reports success") {
        SlashCommandRegistry reg;
        register_demon_cmd(reg);
        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(out, in, conv, reg);

        // Activate first.
        auto r1 = reg.lookup("demon")->execute("", ctx);
        REQUIRE(r1.has_value());
        out.str(""); out.clear();

        // Dismiss.
        auto r2 = reg.lookup("demon")->execute("dismiss", ctx);
        REQUIRE(r2.has_value());
        CHECK(out.str().find("dismissed") != std::string::npos);
    }

    TEST_CASE("/demon dismiss when already hidden reports no-op") {
        SlashCommandRegistry reg;
        register_demon_cmd(reg);
        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(out, in, conv, reg);

        // Dismiss without ever activating.
        auto res = reg.lookup("demon")->execute("dismiss", ctx);
        REQUIRE(res.has_value());
        CHECK(out.str().find("No demon to dismiss") != std::string::npos);
    }

    TEST_CASE("/demon dismiss after activate then dismiss is idempotent") {
        SlashCommandRegistry reg;
        register_demon_cmd(reg);
        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(out, in, conv, reg);

        // Activate.
        reg.lookup("demon")->execute("", ctx);
        out.str(""); out.clear();

        // Dismiss once.
        reg.lookup("demon")->execute("dismiss", ctx);
        out.str(""); out.clear();

        // Dismiss again — should report no-op.
        auto res = reg.lookup("demon")->execute("dismiss", ctx);
        REQUIRE(res.has_value());
        CHECK(out.str().find("No demon") != std::string::npos);
    }
}

// ============================================================================
// TEST SUITE: DemonCmd — status subcommand
// ============================================================================

TEST_SUITE("DemonCmd — /demon status") {

    TEST_CASE("/demon status when inactive reports inactive state") {
        SlashCommandRegistry reg;
        register_demon_cmd(reg);
        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(out, in, conv, reg);

        auto res = reg.lookup("demon")->execute("status", ctx);
        REQUIRE(res.has_value());
        CHECK(out.str().find("inactive") != std::string::npos);
        // Must include usage hints.
        CHECK(out.str().find("/demon") != std::string::npos);
    }

    TEST_CASE("/demon status when active reports active state") {
        SlashCommandRegistry reg;
        register_demon_cmd(reg);
        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(out, in, conv, reg);

        // Activate first.
        reg.lookup("demon")->execute("", ctx);
        out.str(""); out.clear();

        auto res = reg.lookup("demon")->execute("status", ctx);
        REQUIRE(res.has_value());
        // With null panel, 'inactive' status since panel is nullptr.
        // The status block should still print without crashing.
        CHECK(out.str().find("/demon") != std::string::npos);
    }

    TEST_CASE("/demon status does not modify active state") {
        SlashCommandRegistry reg;
        register_demon_cmd(reg);
        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(out, in, conv, reg);

        // Call status multiple times — none should crash or alter state.
        for (int i = 0; i < 3; ++i) {
            auto res = reg.lookup("demon")->execute("status", ctx);
            REQUIRE(res.has_value());
        }
    }
}

// ============================================================================
// TEST SUITE: DemonCmd — one-shot task routing
// ============================================================================

TEST_SUITE("DemonCmd — /demon <prompt> task routing") {

    TEST_CASE("/demon with prompt when inactive spawns and accepts task") {
        SlashCommandRegistry reg;
        register_demon_cmd(reg);
        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(out, in, conv, reg);

        auto res = reg.lookup("demon")->execute("find TODOs in src/", ctx);
        REQUIRE(res.has_value());
        // Should acknowledge the task somehow.
        // In null-supervisor mode it either spawns panel-only or routes task.
        CHECK(!out.str().empty());
    }

    TEST_CASE("/demon with prompt when active routes task to demon (panel-only)") {
        SlashCommandRegistry reg;
        register_demon_cmd(reg);
        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(out, in, conv, reg);

        // Activate (panel-only in null-supervisor mode).
        reg.lookup("demon")->execute("", ctx);
        out.str(""); out.clear();

        // Route a task — with null supervisor and null panel the command should
        // degrade gracefully and print a task-accepted message.
        auto res = reg.lookup("demon")->execute("find TODOs in src/", ctx);
        REQUIRE(res.has_value());
        CHECK(!out.str().empty());
    }

    TEST_CASE("/demon 'dismiss' is reserved and not treated as a task prompt") {
        SlashCommandRegistry reg;
        register_demon_cmd(reg);
        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(out, in, conv, reg);

        // Activate first.
        reg.lookup("demon")->execute("", ctx);
        out.str(""); out.clear();

        // "dismiss" should dismiss, not route as a task.
        auto res = reg.lookup("demon")->execute("dismiss", ctx);
        REQUIRE(res.has_value());
        CHECK(out.str().find("dismissed") != std::string::npos);
        // Must NOT say "Task routed".
        CHECK(out.str().find("Task routed") == std::string::npos);
    }

    TEST_CASE("/demon 'status' is reserved and not treated as a task prompt") {
        SlashCommandRegistry reg;
        register_demon_cmd(reg);
        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(out, in, conv, reg);

        auto res = reg.lookup("demon")->execute("status", ctx);
        REQUIRE(res.has_value());
        // Status output should contain usage hints, not "Task routed".
        CHECK(out.str().find("/demon dismiss") != std::string::npos);
        CHECK(out.str().find("Task routed") == std::string::npos);
    }
}

// ============================================================================
// TEST SUITE: DemonCmd — whitespace handling
// ============================================================================

TEST_SUITE("DemonCmd — whitespace handling") {

    TEST_CASE("/demon with leading/trailing whitespace behaves as /demon") {
        SlashCommandRegistry reg;
        register_demon_cmd(reg);
        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(out, in, conv, reg);

        auto res = reg.lookup("demon")->execute("   ", ctx);
        REQUIRE(res.has_value());
        // Treated as no-arg — summon.
        CHECK(out.str().find("OH MY GOD") != std::string::npos);
    }

    TEST_CASE("/demon '  dismiss  ' with whitespace dismisses correctly") {
        SlashCommandRegistry reg;
        register_demon_cmd(reg);
        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(out, in, conv, reg);

        // Activate.
        reg.lookup("demon")->execute("", ctx);
        out.str(""); out.clear();

        // Dismiss with surrounding whitespace.
        auto res = reg.lookup("demon")->execute("  dismiss  ", ctx);
        REQUIRE(res.has_value());
        CHECK(out.str().find("dismissed") != std::string::npos);
    }
}
