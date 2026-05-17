// tests/integration/test_copy_command.cpp
//
// doctest integration-test suite for CPP S.13: /copy slash command.
//
// Strategy
// --------
// The clipboard dispatch layer in CopyCmd honours the BATBOX_CLIPBOARD_CMD
// environment variable.  All tests inject a mock clipboard command that writes
// its stdin to a temp file, so no real pbcopy/xclip/wl-copy is required.
//
// MockConversation provides a pre-populated assistant_messages vector so tests
// can exercise Nth-from-last retrieval without a live inference engine.
//
// Coverage:
//   - /copy (no args) copies the most-recent assistant message
//   - /copy 2  copies the second-from-last assistant message
//   - /copy on empty conversation returns Err
//   - /copy N where N > message count returns Err
//   - /copy with invalid arg returns Err with usage hint
//   - Confirmation message reports correct byte count
//   - No clipboard tool available (BATBOX_CLIPBOARD_CMD="") returns Err
//   - Clipboard command that exits non-zero returns Err
//   - registers under primary name "copy" with no aliases

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/repl/CommandContext.hpp>

#include <cstdlib>
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
    std::vector<std::string> assistant_messages;  // index 0 = oldest

    void reset_messages() override { assistant_messages.clear(); }
    void inject_user_message(std::string_view /*text*/) override {}

    std::string last_assistant_message(std::size_t n = 1) const override {
        if (n == 0 || n > assistant_messages.size()) return {};
        return assistant_messages[assistant_messages.size() - n];
    }
};

// ============================================================================
// Registration declaration (defined in CopyCmd.cpp)
// ============================================================================

namespace batbox::commands {
    void register_copy_cmd(SlashCommandRegistry&);
}

// ============================================================================
// Helpers
// ============================================================================

/// Build a minimal CommandContext pointing at a MockConversation.
static std::tuple<SlashCommandRegistry, MockConversation,
                  std::ostringstream, std::istringstream>
make_fixture()
{
    return {};
}

/// Write a shell script to `path` that copies stdin to `capture_file`.
/// The script is made executable.
static void write_capture_script(const fs::path& script_path,
                                 const fs::path& capture_file)
{
    std::ofstream f(script_path);
    f << "#!/bin/sh\n";
    f << "cat > \"" << capture_file.string() << "\"\n";
    f.close();
    fs::permissions(script_path,
                    fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::add);
}

/// Write a script that unconditionally exits with status `code`.
static void write_fail_script(const fs::path& script_path, int code)
{
    std::ofstream f(script_path);
    f << "#!/bin/sh\n";
    f << "exit " << code << "\n";
    f.close();
    fs::permissions(script_path,
                    fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::add);
}

// ============================================================================
// TEST SUITE: CopyCmd — registration
// ============================================================================

TEST_SUITE("CopyCmd — registration") {

    TEST_CASE("registers under primary name 'copy'") {
        SlashCommandRegistry reg;
        register_copy_cmd(reg);
        REQUIRE(reg.lookup("copy") != nullptr);
        CHECK(reg.lookup("copy")->name() == "copy");
    }

    TEST_CASE("has no aliases") {
        SlashCommandRegistry reg;
        register_copy_cmd(reg);
        CHECK(reg.lookup("copy")->aliases().empty());
    }

    TEST_CASE("requires_args is false") {
        SlashCommandRegistry reg;
        register_copy_cmd(reg);
        CHECK_FALSE(reg.lookup("copy")->requires_args());
    }
}

// ============================================================================
// TEST SUITE: CopyCmd — clipboard dispatch (mock via BATBOX_CLIPBOARD_CMD)
// ============================================================================

TEST_SUITE("CopyCmd — clipboard dispatch") {

    TEST_CASE("/copy (no args) copies the most-recent assistant message") {
        const fs::path tmp       = fs::temp_directory_path() / "batbox_copy_t1";
        const fs::path script    = tmp / "clip.sh";
        const fs::path captured  = tmp / "captured.txt";
        fs::create_directories(tmp);
        write_capture_script(script, captured);
        ::setenv("BATBOX_CLIPBOARD_CMD", script.string().c_str(), 1);

        SlashCommandRegistry reg;
        register_copy_cmd(reg);
        MockConversation conv;
        conv.assistant_messages = { "first message", "second message", "latest message" };
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx{out, in, false, conv, reg, fs::current_path()};

        auto res = reg.lookup("copy")->execute("", ctx);
        REQUIRE(res.has_value());

        // Verify captured content equals the last message.
        std::ifstream f(captured);
        std::string got((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        CHECK(got == "latest message");

        // Verify confirmation output mentions byte count.
        CHECK(out.str().find("14")  != std::string::npos);  // len("latest message")
        CHECK(out.str().find("byte") != std::string::npos);

        ::unsetenv("BATBOX_CLIPBOARD_CMD");
        fs::remove_all(tmp);
    }

    TEST_CASE("/copy 2 copies the second-from-last assistant message") {
        const fs::path tmp      = fs::temp_directory_path() / "batbox_copy_t2";
        const fs::path script   = tmp / "clip.sh";
        const fs::path captured = tmp / "captured.txt";
        fs::create_directories(tmp);
        write_capture_script(script, captured);
        ::setenv("BATBOX_CLIPBOARD_CMD", script.string().c_str(), 1);

        SlashCommandRegistry reg;
        register_copy_cmd(reg);
        MockConversation conv;
        conv.assistant_messages = { "alpha", "beta", "gamma" };
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx{out, in, false, conv, reg, fs::current_path()};

        auto res = reg.lookup("copy")->execute("2", ctx);
        REQUIRE(res.has_value());

        std::ifstream f(captured);
        std::string got((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        CHECK(got == "beta");

        // Confirmation includes "(message 2 from last)".
        CHECK(out.str().find("message 2 from last") != std::string::npos);

        ::unsetenv("BATBOX_CLIPBOARD_CMD");
        fs::remove_all(tmp);
    }

    TEST_CASE("/copy with leading whitespace arg is parsed correctly") {
        const fs::path tmp      = fs::temp_directory_path() / "batbox_copy_t3";
        const fs::path script   = tmp / "clip.sh";
        const fs::path captured = tmp / "captured.txt";
        fs::create_directories(tmp);
        write_capture_script(script, captured);
        ::setenv("BATBOX_CLIPBOARD_CMD", script.string().c_str(), 1);

        SlashCommandRegistry reg;
        register_copy_cmd(reg);
        MockConversation conv;
        conv.assistant_messages = { "msg1", "msg2" };
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx{out, in, false, conv, reg, fs::current_path()};

        // "  1  " — whitespace on both sides.
        auto res = reg.lookup("copy")->execute("  1  ", ctx);
        REQUIRE(res.has_value());

        std::ifstream f(captured);
        std::string got((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        CHECK(got == "msg2");

        ::unsetenv("BATBOX_CLIPBOARD_CMD");
        fs::remove_all(tmp);
    }
}

// ============================================================================
// TEST SUITE: CopyCmd — error paths
// ============================================================================

TEST_SUITE("CopyCmd — error paths") {

    TEST_CASE("/copy on empty conversation returns Err") {
        ::setenv("BATBOX_CLIPBOARD_CMD", "cat", 1);

        SlashCommandRegistry reg;
        register_copy_cmd(reg);
        MockConversation conv;  // no messages
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx{out, in, false, conv, reg, fs::current_path()};

        auto res = reg.lookup("copy")->execute("", ctx);
        CHECK_FALSE(res.has_value());
        CHECK(res.error().find("no assistant") != std::string::npos);

        ::unsetenv("BATBOX_CLIPBOARD_CMD");
    }

    TEST_CASE("/copy N where N exceeds message count returns Err") {
        ::setenv("BATBOX_CLIPBOARD_CMD", "cat", 1);

        SlashCommandRegistry reg;
        register_copy_cmd(reg);
        MockConversation conv;
        conv.assistant_messages = { "only message" };
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx{out, in, false, conv, reg, fs::current_path()};

        auto res = reg.lookup("copy")->execute("5", ctx);
        CHECK_FALSE(res.has_value());
        CHECK(res.error().find("5") != std::string::npos);

        ::unsetenv("BATBOX_CLIPBOARD_CMD");
    }

    TEST_CASE("/copy with non-numeric arg returns Err with usage hint") {
        ::setenv("BATBOX_CLIPBOARD_CMD", "cat", 1);

        SlashCommandRegistry reg;
        register_copy_cmd(reg);
        MockConversation conv;
        conv.assistant_messages = { "hello" };
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx{out, in, false, conv, reg, fs::current_path()};

        auto res = reg.lookup("copy")->execute("all", ctx);
        CHECK_FALSE(res.has_value());
        CHECK(res.error().find("/copy") != std::string::npos);
        CHECK(res.error().find("Usage") != std::string::npos);

        ::unsetenv("BATBOX_CLIPBOARD_CMD");
    }

    TEST_CASE("/copy with zero arg returns Err") {
        ::setenv("BATBOX_CLIPBOARD_CMD", "cat", 1);

        SlashCommandRegistry reg;
        register_copy_cmd(reg);
        MockConversation conv;
        conv.assistant_messages = { "hello" };
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx{out, in, false, conv, reg, fs::current_path()};

        auto res = reg.lookup("copy")->execute("0", ctx);
        CHECK_FALSE(res.has_value());

        ::unsetenv("BATBOX_CLIPBOARD_CMD");
    }

    TEST_CASE("No clipboard tool available returns Err") {
        // Force empty clipboard command so detect_clipboard_cmd() returns "".
        ::setenv("BATBOX_CLIPBOARD_CMD", "", 1);

        SlashCommandRegistry reg;
        register_copy_cmd(reg);
        MockConversation conv;
        conv.assistant_messages = { "some text" };
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx{out, in, false, conv, reg, fs::current_path()};

        auto res = reg.lookup("copy")->execute("", ctx);
        CHECK_FALSE(res.has_value());
        CHECK(res.error().find("no clipboard tool") != std::string::npos);

        ::unsetenv("BATBOX_CLIPBOARD_CMD");
    }

    TEST_CASE("Clipboard command that exits non-zero returns Err") {
        const fs::path tmp    = fs::temp_directory_path() / "batbox_copy_t_fail";
        const fs::path script = tmp / "clip_fail.sh";
        fs::create_directories(tmp);
        write_fail_script(script, 1);
        ::setenv("BATBOX_CLIPBOARD_CMD", script.string().c_str(), 1);

        SlashCommandRegistry reg;
        register_copy_cmd(reg);
        MockConversation conv;
        conv.assistant_messages = { "some text" };
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx{out, in, false, conv, reg, fs::current_path()};

        auto res = reg.lookup("copy")->execute("", ctx);
        CHECK_FALSE(res.has_value());
        CHECK(res.error().find("clipboard write failed") != std::string::npos);

        ::unsetenv("BATBOX_CLIPBOARD_CMD");
        fs::remove_all(tmp);
    }
}
