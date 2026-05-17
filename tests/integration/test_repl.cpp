// tests/integration/test_repl.cpp
// =============================================================================
// Integration tests for batbox::repl::Repl (CPP 2.6).
//
// Strategy
// --------
//  • No real inference calls — tests verify prefix routing and side effects
//    only, using a real Conversation (no run_turn invoked) and a real BashTool.
//  • Tests are self-contained; all temporary files go to fs::temp_directory_path().
//
// Test cases
// ----------
//   1.  '/' prefix routes to SlashCommandRegistry::lookup + execute.
//   2.  Unknown '/' command prints "Unknown command" to output.
//   3.  '!' prefix executes a shell command via BashTool and prints output.
//   4.  '!' with no command prints usage hint.
//   5.  '#' prefix appends a note line to BATBOX.md.
//   6.  '#' appends to existing BATBOX.md.
//   7.  '#' with no text prints usage hint.
//   8.  Backslash-continuation accumulates lines, then dispatches.
//   9.  Empty line while accumulating terminates the multi-line block.
//  10.  Submitted non-blank lines are pushed to History.
//  11.  Blank lines are NOT pushed to History.
//  12.  request_exit / exit_requested work correctly.
//  13.  '/exit' command sets exit_requested via CommandContext.
//  14.  cancel() fires the cancel source (does not crash).
//  15.  on_submit_callback returns a callable that routes to handle_input.
//  16.  Bare '/' lists available commands.
//  17.  Alias resolution: /quit routes to ExitCommand.
//  18.  '#' finds BATBOX.md walking up from a subdirectory.
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

// Repl under test.
#include <batbox/repl/Repl.hpp>

// Dependencies.
#include <batbox/repl/CommandContext.hpp>
#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/config/Config.hpp>
#include <batbox/conversation/Conversation.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/inference/Client.hpp>
#include <batbox/plugins/PluginRegistry.hpp>
#include <batbox/repl/Autocomplete.hpp>
#include <batbox/repl/History.hpp>
#include <batbox/session/SessionStore.hpp>
#include <batbox/tools/BashTool.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace batbox;
using namespace batbox::repl;
using namespace batbox::commands;

// =============================================================================
// Helpers
// =============================================================================

namespace {

// ---------------------------------------------------------------------------
// TmpDir — RAII temporary directory removed on destruction.
// ---------------------------------------------------------------------------
struct TmpDir {
    fs::path path;

    TmpDir() {
        path = fs::temp_directory_path() / ("batbox_test_repl_"
            + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(path);
    }

    ~TmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// ---------------------------------------------------------------------------
// EchoCommand — echoes its args to output.
// ---------------------------------------------------------------------------
class EchoCommand final : public ISlashCommand {
public:
    std::string_view name()        const noexcept override { return "echo"; }
    std::string_view description() const noexcept override { return "Echo arguments"; }
    std::vector<std::string> aliases() const override { return {}; }
    std::string_view usage()       const noexcept override { return "/echo <text>"; }
    bool requires_args()           const noexcept override { return false; }
    CommandPhase phase()           const noexcept override { return CommandPhase::Phase1; }

    Result<void> execute(std::string_view args, CommandContext& ctx) override {
        ctx.output << "echo:" << args << "\n";
        return Result<void>{};
    }
};

// ---------------------------------------------------------------------------
// ExitCommand — sets exit_requested on the CommandContext.
// ---------------------------------------------------------------------------
class ExitCommand final : public ISlashCommand {
public:
    std::string_view name()        const noexcept override { return "exit"; }
    std::string_view description() const noexcept override { return "Exit"; }
    std::vector<std::string> aliases() const override { return {"quit"}; }
    std::string_view usage()       const noexcept override { return "/exit"; }
    bool requires_args()           const noexcept override { return false; }
    CommandPhase phase()           const noexcept override { return CommandPhase::Phase1; }

    Result<void> execute(std::string_view /*args*/, CommandContext& ctx) override {
        ctx.exit_requested = true;
        return Result<void>{};
    }
};

// ---------------------------------------------------------------------------
// ReplFixture — shared test setup.
// ---------------------------------------------------------------------------
struct ReplFixture {
    TmpDir tmp;
    batbox::config::Config            cfg;
    batbox::session::SessionStore     store;
    batbox::inference::Client         client;
    batbox::conversation::Conversation conv;

    SlashCommandRegistry              registry;
    batbox::plugins::PluginRegistry   plugin_registry;
    batbox::repl::History             history;
    batbox::repl::Autocomplete        autocomplete;
    batbox::tools::BashTool           bash_tool;

    std::ostringstream output;
    std::istringstream input_stream;

    std::unique_ptr<Repl> repl;

    explicit ReplFixture()
        : store{tmp.path}
        , client{cfg}
        , conv{client, store, cfg, tmp.path}
        , history{std::optional<fs::path>{fs::path{}}}
        , autocomplete{registry, plugin_registry}
        , bash_tool{}
    {
        cfg.api.default_model = "gpt-4o-mini";

        // Register test commands.
        (void)registry.register_command(std::make_shared<EchoCommand>());
        (void)registry.register_command(std::make_shared<ExitCommand>());

        repl = std::make_unique<Repl>(
            conv, registry, bash_tool, history, autocomplete, tmp.path);
        repl->set_output_stream(output);
        repl->set_input_stream(input_stream);
    }
};

} // namespace

// =============================================================================
// Tests
// =============================================================================

TEST_CASE("1. slash prefix dispatches to registered command") {
    ReplFixture f;
    f.repl->handle_input("/echo hello world");
    const std::string out = f.output.str();
    REQUIRE(out.find("echo:hello world") != std::string::npos);
}

TEST_CASE("2. unknown slash command prints error message") {
    ReplFixture f;
    f.repl->handle_input("/nonexistent_cmd");
    const std::string out = f.output.str();
    REQUIRE(out.find("Unknown command") != std::string::npos);
    REQUIRE(out.find("nonexistent_cmd") != std::string::npos);
}

TEST_CASE("3. bash prefix executes shell command and prints output") {
    ReplFixture f;
    f.repl->handle_input("!echo batbox_test_marker");
    const std::string out = f.output.str();
    REQUIRE(out.find("batbox_test_marker") != std::string::npos);
}

TEST_CASE("4. bash prefix with empty command prints usage hint") {
    ReplFixture f;
    f.repl->handle_input("!");
    const std::string out = f.output.str();
    REQUIRE(out.find("Usage") != std::string::npos);
}

TEST_CASE("5. note prefix appends text to BATBOX.md in cwd") {
    ReplFixture f;
    f.repl->handle_input("# This is a test note");

    const fs::path batbox_md = f.tmp.path / "BATBOX.md";
    REQUIRE(fs::exists(batbox_md));

    std::ifstream ifs(batbox_md);
    const std::string content((std::istreambuf_iterator<char>(ifs)),
                               std::istreambuf_iterator<char>());
    REQUIRE(content.find("This is a test note") != std::string::npos);

    const std::string out = f.output.str();
    REQUIRE(out.find("Note appended") != std::string::npos);
}

TEST_CASE("6. note prefix appends to existing BATBOX.md") {
    ReplFixture f;
    const fs::path batbox_md = f.tmp.path / "BATBOX.md";
    {
        std::ofstream ofs(batbox_md);
        ofs << "# Existing content\n";
    }

    f.repl->handle_input("# Second note");

    std::ifstream ifs(batbox_md);
    const std::string content((std::istreambuf_iterator<char>(ifs)),
                               std::istreambuf_iterator<char>());
    REQUIRE(content.find("Existing content") != std::string::npos);
    REQUIRE(content.find("Second note") != std::string::npos);
}

TEST_CASE("7. note prefix with empty text prints usage hint") {
    ReplFixture f;
    f.repl->handle_input("#");
    const std::string out = f.output.str();
    REQUIRE(out.find("Usage") != std::string::npos);
}

TEST_CASE("8. backslash continuation accumulates lines then dispatches") {
    ReplFixture f;

    // First continuation — no dispatch yet.
    f.repl->handle_input("/echo line1\\");
    REQUIRE(f.output.str().empty());

    // Second continuation — still no dispatch.
    f.repl->handle_input("line2\\");
    REQUIRE(f.output.str().empty());

    // Final line — terminates the block and dispatches.
    f.repl->handle_input("line3");
    const std::string out = f.output.str();
    // The assembled line "/echo line1\nline2\nline3" dispatches to /echo.
    REQUIRE(out.find("echo:") != std::string::npos);
}

TEST_CASE("9. empty line terminates multi-line accumulation") {
    ReplFixture f;

    f.repl->handle_input("/echo hello\\");
    REQUIRE(f.output.str().empty());

    // Empty line terminates the block.
    f.repl->handle_input("");
    const std::string out = f.output.str();
    REQUIRE(out.find("echo:") != std::string::npos);
}

TEST_CASE("10. submitted non-blank lines are pushed to History") {
    ReplFixture f;
    REQUIRE(f.history.size() == 0);

    f.repl->handle_input("/echo hello");
    REQUIRE(f.history.size() == 1);

    const auto entry = f.history.at(0);
    REQUIRE(entry.has_value());
    REQUIRE(*entry == "/echo hello");
}

TEST_CASE("11. blank lines are not pushed to History") {
    ReplFixture f;
    REQUIRE(f.history.size() == 0);

    f.repl->handle_input("   ");
    f.repl->handle_input("\t");
    f.repl->handle_input("");
    REQUIRE(f.history.size() == 0);
}

TEST_CASE("12. request_exit sets exit_requested flag") {
    ReplFixture f;
    REQUIRE_FALSE(f.repl->exit_requested());
    f.repl->request_exit();
    REQUIRE(f.repl->exit_requested());
}

TEST_CASE("13. slash exit command sets exit_requested on Repl") {
    ReplFixture f;
    REQUIRE_FALSE(f.repl->exit_requested());
    f.repl->handle_input("/exit");
    REQUIRE(f.repl->exit_requested());
}

TEST_CASE("14. cancel fires the cancel source without crashing") {
    ReplFixture f;
    REQUIRE_NOTHROW(f.repl->cancel());
}

TEST_CASE("15. on_submit_callback returns callable that dispatches correctly") {
    ReplFixture f;
    auto cb = f.repl->on_submit_callback();

    REQUIRE(f.history.size() == 0);
    cb("/echo via_callback");

    const std::string out = f.output.str();
    REQUIRE(out.find("echo:via_callback") != std::string::npos);
    REQUIRE(f.history.size() == 1);
}

TEST_CASE("16. bare slash lists available commands") {
    ReplFixture f;
    f.repl->handle_input("/");
    const std::string out = f.output.str();
    REQUIRE(out.find("echo") != std::string::npos);
    REQUIRE(out.find("exit") != std::string::npos);
}

TEST_CASE("17. slash alias resolves correctly (quit -> exit command)") {
    ReplFixture f;
    REQUIRE_FALSE(f.repl->exit_requested());
    f.repl->handle_input("/quit");
    REQUIRE(f.repl->exit_requested());
}

TEST_CASE("18. note prefix finds BATBOX.md in parent directory") {
    ReplFixture f;

    // Create BATBOX.md in the tmp root (parent).
    const fs::path batbox_md = f.tmp.path / "BATBOX.md";
    {
        std::ofstream ofs(batbox_md);
        ofs << "# Parent BATBOX\n";
    }

    // Create a subdirectory and build a Repl with cwd = subdir.
    const fs::path subdir = f.tmp.path / "subdir";
    fs::create_directories(subdir);

    std::ostringstream out2;
    Repl repl2(f.conv, f.registry, f.bash_tool, f.history, f.autocomplete, subdir);
    repl2.set_output_stream(out2);

    repl2.handle_input("# Child note");

    // The parent's BATBOX.md should have been extended.
    std::ifstream ifs(batbox_md);
    const std::string content((std::istreambuf_iterator<char>(ifs)),
                               std::istreambuf_iterator<char>());
    REQUIRE(content.find("Child note") != std::string::npos);
}
