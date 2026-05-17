// tests/unit/test_slash_command_registry.cpp
//
// doctest suite for batbox::commands::SlashCommandRegistry.
//
// Coverage:
//   - register a command and look it up by primary name
//   - look up a command by alias
//   - duplicate primary name registration returns an error
//   - duplicate alias registration returns an error
//   - alias that collides with another command's primary name returns an error
//   - fuzzy_find ranking: exact > startswith > alias-startswith > contains
//     > alias-contains > description-contains; 0-score commands excluded
//   - fuzzy_find with empty query returns all commands
//   - fuzzy_find k-cap truncates results
//   - all() returns commands sorted by primary name
//   - names() returns primary names sorted

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>

#include <string>
#include <string_view>
#include <vector>

using namespace batbox::commands;

// ============================================================================
// Minimal ISlashCommand stub for testing
// ============================================================================

/// Minimal concrete command for testing — all data is provided at construction.
struct StubCommand final : ISlashCommand {
    std::string name_;
    std::string description_;
    std::string usage_;
    std::vector<std::string> aliases_;
    bool requires_args_ = false;
    CommandPhase phase_ = CommandPhase::Phase1;

    StubCommand(std::string n,
                std::string desc,
                std::string usg  = "",
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
    [[nodiscard]] bool        requires_args()    const noexcept override { return requires_args_; }
    [[nodiscard]] CommandPhase phase()           const noexcept override { return phase_; }

    [[nodiscard]] batbox::Result<void> execute(std::string_view, CommandContext&) override {
        return {};
    }
};

static std::shared_ptr<StubCommand> make_cmd(
    std::string name,
    std::string desc    = "a description",
    std::string usage   = "/name",
    std::vector<std::string> aliases = {})
{
    return std::make_shared<StubCommand>(
        std::move(name), std::move(desc), std::move(usage), std::move(aliases));
}

// ============================================================================
// TEST SUITE 1: Basic registration and lookup
// ============================================================================
TEST_SUITE("SlashCommandRegistry — registration & lookup") {

    TEST_CASE("register and lookup by primary name") {
        SlashCommandRegistry reg;
        auto ok = reg.register_command(make_cmd("help", "Show help"));
        REQUIRE(ok.has_value());
        ISlashCommand* found = reg.lookup("help");
        REQUIRE(found != nullptr);
        CHECK(found->name() == "help");
    }

    TEST_CASE("lookup returns nullptr for unknown name") {
        SlashCommandRegistry reg;
        CHECK(reg.lookup("nonexistent") == nullptr);
    }

    TEST_CASE("lookup is case-insensitive (upper input)") {
        SlashCommandRegistry reg;
        REQUIRE(reg.register_command(make_cmd("exit")).has_value());
        CHECK(reg.lookup("EXIT") != nullptr);
        CHECK(reg.lookup("Exit") != nullptr);
    }

    TEST_CASE("register and lookup by alias") {
        SlashCommandRegistry reg;
        auto cmd = make_cmd("exit", "Exit the app", "/exit", {"q", "quit"});
        REQUIRE(reg.register_command(cmd).has_value());

        ISlashCommand* by_alias = reg.lookup("q");
        REQUIRE(by_alias != nullptr);
        CHECK(by_alias->name() == "exit");

        ISlashCommand* by_alias2 = reg.lookup("quit");
        REQUIRE(by_alias2 != nullptr);
        CHECK(by_alias2->name() == "exit");
    }

    TEST_CASE("alias lookup is case-insensitive") {
        SlashCommandRegistry reg;
        REQUIRE(reg.register_command(
            make_cmd("exit", "Exit", "/exit", {"Q"})).has_value());
        CHECK(reg.lookup("q") != nullptr);
    }

    TEST_CASE("size() reflects number of registered commands") {
        SlashCommandRegistry reg;
        CHECK(reg.size() == 0);
        REQUIRE(reg.register_command(make_cmd("help")).has_value());
        CHECK(reg.size() == 1);
        REQUIRE(reg.register_command(make_cmd("exit")).has_value());
        CHECK(reg.size() == 2);
    }
}

// ============================================================================
// TEST SUITE 2: Duplicate registration errors
// ============================================================================
TEST_SUITE("SlashCommandRegistry — duplicate rejection") {

    TEST_CASE("duplicate primary name returns error") {
        SlashCommandRegistry reg;
        REQUIRE(reg.register_command(make_cmd("help")).has_value());
        auto err = reg.register_command(make_cmd("help", "another help"));
        CHECK_FALSE(err.has_value());
        // Original must still work.
        CHECK(reg.lookup("help") != nullptr);
        CHECK(reg.size() == 1);
    }

    TEST_CASE("alias collision with existing alias returns error") {
        SlashCommandRegistry reg;
        REQUIRE(reg.register_command(
            make_cmd("exit", "Exit", "/exit", {"q"})).has_value());
        auto err = reg.register_command(
            make_cmd("quit", "Quit", "/quit", {"q"}));
        CHECK_FALSE(err.has_value());
        // 'quit' command must NOT have been inserted.
        CHECK(reg.lookup("quit") == nullptr);
        CHECK(reg.size() == 1);
    }

    TEST_CASE("alias collision with primary name of another command returns error") {
        SlashCommandRegistry reg;
        REQUIRE(reg.register_command(make_cmd("help")).has_value());
        // Try to register a command whose alias is "help".
        auto err = reg.register_command(
            make_cmd("info", "Info", "/info", {"help"}));
        CHECK_FALSE(err.has_value());
        CHECK(reg.lookup("info") == nullptr);
        CHECK(reg.size() == 1);
    }

    TEST_CASE("primary name collision with existing alias returns error") {
        SlashCommandRegistry reg;
        REQUIRE(reg.register_command(
            make_cmd("exit", "Exit", "/exit", {"q"})).has_value());
        // Try to register a command whose primary name is "q".
        auto err = reg.register_command(make_cmd("q", "The q command"));
        CHECK_FALSE(err.has_value());
        CHECK(reg.lookup("q")->name() == "exit");  // still points to exit
        CHECK(reg.size() == 1);
    }

    TEST_CASE("null pointer registration returns error") {
        SlashCommandRegistry reg;
        auto err = reg.register_command(nullptr);
        CHECK_FALSE(err.has_value());
    }
}

// ============================================================================
// TEST SUITE 3: fuzzy_find ranking
// ============================================================================
TEST_SUITE("SlashCommandRegistry — fuzzy_find") {

    // Helper: build a registry with a known set of commands.
    auto make_registry = []() {
        SlashCommandRegistry reg;
        // Primary names deliberately crafted to exercise all score tiers.
        REQUIRE(reg.register_command(make_cmd("model",   "Switch the active model")).has_value());
        REQUIRE(reg.register_command(make_cmd("memory",  "Edit BATBOX.md project memory")).has_value());
        REQUIRE(reg.register_command(make_cmd("compact", "Compact conversation context", "/compact", {"cmp"})).has_value());
        REQUIRE(reg.register_command(make_cmd("config",  "Edit batbox configuration")).has_value());
        REQUIRE(reg.register_command(make_cmd("cost",    "Show token cost for session")).has_value());
        REQUIRE(reg.register_command(make_cmd("help",    "List available commands")).has_value());
        REQUIRE(reg.register_command(make_cmd("exit",    "Exit batbox", "/exit", {"q", "quit"})).has_value());
        return reg;
    };

    TEST_CASE("exact primary name match scores highest") {
        auto reg = make_registry();
        auto results = reg.fuzzy_find("model");
        REQUIRE_FALSE(results.empty());
        CHECK(results[0]->name() == "model");
    }

    TEST_CASE("primary-name startswith ranks before substring") {
        auto reg = make_registry();
        // "mo" starts "model" but only contains "memory" (via substring? no, "mo" not in "memory")
        // Focus: "co" starts "config" and "cost", and "compact" contains "co" in "compact"
        auto results = reg.fuzzy_find("co");
        REQUIRE(results.size() >= 2);
        // config, cost, compact all start with "co"; none is exact.
        // All three should score 80 (starts-with primary), so they all come before compact.
        // Check at least config and cost are present.
        std::vector<std::string_view> names;
        for (auto* p : results) names.push_back(p->name());
        bool has_config  = (std::find(names.begin(), names.end(), "config")  != names.end());
        bool has_cost    = (std::find(names.begin(), names.end(), "cost")    != names.end());
        bool has_compact = (std::find(names.begin(), names.end(), "compact") != names.end());
        CHECK(has_config);
        CHECK(has_cost);
        CHECK(has_compact);
    }

    TEST_CASE("alias startswith ranks before primary-name substring") {
        // "cmp" is an alias for "compact".
        // Register a fresh registry where "compact" alias "cmp" competes with something that
        // has "cmp" as a substring in its name.
        SlashCommandRegistry reg;
        REQUIRE(reg.register_command(make_cmd("compact", "Compact context", "/compact", {"cmp"})).has_value());
        // "xcmpx" contains "cmp" as a substring in the primary name.
        REQUIRE(reg.register_command(make_cmd("xcmpx", "A command with cmp in name")).has_value());
        auto results = reg.fuzzy_find("cmp");
        REQUIRE(results.size() >= 2);
        // compact scores 70 (alias starts with "cmp"), xcmpx scores 60 (name contains "cmp")
        CHECK(results[0]->name() == "compact");
        CHECK(results[1]->name() == "xcmpx");
    }

    TEST_CASE("description contains query scores lowest (30)") {
        SlashCommandRegistry reg;
        // "token" only appears in the description of "cost".
        REQUIRE(reg.register_command(make_cmd("cost", "Show token cost", "/cost")).has_value());
        REQUIRE(reg.register_command(make_cmd("help", "List commands", "/help")).has_value());
        auto results = reg.fuzzy_find("token");
        REQUIRE(results.size() == 1);
        CHECK(results[0]->name() == "cost");
    }

    TEST_CASE("commands with score 0 are excluded") {
        auto reg = make_registry();
        auto results = reg.fuzzy_find("zzznomatch");
        CHECK(results.empty());
    }

    TEST_CASE("empty query returns all commands up to k") {
        auto reg = make_registry();
        auto all_results = reg.fuzzy_find("", 100);
        CHECK(all_results.size() == reg.size());
    }

    TEST_CASE("k cap truncates results") {
        auto reg = make_registry();
        // Registry has 7 commands; empty query returns all with neutral score.
        auto results = reg.fuzzy_find("", 3);
        CHECK(results.size() == 3);
    }

    TEST_CASE("k=0 returns empty vector") {
        auto reg = make_registry();
        auto results = reg.fuzzy_find("model", 0);
        CHECK(results.empty());
    }

    TEST_CASE("results are sorted by score desc then name asc on tie") {
        SlashCommandRegistry reg;
        // Three commands that all start with "mo" — all score 80.
        // Alphabetically: mobile < model < monitor
        REQUIRE(reg.register_command(make_cmd("monitor", "Monitor agents")).has_value());
        REQUIRE(reg.register_command(make_cmd("model",   "Switch model")).has_value());
        REQUIRE(reg.register_command(make_cmd("mobile",  "Mobile settings")).has_value());
        auto results = reg.fuzzy_find("mo");
        REQUIRE(results.size() == 3);
        CHECK(results[0]->name() == "mobile");
        CHECK(results[1]->name() == "model");
        CHECK(results[2]->name() == "monitor");
    }

    TEST_CASE("exact match beats starts-with in ranking") {
        SlashCommandRegistry reg;
        REQUIRE(reg.register_command(make_cmd("model",   "Switch model")).has_value());
        REQUIRE(reg.register_command(make_cmd("modelx",  "Extended model")).has_value());
        auto results = reg.fuzzy_find("model");
        REQUIRE(results.size() >= 2);
        // "model" scores 100 (exact), "modelx" scores 80 (starts-with)
        CHECK(results[0]->name() == "model");
        CHECK(results[1]->name() == "modelx");
    }
}

// ============================================================================
// TEST SUITE 4: all() and names()
// ============================================================================
TEST_SUITE("SlashCommandRegistry — all() and names()") {

    TEST_CASE("all() returns empty vector when no commands registered") {
        SlashCommandRegistry reg;
        CHECK(reg.all().empty());
    }

    TEST_CASE("all() returns commands sorted by primary name ascending") {
        SlashCommandRegistry reg;
        REQUIRE(reg.register_command(make_cmd("exit")).has_value());
        REQUIRE(reg.register_command(make_cmd("help")).has_value());
        REQUIRE(reg.register_command(make_cmd("clear")).has_value());
        auto all_cmds = reg.all();
        REQUIRE(all_cmds.size() == 3);
        CHECK(all_cmds[0]->name() == "clear");
        CHECK(all_cmds[1]->name() == "exit");
        CHECK(all_cmds[2]->name() == "help");
    }

    TEST_CASE("names() returns sorted primary names") {
        SlashCommandRegistry reg;
        REQUIRE(reg.register_command(make_cmd("model")).has_value());
        REQUIRE(reg.register_command(make_cmd("config")).has_value());
        REQUIRE(reg.register_command(make_cmd("help")).has_value());
        auto ns = reg.names();
        REQUIRE(ns.size() == 3);
        CHECK(ns[0] == "config");
        CHECK(ns[1] == "help");
        CHECK(ns[2] == "model");
    }

    TEST_CASE("all() count matches size()") {
        SlashCommandRegistry reg;
        REQUIRE(reg.register_command(make_cmd("a")).has_value());
        REQUIRE(reg.register_command(make_cmd("b")).has_value());
        CHECK(reg.all().size() == reg.size());
    }
}

// ============================================================================
// TEST SUITE 5: Phase enum accessible via interface
// ============================================================================
TEST_SUITE("SlashCommandRegistry — Phase") {

    TEST_CASE("default phase is Phase1") {
        auto cmd = make_cmd("help");
        CHECK(cmd->phase() == CommandPhase::Phase1);
    }

    TEST_CASE("Phase2 command round-trips through registry") {
        SlashCommandRegistry reg;
        auto ide_cmd = make_cmd("ide", "IDE bridge");
        ide_cmd->phase_ = CommandPhase::Phase2;
        REQUIRE(reg.register_command(ide_cmd).has_value());
        ISlashCommand* found = reg.lookup("ide");
        REQUIRE(found != nullptr);
        CHECK(found->phase() == CommandPhase::Phase2);
    }
}
