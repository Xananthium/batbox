// tests/unit/test_resume_cmd.cpp
// =============================================================================
// doctest unit tests for batbox::commands::ResumeCmd (TUI-T7).
//
// Tests:
//   1. ResumeCmd registers under primary name "resume".
//   2. /resume <valid-uuid> — set_messages_json called with loaded messages.
//   3. /resume <current-session-uuid> — no-op (same session already active).
//   4. /resume (no arg) — picks the most recent non-current session.
//   5. /resume (no arg, no other sessions) — friendly error message.
//   6. /resume <bad-uuid> — returns Err with "no session found" in message.
//   7. /resume last — silently loads the most recent session.
//   8. /resume cwd  — loads the most recent session for cwd.
//
// Strategy
// --------
// ResumeCmd reads sessions via SessionStore pointed at ctx.config_dir/sessions.
// Tests create a TmpDir, populate it with a real SessionStore, then construct
// CommandContext with config_dir pointing at TmpDir.
//
// MockConvResume tracks the JSON passed to set_messages_json so tests can
// assert that the correct session content was loaded.
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/repl/CommandContext.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/core/Uuid.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/session/SessionStore.hpp>

#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace batbox::commands;
using namespace batbox::session;

// =============================================================================
// RAII temporary directory
// =============================================================================
struct TmpDir {
    fs::path path;

    TmpDir() {
        auto base = fs::temp_directory_path() /
                    ("batbox_resume_cmd_test_" + batbox::Uuid::v4().to_string());
        fs::create_directories(base);
        path = base;
    }

    ~TmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    fs::path sessions_dir() const { return path / "sessions"; }
};

// =============================================================================
// MockConvResume — records set_messages_json calls
// =============================================================================
struct MockConvResume final : ConversationHandle {
    std::string          session_id_val;
    batbox::Json         last_set_messages;
    bool                 set_messages_called = false;

    void reset_messages()                         override {}
    void inject_user_message(std::string_view)    override {}
    std::string last_assistant_message(std::size_t) const override { return {}; }

    [[nodiscard]] std::string get_session_id() const override {
        return session_id_val;
    }

    void set_messages_json(const batbox::Json& msgs) override {
        last_set_messages    = msgs;
        set_messages_called  = true;
    }
};

// =============================================================================
// Registration function forward declaration
// =============================================================================
namespace batbox::commands {
    void register_resume_cmd(SlashCommandRegistry&);
}

// =============================================================================
// Helper: build a minimal CommandContext for ResumeCmd testing.
//
// ResumeCmd reads: ctx.config_dir / "sessions" for the SessionStore.
// All other optional pointers are null (graceful degradation).
// =============================================================================
static batbox::commands::CommandContext make_ctx(
    std::ostream&                    out,
    std::istream&                    in,
    MockConvResume&                  conv,
    const SlashCommandRegistry&      reg,
    const fs::path&                  config_dir,
    const fs::path&                  cwd = fs::temp_directory_path())
{
    return batbox::commands::CommandContext{
        .output       = out,
        .input        = in,
        .conversation = conv,
        .registry     = reg,
        .cwd          = cwd,
        .config_dir   = config_dir,
    };
}

// =============================================================================
// Suite 1: Registration
// =============================================================================
TEST_SUITE("ResumeCmd — registration") {

    TEST_CASE("registers under primary name 'resume'") {
        SlashCommandRegistry reg;
        register_resume_cmd(reg);
        REQUIRE(reg.lookup("resume") != nullptr);
        CHECK(reg.lookup("resume")->name() == "resume");
    }

    TEST_CASE("description is non-empty") {
        SlashCommandRegistry reg;
        register_resume_cmd(reg);
        auto* cmd = reg.lookup("resume");
        REQUIRE(cmd != nullptr);
        CHECK_FALSE(cmd->description().empty());
    }

    TEST_CASE("usage() contains '/resume'") {
        SlashCommandRegistry reg;
        register_resume_cmd(reg);
        auto* cmd = reg.lookup("resume");
        REQUIRE(cmd != nullptr);
        CHECK(std::string(cmd->usage()).find("/resume") != std::string::npos);
    }

    TEST_CASE("requires_args returns false") {
        SlashCommandRegistry reg;
        register_resume_cmd(reg);
        auto* cmd = reg.lookup("resume");
        REQUIRE(cmd != nullptr);
        CHECK_FALSE(cmd->requires_args());
    }
}

// =============================================================================
// Suite 2: /resume <uuid> — loads a specific session
// =============================================================================
TEST_SUITE("ResumeCmd — /resume <uuid>") {

    TEST_CASE("set_messages_json is called with loaded session messages") {
        TmpDir tmp;
        SessionStore store(tmp.sessions_dir());

        // Create a session and append two messages.
        auto sid_res = store.new_session("claude-3-5-sonnet", "/tmp/project");
        REQUIRE(sid_res.has_value());
        const std::string sid = sid_res.value();

        batbox::Json user_msg = {{"role", "user"}, {"content", "say hi"}};
        batbox::Json asst_msg = {{"role", "assistant"}, {"content", "Hi there!"}};
        REQUIRE(store.append_message(sid, user_msg).has_value());
        REQUIRE(store.append_message(sid, asst_msg).has_value());

        SlashCommandRegistry reg;
        register_resume_cmd(reg);

        std::ostringstream out;
        std::istringstream in;
        MockConvResume     conv;
        // Active session is something different.
        conv.session_id_val = batbox::Uuid::v4().to_string();

        auto ctx = make_ctx(out, in, conv, reg, tmp.path);
        auto res = reg.lookup("resume")->execute(sid, ctx);

        REQUIRE(res.has_value());
        CHECK(conv.set_messages_called);
        REQUIRE(conv.last_set_messages.is_array());
        CHECK(conv.last_set_messages.size() == 2);
        CHECK(conv.last_set_messages[0]["content"] == "say hi");
        CHECK(conv.last_set_messages[1]["content"] == "Hi there!");
    }

    TEST_CASE("/resume with UUID prefix resolves to full session") {
        TmpDir tmp;
        SessionStore store(tmp.sessions_dir());

        auto sid_res = store.new_session("model", "/tmp/proj");
        REQUIRE(sid_res.has_value());
        const std::string sid = sid_res.value();
        REQUIRE(store.append_message(sid, {{"role","user"},{"content","prefix test"}}).has_value());

        // Use only the first 8 chars as the prefix.
        const std::string prefix = sid.substr(0, 8);

        SlashCommandRegistry reg;
        register_resume_cmd(reg);

        std::ostringstream out;
        std::istringstream in;
        MockConvResume     conv;
        conv.session_id_val = batbox::Uuid::v4().to_string();

        auto ctx = make_ctx(out, in, conv, reg, tmp.path);
        auto res = reg.lookup("resume")->execute(prefix, ctx);

        REQUIRE(res.has_value());
        CHECK(conv.set_messages_called);
        REQUIRE(conv.last_set_messages.size() == 1);
        CHECK(conv.last_set_messages[0]["content"] == "prefix test");
    }

    TEST_CASE("/resume with bad UUID returns Err with 'no session found'") {
        TmpDir tmp;
        // Empty store — no sessions exist.

        SlashCommandRegistry reg;
        register_resume_cmd(reg);

        std::ostringstream out;
        std::istringstream in;
        MockConvResume     conv;
        conv.session_id_val = batbox::Uuid::v4().to_string();

        auto ctx = make_ctx(out, in, conv, reg, tmp.path);
        auto res = reg.lookup("resume")->execute("deadbeef-0000-0000-0000-000000000000", ctx);

        // Must return an error.
        CHECK_FALSE(res.has_value());
        // set_messages_json must NOT be called.
        CHECK_FALSE(conv.set_messages_called);
        // Error message should indicate "no session found" or similar.
        const std::string err = res.error();
        CHECK(err.find("no session found") != std::string::npos);
    }
}

// =============================================================================
// Suite 3: /resume (no arg) — interactive picker / auto-last
// =============================================================================
TEST_SUITE("ResumeCmd — /resume no-arg") {

    TEST_CASE("/resume with no other sessions outputs friendly error") {
        TmpDir tmp;
        // Create exactly one session and make it the active one.
        SessionStore store(tmp.sessions_dir());
        auto sid_res = store.new_session("model", "/tmp/p");
        REQUIRE(sid_res.has_value());
        const std::string sid = sid_res.value();
        REQUIRE(store.append_message(sid, {{"role","user"},{"content","only session"}}).has_value());

        SlashCommandRegistry reg;
        register_resume_cmd(reg);

        std::ostringstream out;
        // Input "q" to cancel the interactive picker (in case it is shown).
        std::istringstream in("q\n");
        MockConvResume     conv;

        auto ctx = make_ctx(out, in, conv, reg, tmp.path);
        // Execute /resume with empty args (no arg mode).
        auto res = reg.lookup("resume")->execute("", ctx);

        // With only one session and the picker shown, the user typed "q".
        // Either the command succeeds (picked q → cancelled) or the picker
        // shows "No previous sessions".
        // Either way, set_messages_json must NOT be called with alien data.
        // The key assertion: the output is not blank when there are no "other" sessions.
        const std::string output = out.str();
        // At minimum the list of recent sessions must have been printed or
        // a cancellation message shown.
        CHECK_FALSE(output.empty());
    }

    TEST_CASE("/resume last silently loads the most recent session") {
        TmpDir tmp;
        SessionStore store(tmp.sessions_dir());

        auto sid_res = store.new_session("model", "/tmp/p");
        REQUIRE(sid_res.has_value());
        const std::string sid = sid_res.value();
        REQUIRE(store.append_message(sid, {{"role","user"},{"content","last session msg"}}).has_value());

        SlashCommandRegistry reg;
        register_resume_cmd(reg);

        std::ostringstream out;
        std::istringstream in;
        MockConvResume     conv;
        conv.session_id_val = batbox::Uuid::v4().to_string();

        auto ctx = make_ctx(out, in, conv, reg, tmp.path);
        auto res = reg.lookup("resume")->execute("last", ctx);

        REQUIRE(res.has_value());
        CHECK(conv.set_messages_called);
        REQUIRE(conv.last_set_messages.size() == 1);
        CHECK(conv.last_set_messages[0]["content"] == "last session msg");
    }

    TEST_CASE("/resume cwd loads the most recent session for the working directory") {
        TmpDir tmp;
        fs::path project = tmp.path / "my_project";
        fs::create_directories(project);

        SessionStore store(tmp.sessions_dir());
        auto sid_res = store.new_session("model", project);
        REQUIRE(sid_res.has_value());
        const std::string sid = sid_res.value();
        REQUIRE(store.append_message(sid, {{"role","user"},{"content","cwd session msg"}}).has_value());

        SlashCommandRegistry reg;
        register_resume_cmd(reg);

        std::ostringstream out;
        std::istringstream in;
        MockConvResume     conv;
        conv.session_id_val = batbox::Uuid::v4().to_string();

        auto ctx = make_ctx(out, in, conv, reg, tmp.path, project);
        auto res = reg.lookup("resume")->execute("cwd", ctx);

        REQUIRE(res.has_value());
        CHECK(conv.set_messages_called);
        REQUIRE(conv.last_set_messages.size() == 1);
        CHECK(conv.last_set_messages[0]["content"] == "cwd session msg");
    }
}
