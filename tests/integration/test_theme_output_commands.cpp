// tests/integration/test_theme_output_commands.cpp
//
// Integration tests for CPP S.3: /theme and /output-style slash commands.
//
// Strategy
// --------
// Both commands are compiled directly into this test executable via their
// .cpp files + SlashCommandRegistry.cpp + SettingsLoader.cpp so the test
// builds independently of the full batbox_commands or batbox_config OBJECT
// library being assembled.
//
// Tests exercise:
//   ThemeCmd  — picker lists all 5 themes; numeric + name selection;
//               empty-arg cancellation; invalid arg returns Err;
//               selection persists to a temp settings.json.
//   OutputStyleCmd — picker lists 3 styles; numeric + name selection;
//                    empty-arg cancellation; invalid arg returns Err;
//                    selection persists to settings.json; output_style
//                    value is exactly "markdown", "plain", or "auto".
//   Joint    — both commands co-register without collision; both appear
//              in the registry's primary-name lookup.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/config/SettingsLoader.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/repl/CommandContext.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using namespace batbox::commands;

// ============================================================================
// MockConversation — satisfies ConversationHandle for testing
// ============================================================================

struct MockConversation final : ConversationHandle {
    bool reset_called = false;
    std::string last_injected;
    std::vector<std::string> assistant_messages;

    void reset_messages() override { reset_called = true; }
    void inject_user_message(std::string_view text) override {
        last_injected = std::string(text);
    }
    std::string last_assistant_message(std::size_t n = 1) const override {
        if (n == 0 || n > assistant_messages.size()) return {};
        return assistant_messages[assistant_messages.size() - n];
    }
};

// ============================================================================
// Registration function declarations (defined in each .cpp)
// ============================================================================

namespace batbox::commands {
    void register_theme_cmd(SlashCommandRegistry&);
    void register_output_style_cmd(SlashCommandRegistry&);
}

// ============================================================================
// Helper: build a CommandContext with a temp config_dir
// ============================================================================

struct TestCtx {
    fs::path              tmp_dir;
    std::ostringstream    out;
    std::istringstream    in;
    MockConversation      conv;
    SlashCommandRegistry  reg;

    explicit TestCtx(std::string input_str = "")
        : tmp_dir(fs::temp_directory_path() / ("batbox_s3_test_" + std::to_string(
              std::hash<std::string>{}(__TIME__ + std::to_string(
                  reinterpret_cast<std::uintptr_t>(this))))))
        , in(std::move(input_str))
    {
        fs::create_directories(tmp_dir);
        register_theme_cmd(reg);
        register_output_style_cmd(reg);
    }

    CommandContext make_ctx() {
        CommandContext ctx{out, in, false, conv, reg, fs::current_path()};
        ctx.config_dir = tmp_dir;
        return ctx;
    }

    fs::path settings_path() const {
        return tmp_dir / "settings.json";
    }

    ~TestCtx() {
        fs::remove_all(tmp_dir);
    }
};

// ============================================================================
// TEST SUITE: ThemeCmd — /theme
// ============================================================================
TEST_SUITE("ThemeCmd — /theme") {

    TEST_CASE("registers under primary name 'theme'") {
        SlashCommandRegistry reg;
        register_theme_cmd(reg);
        REQUIRE(reg.lookup("theme") != nullptr);
        CHECK(reg.lookup("theme")->name() == "theme");
    }

    TEST_CASE("has no aliases") {
        SlashCommandRegistry reg;
        register_theme_cmd(reg);
        CHECK(reg.lookup("theme")->aliases().empty());
    }

    TEST_CASE("requires_args is false") {
        SlashCommandRegistry reg;
        register_theme_cmd(reg);
        CHECK_FALSE(reg.lookup("theme")->requires_args());
    }

    TEST_CASE("execute without args lists all 5 themes") {
        TestCtx tc("1\n");   // select first theme
        auto ctx = tc.make_ctx();
        auto res = tc.reg.lookup("theme")->execute("", ctx);
        REQUIRE(res.has_value());
        const std::string text = tc.out.str();
        // All 5 theme names must appear.
        CHECK(text.find("miss-kittin")    != std::string::npos);
        CHECK(text.find("stock-exchange") != std::string::npos);
        CHECK(text.find("frank-sinatra")  != std::string::npos);
        CHECK(text.find("monochrome")     != std::string::npos);
        CHECK(text.find("classic")        != std::string::npos);
    }

    TEST_CASE("execute shows numbered list (5 entries)") {
        TestCtx tc("\n");   // cancel
        auto ctx = tc.make_ctx();
        auto res = tc.reg.lookup("theme")->execute("", ctx);
        REQUIRE(res.has_value());
        const std::string text = tc.out.str();
        CHECK(text.find("[1]") != std::string::npos);
        CHECK(text.find("[2]") != std::string::npos);
        CHECK(text.find("[3]") != std::string::npos);
        CHECK(text.find("[4]") != std::string::npos);
        CHECK(text.find("[5]") != std::string::npos);
    }

    TEST_CASE("numeric selection 1 applies miss-kittin and persists") {
        TestCtx tc("1\n");
        auto ctx = tc.make_ctx();
        auto res = tc.reg.lookup("theme")->execute("", ctx);
        REQUIRE(res.has_value());
        // Confirmation message.
        CHECK(tc.out.str().find("miss-kittin") != std::string::npos);
        // Persisted.
        REQUIRE(fs::exists(tc.settings_path()));
        auto load_res = batbox::config::load_settings(tc.settings_path());
        REQUIRE(load_res.has_value());
        CHECK(load_res.value().theme == "miss-kittin");
    }

    TEST_CASE("numeric selection 2 applies stock-exchange and persists") {
        TestCtx tc("2\n");
        auto ctx = tc.make_ctx();
        auto res = tc.reg.lookup("theme")->execute("", ctx);
        REQUIRE(res.has_value());
        CHECK(tc.out.str().find("stock-exchange") != std::string::npos);
        auto load_res = batbox::config::load_settings(tc.settings_path());
        REQUIRE(load_res.has_value());
        CHECK(load_res.value().theme == "stock-exchange");
    }

    TEST_CASE("numeric selection 3 applies frank-sinatra and persists") {
        TestCtx tc("3\n");
        auto ctx = tc.make_ctx();
        auto res = tc.reg.lookup("theme")->execute("", ctx);
        REQUIRE(res.has_value());
        CHECK(tc.out.str().find("frank-sinatra") != std::string::npos);
        auto load_res = batbox::config::load_settings(tc.settings_path());
        REQUIRE(load_res.has_value());
        CHECK(load_res.value().theme == "frank-sinatra");
    }

    TEST_CASE("numeric selection 4 applies monochrome and persists") {
        TestCtx tc("4\n");
        auto ctx = tc.make_ctx();
        auto res = tc.reg.lookup("theme")->execute("", ctx);
        REQUIRE(res.has_value());
        CHECK(tc.out.str().find("monochrome") != std::string::npos);
        auto load_res = batbox::config::load_settings(tc.settings_path());
        REQUIRE(load_res.has_value());
        CHECK(load_res.value().theme == "monochrome");
    }

    TEST_CASE("numeric selection 5 applies classic and persists") {
        TestCtx tc("5\n");
        auto ctx = tc.make_ctx();
        auto res = tc.reg.lookup("theme")->execute("", ctx);
        REQUIRE(res.has_value());
        CHECK(tc.out.str().find("classic") != std::string::npos);
        auto load_res = batbox::config::load_settings(tc.settings_path());
        REQUIRE(load_res.has_value());
        CHECK(load_res.value().theme == "classic");
    }

    TEST_CASE("name argument applies theme directly without picker") {
        TestCtx tc;
        auto ctx = tc.make_ctx();
        auto res = tc.reg.lookup("theme")->execute("monochrome", ctx);
        REQUIRE(res.has_value());
        CHECK(tc.out.str().find("monochrome") != std::string::npos);
        // No prompt numbers in the output (direct apply skips the list).
        CHECK(tc.out.str().find("[1]") == std::string::npos);
        auto load_res = batbox::config::load_settings(tc.settings_path());
        REQUIRE(load_res.has_value());
        CHECK(load_res.value().theme == "monochrome");
    }

    TEST_CASE("empty Enter input cancels without error") {
        TestCtx tc("\n");
        auto ctx = tc.make_ctx();
        auto res = tc.reg.lookup("theme")->execute("", ctx);
        REQUIRE(res.has_value());
        CHECK(tc.out.str().find("Cancel") != std::string::npos);
        // No settings file written (no temp dir settings.json yet).
        CHECK_FALSE(fs::exists(tc.settings_path()));
    }

    TEST_CASE("invalid argument returns Err") {
        TestCtx tc;
        auto ctx = tc.make_ctx();
        auto res = tc.reg.lookup("theme")->execute("vaporwave", ctx);
        CHECK_FALSE(res.has_value());
        CHECK(res.error().find("unknown theme") != std::string::npos);
    }

    TEST_CASE("out-of-range numeric input returns Err") {
        TestCtx tc("9\n");
        auto ctx = tc.make_ctx();
        auto res = tc.reg.lookup("theme")->execute("", ctx);
        CHECK_FALSE(res.has_value());
    }

    TEST_CASE("selection overwrites previous theme in settings") {
        TestCtx tc("1\n");
        {
            auto ctx = tc.make_ctx();
            (void)tc.reg.lookup("theme")->execute("", ctx);  // apply miss-kittin
        }
        // Now overwrite with classic (by argument).
        {
            std::istringstream dummy_in;
            std::ostringstream dummy_out;
            MockConversation   mc;
            CommandContext ctx2{dummy_out, dummy_in, false, mc, tc.reg, fs::current_path()};
            ctx2.config_dir = tc.tmp_dir;
            auto res2 = tc.reg.lookup("theme")->execute("classic", ctx2);
            REQUIRE(res2.has_value());
        }
        auto load_res = batbox::config::load_settings(tc.settings_path());
        REQUIRE(load_res.has_value());
        CHECK(load_res.value().theme == "classic");
    }
}

// ============================================================================
// TEST SUITE: OutputStyleCmd — /output-style
// ============================================================================
TEST_SUITE("OutputStyleCmd — /output-style") {

    TEST_CASE("registers under primary name 'output-style'") {
        SlashCommandRegistry reg;
        register_output_style_cmd(reg);
        REQUIRE(reg.lookup("output-style") != nullptr);
        CHECK(reg.lookup("output-style")->name() == "output-style");
    }

    TEST_CASE("has no aliases") {
        SlashCommandRegistry reg;
        register_output_style_cmd(reg);
        CHECK(reg.lookup("output-style")->aliases().empty());
    }

    TEST_CASE("requires_args is false") {
        SlashCommandRegistry reg;
        register_output_style_cmd(reg);
        CHECK_FALSE(reg.lookup("output-style")->requires_args());
    }

    TEST_CASE("execute without args lists all 3 styles") {
        TestCtx tc("1\n");
        auto ctx = tc.make_ctx();
        auto res = tc.reg.lookup("output-style")->execute("", ctx);
        REQUIRE(res.has_value());
        const std::string text = tc.out.str();
        CHECK(text.find("markdown") != std::string::npos);
        CHECK(text.find("plain")    != std::string::npos);
        CHECK(text.find("auto")     != std::string::npos);
    }

    TEST_CASE("execute shows 3 numbered entries") {
        TestCtx tc("\n");
        auto ctx = tc.make_ctx();
        auto res = tc.reg.lookup("output-style")->execute("", ctx);
        REQUIRE(res.has_value());
        const std::string text = tc.out.str();
        CHECK(text.find("[1]") != std::string::npos);
        CHECK(text.find("[2]") != std::string::npos);
        CHECK(text.find("[3]") != std::string::npos);
        // Should NOT show a 4th entry.
        CHECK(text.find("[4]") == std::string::npos);
    }

    TEST_CASE("selection 1 applies markdown and persists") {
        TestCtx tc("1\n");
        auto ctx = tc.make_ctx();
        auto res = tc.reg.lookup("output-style")->execute("", ctx);
        REQUIRE(res.has_value());
        CHECK(tc.out.str().find("markdown") != std::string::npos);
        auto load_res = batbox::config::load_settings(tc.settings_path());
        REQUIRE(load_res.has_value());
        CHECK(load_res.value().output_style == "markdown");
    }

    TEST_CASE("selection 2 applies plain and persists") {
        TestCtx tc("2\n");
        auto ctx = tc.make_ctx();
        auto res = tc.reg.lookup("output-style")->execute("", ctx);
        REQUIRE(res.has_value());
        CHECK(tc.out.str().find("plain") != std::string::npos);
        auto load_res = batbox::config::load_settings(tc.settings_path());
        REQUIRE(load_res.has_value());
        CHECK(load_res.value().output_style == "plain");
    }

    TEST_CASE("selection 3 applies auto and persists") {
        TestCtx tc("3\n");
        auto ctx = tc.make_ctx();
        auto res = tc.reg.lookup("output-style")->execute("", ctx);
        REQUIRE(res.has_value());
        CHECK(tc.out.str().find("auto") != std::string::npos);
        auto load_res = batbox::config::load_settings(tc.settings_path());
        REQUIRE(load_res.has_value());
        CHECK(load_res.value().output_style == "auto");
    }

    TEST_CASE("argument 'plain' applies directly without picker") {
        TestCtx tc;
        auto ctx = tc.make_ctx();
        auto res = tc.reg.lookup("output-style")->execute("plain", ctx);
        REQUIRE(res.has_value());
        CHECK(tc.out.str().find("plain") != std::string::npos);
        CHECK(tc.out.str().find("[1]") == std::string::npos);
        auto load_res = batbox::config::load_settings(tc.settings_path());
        REQUIRE(load_res.has_value());
        CHECK(load_res.value().output_style == "plain");
    }

    TEST_CASE("argument 'auto' applies directly") {
        TestCtx tc;
        auto ctx = tc.make_ctx();
        auto res = tc.reg.lookup("output-style")->execute("auto", ctx);
        REQUIRE(res.has_value());
        CHECK(tc.out.str().find("auto") != std::string::npos);
        auto load_res = batbox::config::load_settings(tc.settings_path());
        REQUIRE(load_res.has_value());
        CHECK(load_res.value().output_style == "auto");
    }

    TEST_CASE("empty Enter input cancels without error") {
        TestCtx tc("\n");
        auto ctx = tc.make_ctx();
        auto res = tc.reg.lookup("output-style")->execute("", ctx);
        REQUIRE(res.has_value());
        CHECK(tc.out.str().find("Cancel") != std::string::npos);
        CHECK_FALSE(fs::exists(tc.settings_path()));
    }

    TEST_CASE("invalid argument returns Err") {
        TestCtx tc;
        auto ctx = tc.make_ctx();
        auto res = tc.reg.lookup("output-style")->execute("rich-text", ctx);
        CHECK_FALSE(res.has_value());
        CHECK(res.error().find("unknown style") != std::string::npos);
    }

    TEST_CASE("out-of-range numeric input returns Err") {
        TestCtx tc("7\n");
        auto ctx = tc.make_ctx();
        auto res = tc.reg.lookup("output-style")->execute("", ctx);
        CHECK_FALSE(res.has_value());
    }

    TEST_CASE("output_style value is persisted as exact string 'markdown'") {
        TestCtx tc;
        auto ctx = tc.make_ctx();
        (void)tc.reg.lookup("output-style")->execute("markdown", ctx);
        auto load_res = batbox::config::load_settings(tc.settings_path());
        REQUIRE(load_res.has_value());
        CHECK(load_res.value().output_style == "markdown");
    }

    TEST_CASE("setting output-style preserves existing theme in settings") {
        // Pre-write a settings.json with a theme already set.
        fs::path tmp = fs::temp_directory_path() / "batbox_s3_style_preserve";
        fs::create_directories(tmp);
        {
            batbox::config::Settings s;
            s.theme = "classic";
            (void)batbox::config::write_settings(tmp / "settings.json", s);
        }

        SlashCommandRegistry reg;
        register_output_style_cmd(reg);

        std::ostringstream    out;
        std::istringstream    in;
        MockConversation      conv;
        CommandContext ctx{out, in, false, conv, reg, fs::current_path()};
        ctx.config_dir = tmp;

        auto res = reg.lookup("output-style")->execute("plain", ctx);
        REQUIRE(res.has_value());

        auto load_res = batbox::config::load_settings(tmp / "settings.json");
        REQUIRE(load_res.has_value());
        CHECK(load_res.value().output_style == "plain");
        // Existing theme must not have been clobbered.
        CHECK(load_res.value().theme == "classic");

        fs::remove_all(tmp);
    }
}

// ============================================================================
// TEST SUITE: Joint registration — both commands together
// ============================================================================
TEST_SUITE("CPP S.3 — joint registration") {

    TEST_CASE("theme and output-style co-register without collision") {
        SlashCommandRegistry reg;
        register_theme_cmd(reg);
        register_output_style_cmd(reg);

        CHECK(reg.size() == 2);
        CHECK(reg.lookup("theme")        != nullptr);
        CHECK(reg.lookup("output-style") != nullptr);
    }

    TEST_CASE("theme lookup returns correct name") {
        SlashCommandRegistry reg;
        register_theme_cmd(reg);
        register_output_style_cmd(reg);
        CHECK(reg.lookup("theme")->name() == "theme");
    }

    TEST_CASE("output-style lookup returns correct name") {
        SlashCommandRegistry reg;
        register_theme_cmd(reg);
        register_output_style_cmd(reg);
        CHECK(reg.lookup("output-style")->name() == "output-style");
    }
}
