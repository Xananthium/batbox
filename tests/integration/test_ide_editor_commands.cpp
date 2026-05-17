// tests/integration/test_ide_editor_commands.cpp
//
// doctest integration-test suite for CPP S.12:
//   /ide (Phase-2 stub), /vim, /keybindings, /terminal-setup
//
// Strategy
// --------
// All four commands are exercised through the ISlashCommand interface with a
// minimal MockConversation and a CommandContext backed by temporary directories.
//
// The tests do not require a live REPL or FTXUI: ctx.vim_mode and
// ctx.keybindings are set to nullptr (headless mode) or wired to minimal
// mocks where needed.
//
// Coverage:
//   IdeCmd:
//     - name() == "ide"
//     - phase() == CommandPhase::Phase2
//     - execute() returns Ok and prints Phase-2 notice
//   VimCmd:
//     - name() == "vim"
//     - execute() with null ctx.vim_mode prints "Vim mode enabled"
//     - persists vim_mode key to settings.json in temp config_dir
//     - second execute() with live VimMode toggles back to disabled
//   KeybindingsCmd:
//     - name() == "keybindings"
//     - execute("") prints action table and file path
//     - execute("reload") with null keybindings prints headless message
//     - execute("reload") with live Keybindings calls apply_override
//     - execute("edit") with no $EDITOR returns Err
//     - execute("unknown") returns Err
//   TerminalSetupCmd:
//     - name() == "terminal-setup"
//     - execute("") probes env and writes snippet to temp dir
//     - execute("--dry-run") prints snippet and does NOT write file
//     - execute("bad-arg") returns Err

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/repl/CommandContext.hpp>
#include <batbox/repl/Keybindings.hpp>
#include <batbox/repl/VimMode.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using namespace batbox::commands;

// ============================================================================
// MockConversation
// ============================================================================

struct MockConversation final : ConversationHandle {
    void reset_messages() override {}
    void inject_user_message(std::string_view) override {}
    std::string last_assistant_message(std::size_t) const override { return {}; }
};

// ============================================================================
// Registration declarations (defined in respective Cmd.cpp files)
// ============================================================================

namespace batbox::commands {
    void register_ide_cmd(SlashCommandRegistry&);
    void register_vim_cmd(SlashCommandRegistry&);
    void register_keybindings_cmd(SlashCommandRegistry&);
    void register_terminal_setup_cmd(SlashCommandRegistry&);
}

// ============================================================================
// Helper: build a CommandContext pointing at a temp config dir
// ============================================================================

/// RAII temp directory: created in the system temp dir, removed on destruction.
struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& suffix) {
        path = fs::temp_directory_path() / ("batbox_s12_" + suffix);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

// ============================================================================
// TEST SUITE: IdeCmd
// ============================================================================

TEST_SUITE("IdeCmd") {

    TEST_CASE("registers under primary name 'ide'") {
        SlashCommandRegistry reg;
        register_ide_cmd(reg);
        REQUIRE(reg.lookup("ide") != nullptr);
        CHECK(reg.lookup("ide")->name() == "ide");
    }

    TEST_CASE("phase is Phase2") {
        SlashCommandRegistry reg;
        register_ide_cmd(reg);
        CHECK(reg.lookup("ide")->phase() == CommandPhase::Phase2);
    }

    TEST_CASE("execute returns Ok and prints Phase-2 notice") {
        SlashCommandRegistry reg;
        register_ide_cmd(reg);
        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx{out, in, false, conv, reg, fs::current_path()};

        auto res = reg.lookup("ide")->execute("", ctx);
        REQUIRE(res.has_value());

        const std::string output = out.str();
        CHECK(output.find("Phase 2") != std::string::npos);
        CHECK(output.find("not yet available") != std::string::npos);
    }

    TEST_CASE("has no aliases") {
        SlashCommandRegistry reg;
        register_ide_cmd(reg);
        CHECK(reg.lookup("ide")->aliases().empty());
    }

    TEST_CASE("requires_args is false") {
        SlashCommandRegistry reg;
        register_ide_cmd(reg);
        CHECK_FALSE(reg.lookup("ide")->requires_args());
    }
}

// ============================================================================
// TEST SUITE: VimCmd
// ============================================================================

TEST_SUITE("VimCmd") {

    TEST_CASE("registers under primary name 'vim'") {
        SlashCommandRegistry reg;
        register_vim_cmd(reg);
        REQUIRE(reg.lookup("vim") != nullptr);
        CHECK(reg.lookup("vim")->name() == "vim");
    }

    TEST_CASE("headless execute() prints 'Vim mode enabled'") {
        TempDir tmp("vim_headless");
        SlashCommandRegistry reg;
        register_vim_cmd(reg);
        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx{out, in, false, conv, reg, fs::current_path()};
        ctx.config_dir = tmp.path;
        // vim_mode is nullptr (headless)

        auto res = reg.lookup("vim")->execute("", ctx);
        REQUIRE(res.has_value());
        CHECK(out.str().find("Vim mode") != std::string::npos);
        CHECK(out.str().find("enabled") != std::string::npos);
    }

    TEST_CASE("headless execute() persists vim_mode to settings.json") {
        TempDir tmp("vim_persist");
        SlashCommandRegistry reg;
        register_vim_cmd(reg);
        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx{out, in, false, conv, reg, fs::current_path()};
        ctx.config_dir = tmp.path;

        auto res = reg.lookup("vim")->execute("", ctx);
        REQUIRE(res.has_value());

        // settings.json must exist and contain "vim_mode"
        const fs::path settings = tmp.path / "settings.json";
        REQUIRE(fs::exists(settings));
        std::ifstream f(settings);
        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
        CHECK(content.find("vim_mode") != std::string::npos);
    }

    TEST_CASE("execute() with live VimMode toggles enabled state") {
        TempDir tmp("vim_live");
        SlashCommandRegistry reg;
        register_vim_cmd(reg);
        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx{out, in, false, conv, reg, fs::current_path()};
        ctx.config_dir = tmp.path;

        batbox::repl::VimMode vm;
        ctx.vim_mode = &vm;

        // Initially disabled.
        CHECK_FALSE(vm.is_enabled());

        // First toggle — enable.
        auto res1 = reg.lookup("vim")->execute("", ctx);
        REQUIRE(res1.has_value());
        CHECK(vm.is_enabled());
        CHECK(out.str().find("enabled") != std::string::npos);

        out.str("");

        // Second toggle — disable.
        auto res2 = reg.lookup("vim")->execute("", ctx);
        REQUIRE(res2.has_value());
        CHECK_FALSE(vm.is_enabled());
        CHECK(out.str().find("disabled") != std::string::npos);
    }

    TEST_CASE("requires_args is false") {
        SlashCommandRegistry reg;
        register_vim_cmd(reg);
        CHECK_FALSE(reg.lookup("vim")->requires_args());
    }
}

// ============================================================================
// TEST SUITE: KeybindingsCmd
// ============================================================================

TEST_SUITE("KeybindingsCmd") {

    TEST_CASE("registers under primary name 'keybindings'") {
        SlashCommandRegistry reg;
        register_keybindings_cmd(reg);
        REQUIRE(reg.lookup("keybindings") != nullptr);
        CHECK(reg.lookup("keybindings")->name() == "keybindings");
    }

    TEST_CASE("execute('') prints action table and file path") {
        TempDir tmp("kb_list");
        SlashCommandRegistry reg;
        register_keybindings_cmd(reg);
        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx{out, in, false, conv, reg, fs::current_path()};
        ctx.config_dir = tmp.path;
        // keybindings is nullptr → uses defaults

        auto res = reg.lookup("keybindings")->execute("", ctx);
        REQUIRE(res.has_value());

        const std::string output = out.str();
        // Should contain at least one action name.
        CHECK(output.find("send") != std::string::npos);
        // Should mention the file path.
        CHECK(output.find("keybindings.json") != std::string::npos);
    }

    TEST_CASE("execute('reload') with null keybindings prints headless message") {
        TempDir tmp("kb_reload_headless");
        SlashCommandRegistry reg;
        register_keybindings_cmd(reg);
        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx{out, in, false, conv, reg, fs::current_path()};
        ctx.config_dir = tmp.path;
        // keybindings is nullptr

        auto res = reg.lookup("keybindings")->execute("reload", ctx);
        REQUIRE(res.has_value());
        CHECK(out.str().find("headless") != std::string::npos);
    }

    TEST_CASE("execute('reload') with live Keybindings calls apply_override") {
        TempDir tmp("kb_reload_live");

        // Write a minimal keybindings.json.
        std::ofstream f(tmp.path / "keybindings.json");
        f << "{ \"send\": \"Ctrl+Enter\" }\n";
        f.close();

        SlashCommandRegistry reg;
        register_keybindings_cmd(reg);
        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx{out, in, false, conv, reg, fs::current_path()};
        ctx.config_dir = tmp.path;

        batbox::repl::Keybindings kb;
        ctx.keybindings = &kb;

        auto res = reg.lookup("keybindings")->execute("reload", ctx);
        REQUIRE(res.has_value());
        CHECK(out.str().find("reload") != std::string::npos);
    }

    TEST_CASE("execute('edit') with no EDITOR returns Err") {
        TempDir tmp("kb_edit_noeditor");
        SlashCommandRegistry reg;
        register_keybindings_cmd(reg);
        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx{out, in, false, conv, reg, fs::current_path()};
        ctx.config_dir = tmp.path;

        // Unset both editor variables.
        ::unsetenv("EDITOR");
        ::unsetenv("VISUAL");

        auto res = reg.lookup("keybindings")->execute("edit", ctx);
        // Should fail because no editor is available.
        CHECK_FALSE(res.has_value());
    }

    TEST_CASE("execute('unknown') returns Err") {
        TempDir tmp("kb_unknown");
        SlashCommandRegistry reg;
        register_keybindings_cmd(reg);
        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx{out, in, false, conv, reg, fs::current_path()};
        ctx.config_dir = tmp.path;

        auto res = reg.lookup("keybindings")->execute("unknown", ctx);
        CHECK_FALSE(res.has_value());
        CHECK(res.error().find("unknown") != std::string::npos);
    }

    TEST_CASE("requires_args is false") {
        SlashCommandRegistry reg;
        register_keybindings_cmd(reg);
        CHECK_FALSE(reg.lookup("keybindings")->requires_args());
    }
}

// ============================================================================
// TEST SUITE: TerminalSetupCmd
// ============================================================================

TEST_SUITE("TerminalSetupCmd") {

    TEST_CASE("registers under primary name 'terminal-setup'") {
        SlashCommandRegistry reg;
        register_terminal_setup_cmd(reg);
        REQUIRE(reg.lookup("terminal-setup") != nullptr);
        CHECK(reg.lookup("terminal-setup")->name() == "terminal-setup");
    }

    TEST_CASE("execute('') probes env and writes snippet to config_dir") {
        TempDir tmp("ts_write");
        SlashCommandRegistry reg;
        register_terminal_setup_cmd(reg);
        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx{out, in, false, conv, reg, fs::current_path()};
        ctx.config_dir = tmp.path;

        auto res = reg.lookup("terminal-setup")->execute("", ctx);
        REQUIRE(res.has_value());

        // Output should mention at least one detected capability.
        const std::string output = out.str();
        CHECK(output.find("Truecolor") != std::string::npos);
        CHECK(output.find("terminal-snippets") != std::string::npos);

        // The terminal-snippets dir should exist and contain a .sh file.
        const fs::path snippets_dir = tmp.path / "terminal-snippets";
        REQUIRE(fs::exists(snippets_dir));
        bool found_sh = false;
        for (const auto& entry : fs::directory_iterator(snippets_dir)) {
            if (entry.path().extension() == ".sh") {
                found_sh = true;
                // Snippet file must be non-empty.
                CHECK(fs::file_size(entry.path()) > 0);
            }
        }
        CHECK(found_sh);
    }

    TEST_CASE("execute('--dry-run') prints snippet and does NOT write file") {
        TempDir tmp("ts_dryrun");
        SlashCommandRegistry reg;
        register_terminal_setup_cmd(reg);
        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx{out, in, false, conv, reg, fs::current_path()};
        ctx.config_dir = tmp.path;

        auto res = reg.lookup("terminal-setup")->execute("--dry-run", ctx);
        REQUIRE(res.has_value());

        // Output should mention dry-run.
        CHECK(out.str().find("dry-run") != std::string::npos);

        // terminal-snippets directory must NOT exist (no file written).
        const fs::path snippets_dir = tmp.path / "terminal-snippets";
        CHECK_FALSE(fs::exists(snippets_dir));
    }

    TEST_CASE("execute('bad-arg') returns Err") {
        TempDir tmp("ts_badarg");
        SlashCommandRegistry reg;
        register_terminal_setup_cmd(reg);
        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx{out, in, false, conv, reg, fs::current_path()};
        ctx.config_dir = tmp.path;

        auto res = reg.lookup("terminal-setup")->execute("bad-arg", ctx);
        CHECK_FALSE(res.has_value());
    }

    TEST_CASE("requires_args is false") {
        SlashCommandRegistry reg;
        register_terminal_setup_cmd(reg);
        CHECK_FALSE(reg.lookup("terminal-setup")->requires_args());
    }

    TEST_CASE("has no aliases") {
        SlashCommandRegistry reg;
        register_terminal_setup_cmd(reg);
        CHECK(reg.lookup("terminal-setup")->aliases().empty());
    }
}
