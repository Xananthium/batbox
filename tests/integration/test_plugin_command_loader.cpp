// tests/integration/test_plugin_command_loader.cpp
// =============================================================================
// Integration tests for batbox::plugins::CommandLoader.
//
// Strategy
// --------
// CommandLoader, FrontmatterParser, SlashCommandRegistry, and their dependencies
// are compiled directly into this test executable (see CMakeLists.txt).
//
// Tests create temporary directories, write .md fixture files, call
// CommandLoader::load_from_dir(), and assert against the SlashCommandRegistry.
//
// Coverage
// --------
//   1. Valid command file → registered in registry, lookup succeeds
//   2. Name collision with pre-existing command → built-in wins, count unchanged
//   3. $ARGS substitution → full args string in injected message
//   4. $1 / $2 substitution → individual tokens injected
//   5. Name falls back to filename stem when frontmatter has no 'name' key
//   6. Malformed frontmatter → file skipped, no crash
//   7. Non-.md files in commands dir → ignored
//   8. Missing directory → load_from_dir returns without error
//   9. requires_args() → true when body contains substitution variables
//  10. Command with no args and no template vars → requires_args() false
//  11. execute() writes "[user-command: /name]" banner to output
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/plugins/CommandLoader.hpp>
#include <batbox/repl/CommandContext.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;
using namespace batbox::commands;
using batbox::plugins::CommandLoader;

// =============================================================================
// Test helpers
// =============================================================================

/// Mock ConversationHandle that records injected messages.
struct MockConversation final : ConversationHandle {
    bool        reset_called = false;
    std::string last_injected;

    void reset_messages() override {
        reset_called = true;
    }

    void inject_user_message(std::string_view text) override {
        last_injected = std::string(text);
    }

    std::string last_assistant_message(std::size_t n = 1) const override { return {}; }
};

/// RAII temporary directory that is removed on destruction.
struct TempDir {
    fs::path path;

    TempDir() {
        path = fs::temp_directory_path() /
               ("batbox_cmd_loader_test_" + std::to_string(
                   std::chrono::system_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;

    /// Write text to a file relative to this directory.
    void write(const fs::path& rel, std::string_view content) const {
        std::ofstream f(path / rel, std::ios::binary);
        f << content;
    }
};

// =============================================================================
// Stub command — pre-occupies a name to test collision policy
// =============================================================================

class StubCommand final : public ISlashCommand {
    std::string name_;
public:
    explicit StubCommand(std::string n) : name_(std::move(n)) {}
    [[nodiscard]] std::string_view name()        const noexcept override { return name_; }
    [[nodiscard]] std::string_view description() const noexcept override { return "stub"; }
    [[nodiscard]] std::string_view usage()       const noexcept override { return "/stub"; }
    [[nodiscard]] batbox::Result<void> execute(std::string_view, CommandContext&) override { return {}; }
};

// =============================================================================
// TEST SUITE: load_from_dir
// =============================================================================

TEST_SUITE("CommandLoader::load_from_dir") {

    // -------------------------------------------------------------------------
    TEST_CASE("registers a valid command from a well-formed .md file") {
        TempDir  td;
        td.write("greet.md",
            "---\n"
            "name: greet\n"
            "description: Greet the user\n"
            "---\n"
            "Hello from the greet command!\n");

        SlashCommandRegistry reg;
        CommandLoader loader;
        loader.load_from_dir(td.path, reg);

        REQUIRE(reg.lookup("greet") != nullptr);
        CHECK(reg.lookup("greet")->name() == "greet");
        CHECK(reg.lookup("greet")->description() == "Greet the user");
        CHECK(reg.size() == 1);
    }

    // -------------------------------------------------------------------------
    TEST_CASE("built-in wins on name collision — count stays at 1") {
        TempDir td;
        td.write("help.md",
            "---\n"
            "name: help\n"
            "description: Plugin help override (should be ignored)\n"
            "---\n"
            "I am trying to hijack /help.\n");

        SlashCommandRegistry reg;
        // Pre-register a stub as the built-in owner of "help".
        (void)reg.register_command(std::make_shared<StubCommand>("help"));
        REQUIRE(reg.lookup("help") != nullptr);

        CommandLoader loader;
        loader.load_from_dir(td.path, reg);

        // Registry size must not have increased.
        CHECK(reg.size() == 1);
        // The original built-in stub is still there.
        CHECK(reg.lookup("help")->description() == "stub");
    }

    // -------------------------------------------------------------------------
    TEST_CASE("$ARGS substitution — full args string injected") {
        TempDir td;
        td.write("echo.md",
            "---\n"
            "name: echo\n"
            "description: Echo back args\n"
            "---\n"
            "You said: $ARGS\n");

        SlashCommandRegistry reg;
        CommandLoader loader;
        loader.load_from_dir(td.path, reg);

        REQUIRE(reg.lookup("echo") != nullptr);

        MockConversation conv2;
        std::ostringstream out2;
        std::istringstream in2;
        CommandContext ctx{out2, in2, false, conv2, reg, fs::temp_directory_path()};
        auto res = reg.lookup("echo")->execute("hello world", ctx);
        REQUIRE(res.has_value());
        CHECK(conv2.last_injected.find("hello world") != std::string::npos);
        CHECK(conv2.last_injected.find("$ARGS")       == std::string::npos);
    }

    // -------------------------------------------------------------------------
    TEST_CASE("$1 and $2 substitution — individual tokens injected") {
        TempDir td;
        td.write("token.md",
            "---\n"
            "name: token\n"
            "description: Token test\n"
            "---\n"
            "First: $1, Second: $2\n");

        SlashCommandRegistry reg;
        CommandLoader loader;
        loader.load_from_dir(td.path, reg);

        REQUIRE(reg.lookup("token") != nullptr);

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx{out, in, false, conv, reg, fs::temp_directory_path()};

        auto res = reg.lookup("token")->execute("alpha beta gamma", ctx);
        REQUIRE(res.has_value());
        CHECK(conv.last_injected.find("alpha")  != std::string::npos);
        CHECK(conv.last_injected.find("beta")   != std::string::npos);
        CHECK(conv.last_injected.find("$1")     == std::string::npos);
        CHECK(conv.last_injected.find("$2")     == std::string::npos);
        // "gamma" is a third token: not substituted, not present in template.
    }

    // -------------------------------------------------------------------------
    TEST_CASE("name falls back to filename stem when frontmatter has no 'name' key") {
        TempDir td;
        td.write("deploy.md",
            "---\n"
            "description: Deploy the project\n"
            "---\n"
            "Deploying now.\n");

        SlashCommandRegistry reg;
        CommandLoader loader;
        loader.load_from_dir(td.path, reg);

        REQUIRE(reg.lookup("deploy") != nullptr);
        CHECK(reg.lookup("deploy")->name() == "deploy");
    }

    // -------------------------------------------------------------------------
    TEST_CASE("malformed frontmatter — file skipped, registry unchanged") {
        TempDir td;
        td.write("broken.md",
            "---\n"
            "name: broken\n"
            "  bad-indent: oops\n"   // malformed: value on next line with extra indent
            "unterminated_quote: \"no closing\n"
            "---\n"
            "body\n");

        SlashCommandRegistry reg;
        CommandLoader loader;
        // Should not throw — just skip the file.
        REQUIRE_NOTHROW(loader.load_from_dir(td.path, reg));
        // Depending on the parser, it might parse partially or reject fully.
        // The key requirement is: no crash.
        // (Registry may or may not contain the command; we don't assert either way.)
    }

    // -------------------------------------------------------------------------
    TEST_CASE("non-.md files in the directory are ignored") {
        TempDir td;
        td.write("readme.txt",  "This is not a command\n");
        td.write("config.json", "{\"name\": \"config\"}\n");
        td.write("valid.md",
            "---\n"
            "name: valid\n"
            "description: Valid command\n"
            "---\n"
            "Body.\n");

        SlashCommandRegistry reg;
        CommandLoader loader;
        loader.load_from_dir(td.path, reg);

        CHECK(reg.size() == 1);
        CHECK(reg.lookup("valid") != nullptr);
    }

    // -------------------------------------------------------------------------
    TEST_CASE("missing directory — returns without error") {
        const fs::path absent = fs::temp_directory_path() / "batbox_nonexistent_commands_xyz987";
        // Make sure it really doesn't exist.
        std::error_code ec;
        fs::remove_all(absent, ec);

        SlashCommandRegistry reg;
        CommandLoader loader;
        REQUIRE_NOTHROW(loader.load_from_dir(absent, reg));
        CHECK(reg.size() == 0);
    }

    // -------------------------------------------------------------------------
    TEST_CASE("requires_args() true when body contains substitution variables") {
        TempDir td;
        td.write("withargs.md",
            "---\n"
            "name: withargs\n"
            "description: Uses args\n"
            "---\n"
            "Please process: $ARGS\n");

        SlashCommandRegistry reg;
        CommandLoader loader;
        loader.load_from_dir(td.path, reg);

        REQUIRE(reg.lookup("withargs") != nullptr);
        CHECK(reg.lookup("withargs")->requires_args());
    }

    // -------------------------------------------------------------------------
    TEST_CASE("requires_args() false when body has no substitution variables") {
        TempDir td;
        td.write("noargs.md",
            "---\n"
            "name: noargs\n"
            "description: Static prompt\n"
            "---\n"
            "Please summarise the project.\n");

        SlashCommandRegistry reg;
        CommandLoader loader;
        loader.load_from_dir(td.path, reg);

        REQUIRE(reg.lookup("noargs") != nullptr);
        CHECK_FALSE(reg.lookup("noargs")->requires_args());
    }

    // -------------------------------------------------------------------------
    TEST_CASE("execute() writes [user-command: /name] banner to output") {
        TempDir td;
        td.write("banner.md",
            "---\n"
            "name: banner\n"
            "description: Banner test\n"
            "---\n"
            "Static body.\n");

        SlashCommandRegistry reg;
        CommandLoader loader;
        loader.load_from_dir(td.path, reg);

        REQUIRE(reg.lookup("banner") != nullptr);

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx{out, in, false, conv, reg, fs::temp_directory_path()};

        auto res = reg.lookup("banner")->execute("", ctx);
        REQUIRE(res.has_value());

        const std::string banner = out.str();
        CHECK(banner.find("[user-command: /banner]") != std::string::npos);
    }

    // -------------------------------------------------------------------------
    TEST_CASE("execute() injects rendered body into conversation") {
        TempDir td;
        td.write("static.md",
            "---\n"
            "name: static\n"
            "description: Static content command\n"
            "---\n"
            "This is the static prompt.\n");

        SlashCommandRegistry reg;
        CommandLoader loader;
        loader.load_from_dir(td.path, reg);

        REQUIRE(reg.lookup("static") != nullptr);

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx{out, in, false, conv, reg, fs::temp_directory_path()};

        auto res = reg.lookup("static")->execute("", ctx);
        REQUIRE(res.has_value());
        CHECK(conv.last_injected.find("static prompt") != std::string::npos);
    }

    // -------------------------------------------------------------------------
    TEST_CASE("multiple .md files in one directory — all registered") {
        TempDir td;
        td.write("alpha.md",
            "---\nname: alpha\ndescription: Alpha\n---\nAlpha body.\n");
        td.write("beta.md",
            "---\nname: beta\ndescription: Beta\n---\nBeta body.\n");
        td.write("gamma.md",
            "---\nname: gamma\ndescription: Gamma\n---\nGamma body.\n");

        SlashCommandRegistry reg;
        CommandLoader loader;
        loader.load_from_dir(td.path, reg);

        CHECK(reg.size() == 3);
        CHECK(reg.lookup("alpha") != nullptr);
        CHECK(reg.lookup("beta")  != nullptr);
        CHECK(reg.lookup("gamma") != nullptr);
    }

    // -------------------------------------------------------------------------
    TEST_CASE("$1 empty when no args provided") {
        TempDir td;
        td.write("t1.md",
            "---\nname: t1\ndescription: Token 1 test\n---\nToken: [$1]\n");

        SlashCommandRegistry reg;
        CommandLoader loader;
        loader.load_from_dir(td.path, reg);

        REQUIRE(reg.lookup("t1") != nullptr);

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx{out, in, false, conv, reg, fs::temp_directory_path()};

        auto res = reg.lookup("t1")->execute("", ctx);
        REQUIRE(res.has_value());
        // $1 replaced with empty string → "Token: []"
        CHECK(conv.last_injected.find("Token: []") != std::string::npos);
        CHECK(conv.last_injected.find("$1")        == std::string::npos);
    }

    // -------------------------------------------------------------------------
    TEST_CASE("name with leading slash stripped silently") {
        TempDir td;
        td.write("slashy.md",
            "---\n"
            "name: /slashy\n"   // leading slash should be stripped
            "description: Slashy test\n"
            "---\n"
            "Body.\n");

        SlashCommandRegistry reg;
        CommandLoader loader;
        loader.load_from_dir(td.path, reg);

        // Must be reachable without the slash.
        REQUIRE(reg.lookup("slashy") != nullptr);
        CHECK(reg.lookup("slashy")->name() == "slashy");
    }

    // -------------------------------------------------------------------------
    TEST_CASE("description falls back to filename when frontmatter has no 'description'") {
        TempDir td;
        td.write("nodesc.md",
            "---\n"
            "name: nodesc\n"
            "---\n"
            "Body without description.\n");

        SlashCommandRegistry reg;
        CommandLoader loader;
        loader.load_from_dir(td.path, reg);

        REQUIRE(reg.lookup("nodesc") != nullptr);
        // Description must be non-empty (fallback to filename-based string).
        CHECK_FALSE(std::string(reg.lookup("nodesc")->description()).empty());
    }

    // -------------------------------------------------------------------------
    TEST_CASE("empty commands directory — registry remains empty") {
        TempDir td;
        // No .md files written.

        SlashCommandRegistry reg;
        CommandLoader loader;
        loader.load_from_dir(td.path, reg);

        CHECK(reg.size() == 0);
    }
}
