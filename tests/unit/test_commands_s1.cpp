// tests/unit/test_commands_s1.cpp
//
// doctest suite for CPP S.1: /help, /exit, /clear, /init slash commands.
//
// Strategy
// --------
// CommandContext is defined in include/batbox/repl/CommandContext.hpp and
// references ConversationHandle (a minimal virtual interface for /clear).
// This test file provides MockConversation which inherits ConversationHandle,
// then constructs CommandContext instances directly for each test.
//
// Each command .cpp is compiled directly into this test executable via
// CMakeLists.txt (same pattern as test_slash_command_registry).
//
// Coverage:
//   HelpCmd  — execute() writes category headers and each command name;
//              aliases line present when aliases exist.
//   ExitCmd  — execute() sets ctx.exit_requested = true; writes output.
//   ClearCmd — execute() calls conversation.reset_messages(); writes output.
//   InitCmd  — no markers → Err; valid root → writes BATBOX.md; existing
//              file + "n" → cancelled without overwrite; "y" → overwrite.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/repl/CommandContext.hpp>
#include <batbox/core/Result.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using namespace batbox::commands;

// ============================================================================
// MockConversation — satisfies ConversationHandle for testing ClearCmd
// ============================================================================

struct MockConversation final : ConversationHandle {
    bool reset_called = false;
    std::string last_injected;
    std::vector<std::string> assistant_messages;  // index 0 = oldest

    void reset_messages() override { reset_called = true; }
    void inject_user_message(std::string_view text) override { last_injected = std::string(text); }

    std::string last_assistant_message(std::size_t n = 1) const override {
        if (n == 0 || n > assistant_messages.size()) return {};
        return assistant_messages[assistant_messages.size() - n];
    }
};

// ============================================================================
// Registration function declarations (defined in each .cpp)
// ============================================================================

namespace batbox::commands {
    void register_help_cmd (SlashCommandRegistry&);
    void register_exit_cmd (SlashCommandRegistry&);
    void register_clear_cmd(SlashCommandRegistry&);
    void register_init_cmd (SlashCommandRegistry&);
}

// ============================================================================
// TEST SUITE: HelpCmd
// ============================================================================
TEST_SUITE("HelpCmd — /help") {

    TEST_CASE("registers under primary name 'help'") {
        SlashCommandRegistry reg;
        register_help_cmd(reg);
        REQUIRE(reg.lookup("help") != nullptr);
        CHECK(reg.lookup("help")->name() == "help");
    }

    TEST_CASE("registers alias '?'") {
        SlashCommandRegistry reg;
        register_help_cmd(reg);
        ISlashCommand* via_alias = reg.lookup("?");
        REQUIRE(via_alias != nullptr);
        CHECK(via_alias->name() == "help");
    }

    TEST_CASE("execute writes category header and command names") {
        SlashCommandRegistry reg;
        register_help_cmd(reg);
        register_exit_cmd(reg);
        register_clear_cmd(reg);
        register_init_cmd(reg);

        std::ostringstream out;
        std::istringstream in;
        MockConversation conv;
        CommandContext ctx{out, in, false, conv, reg, fs::current_path()};

        auto res = reg.lookup("help")->execute("", ctx);
        REQUIRE(res.has_value());

        const std::string text = out.str();
        CHECK(text.find("Core UX") != std::string::npos);
        CHECK(text.find("/exit")   != std::string::npos);
        CHECK(text.find("/clear")  != std::string::npos);
        CHECK(text.find("/help")   != std::string::npos);
    }

    TEST_CASE("execute prints aliases for /exit") {
        SlashCommandRegistry reg;
        register_help_cmd(reg);
        register_exit_cmd(reg);

        std::ostringstream out;
        std::istringstream in;
        MockConversation conv;
        CommandContext ctx{out, in, false, conv, reg, fs::current_path()};

        auto res = reg.lookup("help")->execute("", ctx);
        REQUIRE(res.has_value());

        const std::string text = out.str();
        CHECK(text.find("/quit") != std::string::npos);
        CHECK(text.find("/q")    != std::string::npos);
    }

    TEST_CASE("execute reports command count") {
        SlashCommandRegistry reg;
        register_help_cmd(reg);
        register_exit_cmd(reg);

        std::ostringstream out;
        std::istringstream in;
        MockConversation conv;
        CommandContext ctx{out, in, false, conv, reg, fs::current_path()};

        auto res = reg.lookup("help")->execute("", ctx);
        REQUIRE(res.has_value());
        CHECK(out.str().find("2 command") != std::string::npos);
    }

    TEST_CASE("execute succeeds on single-command registry") {
        SlashCommandRegistry reg;
        register_help_cmd(reg);

        std::ostringstream out;
        std::istringstream in;
        MockConversation conv;
        CommandContext ctx{out, in, false, conv, reg, fs::current_path()};

        auto res = reg.lookup("help")->execute("", ctx);
        CHECK(res.has_value());
        CHECK(out.str().find("1 command") != std::string::npos);
    }

    TEST_CASE("requires_args is false") {
        SlashCommandRegistry reg;
        register_help_cmd(reg);
        CHECK_FALSE(reg.lookup("help")->requires_args());
    }
}

// ============================================================================
// TEST SUITE: ExitCmd
// ============================================================================
TEST_SUITE("ExitCmd — /exit") {

    TEST_CASE("registers under primary name 'exit'") {
        SlashCommandRegistry reg;
        register_exit_cmd(reg);
        REQUIRE(reg.lookup("exit") != nullptr);
    }

    TEST_CASE("registers aliases 'quit' and 'q'") {
        SlashCommandRegistry reg;
        register_exit_cmd(reg);
        REQUIRE(reg.lookup("quit") != nullptr);
        REQUIRE(reg.lookup("q")    != nullptr);
        CHECK(reg.lookup("quit")->name() == "exit");
        CHECK(reg.lookup("q")->name()    == "exit");
    }

    TEST_CASE("execute sets exit_requested to true") {
        SlashCommandRegistry reg;
        register_exit_cmd(reg);

        std::ostringstream out;
        std::istringstream in;
        MockConversation conv;
        CommandContext ctx{out, in, false, conv, reg, fs::current_path()};
        CHECK_FALSE(ctx.exit_requested);

        auto res = reg.lookup("exit")->execute("", ctx);
        REQUIRE(res.has_value());
        CHECK(ctx.exit_requested);
    }

    TEST_CASE("execute writes a farewell message") {
        SlashCommandRegistry reg;
        register_exit_cmd(reg);

        std::ostringstream out;
        std::istringstream in;
        MockConversation conv;
        CommandContext ctx{out, in, false, conv, reg, fs::current_path()};

        auto res = reg.lookup("exit")->execute("", ctx);
        REQUIRE(res.has_value());
        CHECK_FALSE(out.str().empty());
    }

    TEST_CASE("alias 'q' also sets exit_requested") {
        SlashCommandRegistry reg;
        register_exit_cmd(reg);

        std::ostringstream out;
        std::istringstream in;
        MockConversation conv;
        CommandContext ctx{out, in, false, conv, reg, fs::current_path()};

        ISlashCommand* cmd = reg.lookup("q");
        REQUIRE(cmd != nullptr);
        auto res = cmd->execute("", ctx);
        REQUIRE(res.has_value());
        CHECK(ctx.exit_requested);
    }

    TEST_CASE("requires_args is false") {
        SlashCommandRegistry reg;
        register_exit_cmd(reg);
        CHECK_FALSE(reg.lookup("exit")->requires_args());
    }
}

// ============================================================================
// TEST SUITE: ClearCmd
// ============================================================================
TEST_SUITE("ClearCmd — /clear") {

    TEST_CASE("registers under primary name 'clear'") {
        SlashCommandRegistry reg;
        register_clear_cmd(reg);
        REQUIRE(reg.lookup("clear") != nullptr);
    }

    TEST_CASE("has no aliases") {
        SlashCommandRegistry reg;
        register_clear_cmd(reg);
        CHECK(reg.lookup("clear")->aliases().empty());
    }

    TEST_CASE("execute calls conversation.reset_messages()") {
        SlashCommandRegistry reg;
        register_clear_cmd(reg);

        std::ostringstream out;
        std::istringstream in;
        MockConversation conv;
        CommandContext ctx{out, in, false, conv, reg, fs::current_path()};
        CHECK_FALSE(conv.reset_called);

        auto res = reg.lookup("clear")->execute("", ctx);
        REQUIRE(res.has_value());
        CHECK(conv.reset_called);
    }

    TEST_CASE("execute writes a confirmation message") {
        SlashCommandRegistry reg;
        register_clear_cmd(reg);

        std::ostringstream out;
        std::istringstream in;
        MockConversation conv;
        CommandContext ctx{out, in, false, conv, reg, fs::current_path()};

        auto res = reg.lookup("clear")->execute("", ctx);
        REQUIRE(res.has_value());
        CHECK(out.str().find("cleared") != std::string::npos);
    }

    TEST_CASE("requires_args is false") {
        SlashCommandRegistry reg;
        register_clear_cmd(reg);
        CHECK_FALSE(reg.lookup("clear")->requires_args());
    }
}

// ============================================================================
// TEST SUITE: InitCmd
// ============================================================================
TEST_SUITE("InitCmd — /init") {

    TEST_CASE("registers under primary name 'init'") {
        SlashCommandRegistry reg;
        register_init_cmd(reg);
        REQUIRE(reg.lookup("init") != nullptr);
    }

    TEST_CASE("has no aliases") {
        SlashCommandRegistry reg;
        register_init_cmd(reg);
        CHECK(reg.lookup("init")->aliases().empty());
    }

    TEST_CASE("returns Err when cwd has no project markers") {
        const fs::path tmp = fs::temp_directory_path() / "batbox_test_no_markers";
        fs::create_directories(tmp);
        // Remove any stray marker files from prior runs.
        for (const auto& e : fs::directory_iterator(tmp)) {
            fs::remove(e.path());
        }

        SlashCommandRegistry reg;
        register_init_cmd(reg);

        std::ostringstream out;
        std::istringstream in;
        MockConversation conv;
        CommandContext ctx{out, in, false, conv, reg, tmp};

        auto res = reg.lookup("init")->execute("", ctx);
        CHECK_FALSE(res.has_value());
        CHECK(res.error().find("/init") != std::string::npos);

        fs::remove_all(tmp);
    }

    TEST_CASE("writes BATBOX.md when markers present") {
        const fs::path tmp = fs::temp_directory_path() / "batbox_test_init_write";
        fs::create_directories(tmp);
        { std::ofstream f(tmp / "CMakeLists.txt"); f << "cmake_minimum_required(VERSION 3.24)\n"; }
        fs::remove(tmp / "BATBOX.md");

        SlashCommandRegistry reg;
        register_init_cmd(reg);

        std::ostringstream out;
        std::istringstream in;
        MockConversation conv;
        CommandContext ctx{out, in, false, conv, reg, tmp};

        auto res = reg.lookup("init")->execute("", ctx);
        REQUIRE(res.has_value());
        REQUIRE(fs::exists(tmp / "BATBOX.md"));

        std::ifstream f(tmp / "BATBOX.md");
        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
        CHECK(content.find("BATBOX.md")   != std::string::npos);
        CHECK(content.find("C++ / CMake") != std::string::npos);
        CHECK(content.find("## File tree") != std::string::npos);

        fs::remove_all(tmp);
    }

    TEST_CASE("refuses to overwrite when user answers 'n'") {
        const fs::path tmp = fs::temp_directory_path() / "batbox_test_init_no_overwrite";
        fs::create_directories(tmp);
        { std::ofstream f(tmp / "package.json"); f << "{}\n"; }
        { std::ofstream f(tmp / "BATBOX.md");    f << "SENTINEL\n"; }

        SlashCommandRegistry reg;
        register_init_cmd(reg);

        std::ostringstream out;
        std::istringstream in("n\n");
        MockConversation conv;
        CommandContext ctx{out, in, false, conv, reg, tmp};

        auto res = reg.lookup("init")->execute("", ctx);
        REQUIRE(res.has_value());

        std::ifstream f(tmp / "BATBOX.md");
        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
        CHECK(content.find("SENTINEL") != std::string::npos);

        fs::remove_all(tmp);
    }

    TEST_CASE("overwrites when user answers 'y'") {
        const fs::path tmp = fs::temp_directory_path() / "batbox_test_init_overwrite";
        fs::create_directories(tmp);
        { std::ofstream f(tmp / "Cargo.toml"); f << "[package]\nname=\"test\"\n"; }
        { std::ofstream f(tmp / "BATBOX.md");  f << "OLD CONTENT\n"; }

        SlashCommandRegistry reg;
        register_init_cmd(reg);

        std::ostringstream out;
        std::istringstream in("y\n");
        MockConversation conv;
        CommandContext ctx{out, in, false, conv, reg, tmp};

        auto res = reg.lookup("init")->execute("", ctx);
        REQUIRE(res.has_value());

        std::ifstream f(tmp / "BATBOX.md");
        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
        CHECK(content.find("OLD CONTENT") == std::string::npos);
        CHECK(content.find("BATBOX.md")   != std::string::npos);
        CHECK(content.find("Rust")        != std::string::npos);

        fs::remove_all(tmp);
    }

    TEST_CASE("requires_args is false") {
        SlashCommandRegistry reg;
        register_init_cmd(reg);
        CHECK_FALSE(reg.lookup("init")->requires_args());
    }
}

// ============================================================================
// TEST SUITE: All four commands co-register without collision
// ============================================================================
TEST_SUITE("CPP S.1 — joint registration") {

    TEST_CASE("all four commands register without collision") {
        SlashCommandRegistry reg;
        register_help_cmd (reg);
        register_exit_cmd (reg);
        register_clear_cmd(reg);
        register_init_cmd (reg);

        CHECK(reg.size() == 4);
        CHECK(reg.lookup("help")  != nullptr);
        CHECK(reg.lookup("exit")  != nullptr);
        CHECK(reg.lookup("clear") != nullptr);
        CHECK(reg.lookup("init")  != nullptr);

        // Aliases resolve correctly.
        CHECK(reg.lookup("?")    != nullptr);
        CHECK(reg.lookup("quit") != nullptr);
        CHECK(reg.lookup("q")    != nullptr);
    }

    TEST_CASE("exit aliases do not collide with other primary names") {
        SlashCommandRegistry reg;
        register_help_cmd (reg);
        register_exit_cmd (reg);
        register_clear_cmd(reg);
        register_init_cmd (reg);

        ISlashCommand* q_cmd = reg.lookup("q");
        REQUIRE(q_cmd != nullptr);
        CHECK(q_cmd->name() == "exit");
    }
}
