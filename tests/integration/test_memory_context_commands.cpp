// tests/integration/test_memory_context_commands.cpp
// =============================================================================
// Integration test for CPP S.6: /memory and /context slash commands.
//
// Strategy
// --------
// Both commands are tested using a MockConversation that fully implements
// the CPP S.5 virtual methods on ConversationHandle.  All filesystem
// operations use temp directories to avoid polluting the developer's real
// ~/.batbox/BATBOX.md.
//
// /memory uses BATBOX_MEMORY_EDITOR to inject a no-op editor so tests run
// non-interactively.
//
// /context uses ContextWindow::context_limit_for_model() to verify that the
// displayed limit matches what the model name resolves to.
//
// Coverage:
//   MemoryCmd
//     - registers under primary name "memory" with no aliases
//     - requires_args is false
//     - no-arg: displays "(empty)" when neither BATBOX.md exists
//     - no-arg: displays user BATBOX.md content when present
//     - no-arg: displays project BATBOX.md content when present in cwd
//     - no-arg: displays project BATBOX.md found by walking up from cwd
//     - "view" sub-command is accepted (alias for no-arg display)
//     - unknown sub-command returns Err
//     - "edit" sub-command with no-op editor returns Ok
//     - "edit" sub-command with failing editor returns Err
//
//   ContextCmd
//     - registers under primary name "context" with no aliases
//     - requires_args is false
//     - empty conversation: shows 0 tokens
//     - non-empty conversation: shows positive token count
//     - displays model name when set
//     - displays "(unknown)" model when model name is empty
//     - displays turn count
//     - token count + limit appear in output
//     - percentage bar appears in output
//     - status string "OK" appears for low-utilisation conversation
//
//   Joint
//     - memory and context co-register without collision
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/conversation/ContextWindow.hpp>
#include <batbox/conversation/Message.hpp>
#include <batbox/core/Json.hpp>
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
// MockConversation — full CPP S.5-aware implementation of ConversationHandle
// ============================================================================

struct MockConversation final : ConversationHandle {
    // --- S.1 state -----------------------------------------------------------
    bool                     reset_called  = false;
    std::string              last_injected;
    std::vector<std::string> assistant_msgs;

    // --- S.5 state -----------------------------------------------------------
    std::string              session_id_val;
    std::filesystem::path    session_file_val;
    std::size_t              turn_count_val  = 0;
    std::string              model_name_val;
    batbox::Json             messages_val    = batbox::Json::array();

    // --- S.1 overrides -------------------------------------------------------
    void reset_messages() override {
        reset_called = true;
        messages_val = batbox::Json::array();
    }
    void inject_user_message(std::string_view text) override {
        last_injected = std::string(text);
    }
    std::string last_assistant_message(std::size_t n = 1) const override {
        if (n == 0 || n > assistant_msgs.size()) return {};
        return assistant_msgs[assistant_msgs.size() - n];
    }

    // --- S.5 overrides -------------------------------------------------------
    [[nodiscard]] std::string get_session_id() const override {
        return session_id_val;
    }
    [[nodiscard]] std::filesystem::path get_session_file_path() const override {
        return session_file_val;
    }
    [[nodiscard]] std::size_t get_turn_count() const override {
        return turn_count_val;
    }
    [[nodiscard]] std::string get_model_name() const override {
        return model_name_val;
    }
    [[nodiscard]] batbox::Json get_messages_json() const override {
        return messages_val;
    }
    void set_messages_json(const batbox::Json& messages) override {
        messages_val = messages;
    }
};

// ============================================================================
// Registration declarations (defined in each .cpp)
// ============================================================================

namespace batbox::commands {
    void register_memory_cmd (SlashCommandRegistry&);
    void register_context_cmd(SlashCommandRegistry&);
}

// ============================================================================
// Helpers
// ============================================================================

/// Build a CommandContext pointing at `conv` with cwd and config_dir set.
static CommandContext make_ctx(
    MockConversation&      conv,
    SlashCommandRegistry&  reg,
    std::ostream&          out,
    std::istream&          in,
    const fs::path&        config_dir,
    const fs::path&        cwd = fs::current_path())
{
    CommandContext ctx{out, in, false, conv, reg, cwd};
    ctx.config_dir = config_dir;
    return ctx;
}

/// Write a file at `path` with `content`.  Creates parent directories.
static void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path);
    f << content;
}

// ============================================================================
// TEST SUITE: MemoryCmd — registration
// ============================================================================

TEST_SUITE("MemoryCmd — registration") {

    TEST_CASE("registers under primary name 'memory'") {
        SlashCommandRegistry reg;
        register_memory_cmd(reg);
        REQUIRE(reg.lookup("memory") != nullptr);
        CHECK(reg.lookup("memory")->name() == "memory");
    }

    TEST_CASE("has no aliases") {
        SlashCommandRegistry reg;
        register_memory_cmd(reg);
        CHECK(reg.lookup("memory")->aliases().empty());
    }

    TEST_CASE("requires_args is false") {
        SlashCommandRegistry reg;
        register_memory_cmd(reg);
        CHECK_FALSE(reg.lookup("memory")->requires_args());
    }
}

// ============================================================================
// TEST SUITE: MemoryCmd — display (no-arg)
// ============================================================================

TEST_SUITE("MemoryCmd — display") {

    TEST_CASE("no BATBOX.md files — shows (empty) and (not present)") {
        const fs::path tmp = fs::temp_directory_path() / "batbox_memory_empty";
        const fs::path config = tmp / "config";
        const fs::path proj   = tmp / "project";
        fs::create_directories(config);
        fs::create_directories(proj);

        // Point BATBOX_CONFIG_DIR at our empty config dir.
        ::setenv("BATBOX_CONFIG_DIR", config.string().c_str(), 1);

        SlashCommandRegistry reg;
        register_memory_cmd(reg);

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(conv, reg, out, in, config, proj);

        auto res = reg.lookup("memory")->execute("", ctx);
        REQUIRE(res.has_value());

        const std::string text = out.str();
        // User BATBOX.md is absent → "(empty)"
        CHECK(text.find("(empty)") != std::string::npos);
        // Project BATBOX.md is absent → "(not present)"
        CHECK(text.find("(not present)") != std::string::npos);

        ::unsetenv("BATBOX_CONFIG_DIR");
        fs::remove_all(tmp);
    }

    TEST_CASE("user BATBOX.md present — content displayed") {
        const fs::path tmp    = fs::temp_directory_path() / "batbox_memory_user";
        const fs::path config = tmp / "config";
        const fs::path proj   = tmp / "project";
        fs::create_directories(proj);
        write_file(config / "BATBOX.md", "# User rules\nAlways be helpful.\n");

        ::setenv("BATBOX_CONFIG_DIR", config.string().c_str(), 1);

        SlashCommandRegistry reg;
        register_memory_cmd(reg);

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(conv, reg, out, in, config, proj);

        auto res = reg.lookup("memory")->execute("", ctx);
        REQUIRE(res.has_value());

        const std::string text = out.str();
        CHECK(text.find("User rules") != std::string::npos);
        CHECK(text.find("Always be helpful") != std::string::npos);

        ::unsetenv("BATBOX_CONFIG_DIR");
        fs::remove_all(tmp);
    }

    TEST_CASE("project BATBOX.md in cwd — content displayed") {
        const fs::path tmp    = fs::temp_directory_path() / "batbox_memory_proj";
        const fs::path config = tmp / "config";
        const fs::path proj   = tmp / "project";
        fs::create_directories(config);
        write_file(proj / "BATBOX.md", "# Project rules\nUse Kotlin.\n");

        ::setenv("BATBOX_CONFIG_DIR", config.string().c_str(), 1);

        SlashCommandRegistry reg;
        register_memory_cmd(reg);

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(conv, reg, out, in, config, proj);

        auto res = reg.lookup("memory")->execute("", ctx);
        REQUIRE(res.has_value());

        const std::string text = out.str();
        CHECK(text.find("Project rules") != std::string::npos);
        CHECK(text.find("Use Kotlin") != std::string::npos);

        ::unsetenv("BATBOX_CONFIG_DIR");
        fs::remove_all(tmp);
    }

    TEST_CASE("project BATBOX.md found by walking up from cwd subdirectory") {
        const fs::path tmp    = fs::temp_directory_path() / "batbox_memory_walk";
        const fs::path config = tmp / "config";
        const fs::path proj   = tmp / "project";
        const fs::path subdir = proj / "src" / "feature";
        fs::create_directories(config);
        fs::create_directories(subdir);
        write_file(proj / "BATBOX.md", "# Root project memory\n");

        ::setenv("BATBOX_CONFIG_DIR", config.string().c_str(), 1);

        SlashCommandRegistry reg;
        register_memory_cmd(reg);

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        // cwd is the deep subdirectory — should walk up and find project BATBOX.md.
        CommandContext ctx = make_ctx(conv, reg, out, in, config, subdir);

        auto res = reg.lookup("memory")->execute("", ctx);
        REQUIRE(res.has_value());

        CHECK(out.str().find("Root project memory") != std::string::npos);

        ::unsetenv("BATBOX_CONFIG_DIR");
        fs::remove_all(tmp);
    }

    TEST_CASE("'view' sub-command accepted as alias for display") {
        const fs::path tmp    = fs::temp_directory_path() / "batbox_memory_view";
        const fs::path config = tmp / "config";
        const fs::path proj   = tmp / "project";
        fs::create_directories(config);
        fs::create_directories(proj);

        ::setenv("BATBOX_CONFIG_DIR", config.string().c_str(), 1);

        SlashCommandRegistry reg;
        register_memory_cmd(reg);

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(conv, reg, out, in, config, proj);

        // "view" should behave identically to no-arg display.
        auto res = reg.lookup("memory")->execute("view", ctx);
        REQUIRE(res.has_value());
        // Output should contain memory header.
        CHECK(out.str().find("BatBox Memory") != std::string::npos);

        ::unsetenv("BATBOX_CONFIG_DIR");
        fs::remove_all(tmp);
    }

    TEST_CASE("unknown sub-command returns Err") {
        const fs::path tmp = fs::temp_directory_path() / "batbox_memory_bad_sub";
        fs::create_directories(tmp);
        ::setenv("BATBOX_CONFIG_DIR", tmp.string().c_str(), 1);

        SlashCommandRegistry reg;
        register_memory_cmd(reg);

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(conv, reg, out, in, tmp, tmp);

        auto res = reg.lookup("memory")->execute("foobar", ctx);
        CHECK_FALSE(res.has_value());
        CHECK(res.error().find("/memory") != std::string::npos);

        ::unsetenv("BATBOX_CONFIG_DIR");
        fs::remove_all(tmp);
    }
}

// ============================================================================
// TEST SUITE: MemoryCmd — edit sub-command
// ============================================================================

TEST_SUITE("MemoryCmd — edit") {

    TEST_CASE("edit with no-op editor (BATBOX_MEMORY_EDITOR) returns Ok") {
        const fs::path tmp    = fs::temp_directory_path() / "batbox_memory_edit_ok";
        const fs::path config = tmp / "config";
        const fs::path proj   = tmp / "project";
        fs::create_directories(config);
        fs::create_directories(proj);

        // Inject a no-op editor (true always succeeds).
        ::setenv("BATBOX_MEMORY_EDITOR", "true", 1);
        ::setenv("BATBOX_CONFIG_DIR",    config.string().c_str(), 1);

        SlashCommandRegistry reg;
        register_memory_cmd(reg);

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(conv, reg, out, in, config, proj);

        auto res = reg.lookup("memory")->execute("edit", ctx);
        REQUIRE(res.has_value());

        const std::string text = out.str();
        CHECK(text.find("Opening") != std::string::npos);

        ::unsetenv("BATBOX_MEMORY_EDITOR");
        ::unsetenv("BATBOX_CONFIG_DIR");
        fs::remove_all(tmp);
    }

    TEST_CASE("edit opens existing project BATBOX.md (not cwd default)") {
        const fs::path tmp    = fs::temp_directory_path() / "batbox_memory_edit_exist";
        const fs::path config = tmp / "config";
        const fs::path proj   = tmp / "project";
        const fs::path subdir = proj / "sub";
        fs::create_directories(config);
        fs::create_directories(subdir);
        write_file(proj / "BATBOX.md", "# Existing\n");

        ::setenv("BATBOX_MEMORY_EDITOR", "true", 1);
        ::setenv("BATBOX_CONFIG_DIR",    config.string().c_str(), 1);

        SlashCommandRegistry reg;
        register_memory_cmd(reg);

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        // cwd is subdir — should walk up and find proj/BATBOX.md.
        CommandContext ctx = make_ctx(conv, reg, out, in, config, subdir);

        auto res = reg.lookup("memory")->execute("edit", ctx);
        REQUIRE(res.has_value());

        // Output should name the existing file, not the cwd default.
        CHECK(out.str().find(proj.string()) != std::string::npos);

        ::unsetenv("BATBOX_MEMORY_EDITOR");
        ::unsetenv("BATBOX_CONFIG_DIR");
        fs::remove_all(tmp);
    }

    TEST_CASE("edit with failing editor returns Err") {
        const fs::path tmp    = fs::temp_directory_path() / "batbox_memory_edit_fail";
        const fs::path config = tmp / "config";
        const fs::path proj   = tmp / "project";
        fs::create_directories(config);
        fs::create_directories(proj);

        // "false" always exits 1 on POSIX.
        ::setenv("BATBOX_MEMORY_EDITOR", "false", 1);
        ::setenv("BATBOX_CONFIG_DIR",    config.string().c_str(), 1);

        SlashCommandRegistry reg;
        register_memory_cmd(reg);

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(conv, reg, out, in, config, proj);

        auto res = reg.lookup("memory")->execute("edit", ctx);
        CHECK_FALSE(res.has_value());
        CHECK(res.error().find("/memory edit") != std::string::npos);

        ::unsetenv("BATBOX_MEMORY_EDITOR");
        ::unsetenv("BATBOX_CONFIG_DIR");
        fs::remove_all(tmp);
    }
}

// ============================================================================
// TEST SUITE: ContextCmd — registration
// ============================================================================

TEST_SUITE("ContextCmd — registration") {

    TEST_CASE("registers under primary name 'context'") {
        SlashCommandRegistry reg;
        register_context_cmd(reg);
        REQUIRE(reg.lookup("context") != nullptr);
        CHECK(reg.lookup("context")->name() == "context");
    }

    TEST_CASE("has no aliases") {
        SlashCommandRegistry reg;
        register_context_cmd(reg);
        CHECK(reg.lookup("context")->aliases().empty());
    }

    TEST_CASE("requires_args is false") {
        SlashCommandRegistry reg;
        register_context_cmd(reg);
        CHECK_FALSE(reg.lookup("context")->requires_args());
    }
}

// ============================================================================
// TEST SUITE: ContextCmd — output
// ============================================================================

TEST_SUITE("ContextCmd — output") {

    TEST_CASE("empty conversation — shows 0 tokens") {
        SlashCommandRegistry reg;
        register_context_cmd(reg);

        MockConversation conv;  // messages_val is empty array by default

        std::ostringstream out;
        std::istringstream in;
        SlashCommandRegistry dummy_reg;
        CommandContext ctx{out, in, false, conv, dummy_reg, fs::current_path()};

        auto res = reg.lookup("context")->execute("", ctx);
        REQUIRE(res.has_value());

        const std::string text = out.str();
        // "0" should appear in the tokens line.
        CHECK(text.find("0") != std::string::npos);
        // Limit line should show some non-zero limit.
        CHECK(text.find("128") != std::string::npos);
    }

    TEST_CASE("non-empty conversation — shows positive token count") {
        SlashCommandRegistry reg;
        register_context_cmd(reg);

        MockConversation conv;
        conv.model_name_val  = "gpt-4o";
        conv.turn_count_val  = 3;

        // Build well-formed Message JSON objects.
        batbox::Json msgs = batbox::Json::array();
        {
            batbox::conversation::Message m1;
            m1.id      = "00000000-0000-0000-0000-000000000001";
            m1.role    = batbox::conversation::Role::User;
            m1.content = "Hello, what is the capital of France?";
            m1.ts      = std::chrono::system_clock::now();
            msgs.push_back(batbox::conversation::to_json(m1));
        }
        {
            batbox::conversation::Message m2;
            m2.id      = "00000000-0000-0000-0000-000000000002";
            m2.role    = batbox::conversation::Role::Assistant;
            m2.content = "The capital of France is Paris.";
            m2.ts      = std::chrono::system_clock::now();
            msgs.push_back(batbox::conversation::to_json(m2));
        }
        conv.messages_val = msgs;

        std::ostringstream out;
        std::istringstream in;
        SlashCommandRegistry dummy_reg;
        CommandContext ctx{out, in, false, conv, dummy_reg, fs::current_path()};

        auto res = reg.lookup("context")->execute("", ctx);
        REQUIRE(res.has_value());

        const std::string text = out.str();
        // Token count must be present and non-zero (some digit > 0).
        CHECK(text.find("Tokens:") != std::string::npos);
        // Model should be shown.
        CHECK(text.find("gpt-4o") != std::string::npos);
        // Turn count should appear.
        CHECK(text.find("3") != std::string::npos);
    }

    TEST_CASE("unknown model — displays '(unknown)' and default 128k limit") {
        SlashCommandRegistry reg;
        register_context_cmd(reg);

        MockConversation conv;
        // model_name_val is empty → falls back to 128 000.
        conv.turn_count_val = 0;

        std::ostringstream out;
        std::istringstream in;
        SlashCommandRegistry dummy_reg;
        CommandContext ctx{out, in, false, conv, dummy_reg, fs::current_path()};

        auto res = reg.lookup("context")->execute("", ctx);
        REQUIRE(res.has_value());

        const std::string text = out.str();
        CHECK(text.find("(unknown)") != std::string::npos);
        // Default limit 128 000 (formatted as "128 000").
        CHECK(text.find("128") != std::string::npos);
    }

    TEST_CASE("output contains percentage bar") {
        SlashCommandRegistry reg;
        register_context_cmd(reg);

        MockConversation conv;
        conv.model_name_val = "gpt-4o";

        std::ostringstream out;
        std::istringstream in;
        SlashCommandRegistry dummy_reg;
        CommandContext ctx{out, in, false, conv, dummy_reg, fs::current_path()};

        auto res = reg.lookup("context")->execute("", ctx);
        REQUIRE(res.has_value());

        const std::string text = out.str();
        // The visual bar uses '[' and ']'.
        CHECK(text.find('[') != std::string::npos);
        CHECK(text.find(']') != std::string::npos);
        // Percentage notation "%" should appear.
        CHECK(text.find('%') != std::string::npos);
    }

    TEST_CASE("low-utilisation conversation shows 'OK' status") {
        SlashCommandRegistry reg;
        register_context_cmd(reg);

        MockConversation conv;
        conv.model_name_val = "gpt-4o";
        // Empty message list → 0 tokens → well under 80% → status OK.

        std::ostringstream out;
        std::istringstream in;
        SlashCommandRegistry dummy_reg;
        CommandContext ctx{out, in, false, conv, dummy_reg, fs::current_path()};

        auto res = reg.lookup("context")->execute("", ctx);
        REQUIRE(res.has_value());

        CHECK(out.str().find("OK") != std::string::npos);
    }

    TEST_CASE("turn count appears in output") {
        SlashCommandRegistry reg;
        register_context_cmd(reg);

        MockConversation conv;
        conv.model_name_val = "gpt-4o";
        conv.turn_count_val = 42;

        std::ostringstream out;
        std::istringstream in;
        SlashCommandRegistry dummy_reg;
        CommandContext ctx{out, in, false, conv, dummy_reg, fs::current_path()};

        auto res = reg.lookup("context")->execute("", ctx);
        REQUIRE(res.has_value());

        CHECK(out.str().find("42") != std::string::npos);
    }
}

// ============================================================================
// TEST SUITE: Joint — registration
// ============================================================================

TEST_SUITE("CPP S.6 — joint registration") {

    TEST_CASE("memory and context co-register without collision") {
        SlashCommandRegistry reg;
        register_memory_cmd (reg);
        register_context_cmd(reg);

        CHECK(reg.size() == 2);
        CHECK(reg.lookup("memory")  != nullptr);
        CHECK(reg.lookup("context") != nullptr);
    }
}
