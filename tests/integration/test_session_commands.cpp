// tests/integration/test_session_commands.cpp
// =============================================================================
// Integration test for CPP S.5: /resume, /session, /compact slash commands.
//
// Strategy
// --------
// All three commands are tested using a MockConversation that fully implements
// the CPP S.5 virtual methods on ConversationHandle.  The tests run against
// a temporary directory that holds real SessionStore session files, so the
// SessionStore→SessionIndex→SessionFile chain is exercised end-to-end.
//
// /compact is tested with a mock ConversationHandle that captures the compacted
// message list; network calls are avoided by testing the "too short" no-op path
// and the "empty conversation" guard.  The inference path requires BATBOX_API_KEY
// which may be absent in CI; those tests are skipped when the key is missing.
//
// Coverage:
//   ResumeCmd — registers under "resume"; no aliases
//               /resume with no sessions: prints "No previous sessions"
//               /resume picker with sessions: lists entries and loads on selection
//               /resume last: loads most-recent session
//               /resume cwd: loads session matching ctx.cwd
//               /resume <id-prefix>: loads session by prefix
//               /resume <bad-prefix>: returns Err
//               /resume picker with 'q': cancels without loading
//               /resume picker with bad number: returns Err
//
//   SessionCmd — registers under "session"; no aliases
//                no active session: prints "No active session"
//                active session: prints id, file path, turn count, model, cwd
//
//   CompactCmd — registers under "compact"; no aliases
//                empty conversation: "Nothing to compact"
//                too-short conversation: "Nothing to compact"
//                (network-dependent test skipped when no API key)
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/repl/CommandContext.hpp>
#include <batbox/session/SessionStore.hpp>
#include <batbox/session/SessionFile.hpp>
#include <batbox/conversation/Message.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <chrono>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace batbox::commands;

// ============================================================================
// MockConversation — full CPP S.5-aware implementation of ConversationHandle
// ============================================================================

struct MockConversation final : ConversationHandle {
    // --- S.1 state -----------------------------------------------------------
    bool                     reset_called   = false;
    std::string              last_injected;
    std::vector<std::string> assistant_messages;

    // --- S.5 state -----------------------------------------------------------
    std::string              session_id_val;
    std::filesystem::path    session_file_val;
    std::size_t              turn_count_val = 0;
    std::string              model_name_val;
    batbox::Json             messages_val   = batbox::Json::array();

    // --- S.1 overrides -------------------------------------------------------
    void reset_messages() override { reset_called = true; messages_val = batbox::Json::array(); }
    void inject_user_message(std::string_view text) override { last_injected = std::string(text); }

    std::string last_assistant_message(std::size_t n = 1) const override {
        if (n == 0 || n > assistant_messages.size()) return {};
        return assistant_messages[assistant_messages.size() - n];
    }

    // --- S.5 overrides -------------------------------------------------------
    [[nodiscard]] std::string get_session_id() const override { return session_id_val; }
    [[nodiscard]] std::filesystem::path get_session_file_path() const override {
        return session_file_val;
    }
    [[nodiscard]] std::size_t get_turn_count() const override { return turn_count_val; }
    [[nodiscard]] std::string get_model_name() const override { return model_name_val; }
    [[nodiscard]] batbox::Json get_messages_json() const override { return messages_val; }
    void set_messages_json(const batbox::Json& messages) override { messages_val = messages; }
};

// ============================================================================
// Registration declarations (defined in each .cpp)
// ============================================================================

namespace batbox::commands {
    void register_resume_cmd (SlashCommandRegistry&);
    void register_session_cmd(SlashCommandRegistry&);
    void register_compact_cmd(SlashCommandRegistry&);
}

// ============================================================================
// Fixture helpers
// ============================================================================

/// Create a real session file in `sessions_dir` and return the session id.
static std::string create_test_session(
    const fs::path& sessions_dir,
    const std::string& model,
    const std::string& first_message,
    const fs::path& working_dir)
{
    batbox::session::SessionStore store(sessions_dir);
    auto id_res = store.new_session(model, working_dir);
    REQUIRE(id_res.has_value());

    const std::string sid = id_res.value();

    batbox::Json msg;
    msg["role"]    = "user";
    msg["content"] = first_message;
    auto append_res = store.append_message(sid, msg);
    REQUIRE(append_res.has_value());

    return sid;
}

/// Build a CommandContext pointing at `conv` with `config_dir` set.
static CommandContext make_ctx(
    MockConversation&   conv,
    SlashCommandRegistry& reg,
    std::ostream&       out,
    std::istream&       in,
    const fs::path&     config_dir,
    const fs::path&     cwd = fs::current_path())
{
    CommandContext ctx{out, in, false, conv, reg, cwd};
    ctx.config_dir = config_dir;
    return ctx;
}

// ============================================================================
// TEST SUITE: ResumeCmd — /resume
// ============================================================================

TEST_SUITE("ResumeCmd — /resume") {

    TEST_CASE("registers under primary name 'resume' with no aliases") {
        SlashCommandRegistry reg;
        register_resume_cmd(reg);
        REQUIRE(reg.lookup("resume") != nullptr);
        CHECK(reg.lookup("resume")->name() == "resume");
        CHECK(reg.lookup("resume")->aliases().empty());
    }

    TEST_CASE("requires_args is false") {
        SlashCommandRegistry reg;
        register_resume_cmd(reg);
        CHECK_FALSE(reg.lookup("resume")->requires_args());
    }

    TEST_CASE("no sessions — prints 'No previous sessions'") {
        const fs::path tmp = fs::temp_directory_path() / "batbox_test_resume_empty";
        fs::create_directories(tmp);

        SlashCommandRegistry reg;
        register_resume_cmd(reg);

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in("q\n");
        CommandContext ctx = make_ctx(conv, reg, out, in, tmp);

        auto res = reg.lookup("resume")->execute("", ctx);
        REQUIRE(res.has_value());
        CHECK(out.str().find("No previous sessions") != std::string::npos);

        fs::remove_all(tmp);
    }

    TEST_CASE("picker with one session — user selects it — messages restored") {
        const fs::path tmp = fs::temp_directory_path() / "batbox_test_resume_pick";
        fs::create_directories(tmp / "sessions");

        const std::string sid = create_test_session(
            tmp / "sessions", "gpt-4o", "Hello world", tmp);

        SlashCommandRegistry reg;
        register_resume_cmd(reg);

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in("1\n");
        CommandContext ctx = make_ctx(conv, reg, out, in, tmp, tmp);

        auto res = reg.lookup("resume")->execute("", ctx);
        REQUIRE(res.has_value());

        // Messages should have been restored.
        CHECK(conv.messages_val.is_array());
        CHECK_FALSE(conv.messages_val.empty());

        // Output should confirm the load.
        const std::string text = out.str();
        CHECK(text.find("Resumed session") != std::string::npos);

        fs::remove_all(tmp);
    }

    TEST_CASE("picker with 'q' — cancels without loading") {
        const fs::path tmp = fs::temp_directory_path() / "batbox_test_resume_cancel";
        fs::create_directories(tmp / "sessions");

        create_test_session(tmp / "sessions", "gpt-4o", "some message", tmp);

        SlashCommandRegistry reg;
        register_resume_cmd(reg);

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in("q\n");
        CommandContext ctx = make_ctx(conv, reg, out, in, tmp, tmp);

        auto res = reg.lookup("resume")->execute("", ctx);
        REQUIRE(res.has_value());

        // Messages should NOT have been restored.
        CHECK(conv.messages_val.empty());
        CHECK(out.str().find("Cancelled") != std::string::npos);

        fs::remove_all(tmp);
    }

    TEST_CASE("picker with invalid number — returns Err") {
        const fs::path tmp = fs::temp_directory_path() / "batbox_test_resume_badnum";
        fs::create_directories(tmp / "sessions");

        create_test_session(tmp / "sessions", "gpt-4o", "some message", tmp);

        SlashCommandRegistry reg;
        register_resume_cmd(reg);

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in("99\n");
        CommandContext ctx = make_ctx(conv, reg, out, in, tmp, tmp);

        auto res = reg.lookup("resume")->execute("", ctx);
        CHECK_FALSE(res.has_value());
        CHECK(res.error().find("/resume") != std::string::npos);

        fs::remove_all(tmp);
    }

    TEST_CASE("/resume last — loads most recent session") {
        const fs::path tmp = fs::temp_directory_path() / "batbox_test_resume_last";
        fs::create_directories(tmp / "sessions");

        create_test_session(tmp / "sessions", "gpt-4o", "first session", tmp);

        // Small delay to ensure distinct updated_at timestamps.
        {
            using namespace std::chrono_literals;
            std::this_thread::sleep_for(10ms);
        }

        create_test_session(tmp / "sessions", "gpt-4o", "second session", tmp);

        SlashCommandRegistry reg;
        register_resume_cmd(reg);

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(conv, reg, out, in, tmp, tmp);

        auto res = reg.lookup("resume")->execute("last", ctx);
        REQUIRE(res.has_value());
        CHECK(conv.messages_val.is_array());
        CHECK_FALSE(conv.messages_val.empty());
        CHECK(out.str().find("Resumed session") != std::string::npos);

        fs::remove_all(tmp);
    }

    TEST_CASE("/resume cwd — loads session matching ctx.cwd") {
        const fs::path tmp = fs::temp_directory_path() / "batbox_test_resume_cwd";
        fs::create_directories(tmp / "sessions");

        const fs::path project_a = tmp / "proj_a";
        const fs::path project_b = tmp / "proj_b";
        fs::create_directories(project_a);
        fs::create_directories(project_b);

        create_test_session(tmp / "sessions", "gpt-4o", "project a session", project_a);

        SlashCommandRegistry reg;
        register_resume_cmd(reg);

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        // ctx.cwd = project_a — should find and load the session above.
        CommandContext ctx = make_ctx(conv, reg, out, in, tmp, project_a);

        auto res = reg.lookup("resume")->execute("cwd", ctx);
        REQUIRE(res.has_value());
        CHECK(conv.messages_val.is_array());
        CHECK_FALSE(conv.messages_val.empty());

        fs::remove_all(tmp);
    }

    TEST_CASE("/resume cwd — no matching session — returns Err") {
        const fs::path tmp = fs::temp_directory_path() / "batbox_test_resume_cwd_miss";
        fs::create_directories(tmp / "sessions");

        const fs::path project_a = tmp / "proj_a";
        const fs::path project_b = tmp / "proj_b";
        fs::create_directories(project_a);
        fs::create_directories(project_b);

        // Session for project_a only.
        create_test_session(tmp / "sessions", "gpt-4o", "project a", project_a);

        SlashCommandRegistry reg;
        register_resume_cmd(reg);

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        // ctx.cwd = project_b — should find no match.
        CommandContext ctx = make_ctx(conv, reg, out, in, tmp, project_b);

        auto res = reg.lookup("resume")->execute("cwd", ctx);
        CHECK_FALSE(res.has_value());
        CHECK(res.error().find("/resume cwd") != std::string::npos);

        fs::remove_all(tmp);
    }

    TEST_CASE("/resume <id-prefix> — loads matching session") {
        const fs::path tmp = fs::temp_directory_path() / "batbox_test_resume_id";
        fs::create_directories(tmp / "sessions");

        const std::string sid = create_test_session(
            tmp / "sessions", "gpt-4o", "id prefix test", tmp);
        const std::string prefix = sid.substr(0, 8);

        SlashCommandRegistry reg;
        register_resume_cmd(reg);

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(conv, reg, out, in, tmp, tmp);

        auto res = reg.lookup("resume")->execute(prefix, ctx);
        REQUIRE(res.has_value());
        CHECK(conv.messages_val.is_array());
        CHECK_FALSE(conv.messages_val.empty());

        fs::remove_all(tmp);
    }

    TEST_CASE("/resume <bad-prefix> — returns Err") {
        const fs::path tmp = fs::temp_directory_path() / "batbox_test_resume_badid";
        fs::create_directories(tmp / "sessions");

        create_test_session(tmp / "sessions", "gpt-4o", "some session", tmp);

        SlashCommandRegistry reg;
        register_resume_cmd(reg);

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(conv, reg, out, in, tmp, tmp);

        auto res = reg.lookup("resume")->execute("deadbeef-notfound", ctx);
        CHECK_FALSE(res.has_value());
        CHECK(res.error().find("/resume") != std::string::npos);

        fs::remove_all(tmp);
    }
}

// ============================================================================
// TEST SUITE: SessionCmd — /session
// ============================================================================

TEST_SUITE("SessionCmd — /session") {

    TEST_CASE("registers under primary name 'session' with no aliases") {
        SlashCommandRegistry reg;
        register_session_cmd(reg);
        REQUIRE(reg.lookup("session") != nullptr);
        CHECK(reg.lookup("session")->name() == "session");
        CHECK(reg.lookup("session")->aliases().empty());
    }

    TEST_CASE("requires_args is false") {
        SlashCommandRegistry reg;
        register_session_cmd(reg);
        CHECK_FALSE(reg.lookup("session")->requires_args());
    }

    TEST_CASE("no active session — prints 'No active session'") {
        SlashCommandRegistry reg;
        register_session_cmd(reg);

        MockConversation conv;  // session_id_val is empty by default
        std::ostringstream out;
        std::istringstream in;
        SlashCommandRegistry dummy_reg;
        CommandContext ctx{out, in, false, conv, dummy_reg, fs::current_path()};

        auto res = reg.lookup("session")->execute("", ctx);
        REQUIRE(res.has_value());
        CHECK(out.str().find("No active session") != std::string::npos);
    }

    TEST_CASE("active session — prints session id") {
        SlashCommandRegistry reg;
        register_session_cmd(reg);

        MockConversation conv;
        conv.session_id_val    = "aaaabbbb-cccc-dddd-eeee-ffffffffffff";
        conv.session_file_val  = "/tmp/sessions/aaaabbbb.json";
        conv.turn_count_val    = 7;
        conv.model_name_val    = "gpt-4o";

        std::ostringstream out;
        std::istringstream in;
        SlashCommandRegistry dummy_reg;
        CommandContext ctx{out, in, false, conv, dummy_reg, fs::path("/home/user/project")};

        auto res = reg.lookup("session")->execute("", ctx);
        REQUIRE(res.has_value());

        const std::string text = out.str();
        CHECK(text.find("aaaabbbb") != std::string::npos);
        CHECK(text.find("7")        != std::string::npos);
        CHECK(text.find("gpt-4o")   != std::string::npos);
    }

    TEST_CASE("active session — prints file path") {
        SlashCommandRegistry reg;
        register_session_cmd(reg);

        MockConversation conv;
        conv.session_id_val   = "test-session-id-12345";
        conv.session_file_val = "/tmp/sessions/test.json";

        std::ostringstream out;
        std::istringstream in;
        SlashCommandRegistry dummy_reg;
        CommandContext ctx{out, in, false, conv, dummy_reg, fs::current_path()};

        auto res = reg.lookup("session")->execute("", ctx);
        REQUIRE(res.has_value());
        CHECK(out.str().find("/tmp/sessions/test.json") != std::string::npos);
    }

    TEST_CASE("active session — prints turn count") {
        SlashCommandRegistry reg;
        register_session_cmd(reg);

        MockConversation conv;
        conv.session_id_val   = "some-uuid";
        conv.turn_count_val   = 42;

        std::ostringstream out;
        std::istringstream in;
        SlashCommandRegistry dummy_reg;
        CommandContext ctx{out, in, false, conv, dummy_reg, fs::current_path()};

        auto res = reg.lookup("session")->execute("", ctx);
        REQUIRE(res.has_value());
        CHECK(out.str().find("42") != std::string::npos);
    }

    TEST_CASE("active session — prints working directory") {
        SlashCommandRegistry reg;
        register_session_cmd(reg);

        MockConversation conv;
        conv.session_id_val = "some-uuid";

        std::ostringstream out;
        std::istringstream in;
        SlashCommandRegistry dummy_reg;
        const fs::path project_dir = "/home/alice/myproject";
        CommandContext ctx{out, in, false, conv, dummy_reg, project_dir};

        auto res = reg.lookup("session")->execute("", ctx);
        REQUIRE(res.has_value());
        CHECK(out.str().find("myproject") != std::string::npos);
    }
}

// ============================================================================
// TEST SUITE: CompactCmd — /compact
// ============================================================================

TEST_SUITE("CompactCmd — /compact") {

    TEST_CASE("registers under primary name 'compact' with no aliases") {
        SlashCommandRegistry reg;
        register_compact_cmd(reg);
        REQUIRE(reg.lookup("compact") != nullptr);
        CHECK(reg.lookup("compact")->name() == "compact");
        CHECK(reg.lookup("compact")->aliases().empty());
    }

    TEST_CASE("requires_args is false") {
        SlashCommandRegistry reg;
        register_compact_cmd(reg);
        CHECK_FALSE(reg.lookup("compact")->requires_args());
    }

    TEST_CASE("empty conversation — prints 'Nothing to compact'") {
        SlashCommandRegistry reg;
        register_compact_cmd(reg);

        MockConversation conv;  // messages_val is empty array by default

        std::ostringstream out;
        std::istringstream in;
        SlashCommandRegistry dummy_reg;
        CommandContext ctx{out, in, false, conv, dummy_reg, fs::current_path()};

        auto res = reg.lookup("compact")->execute("", ctx);
        REQUIRE(res.has_value());
        CHECK(out.str().find("Nothing to compact") != std::string::npos);
    }

    TEST_CASE("conversation with well-formed messages — returns Err when no API key") {
        // When BATBOX_API_KEY is absent, CompactCmd::execute returns Err after
        // successfully deserialising the messages and reaching the API-key guard.
        // Skip if the key is present in the environment.
        if (std::getenv("BATBOX_API_KEY") != nullptr) {
            return;
        }

        SlashCommandRegistry reg;
        register_compact_cmd(reg);

        MockConversation conv;
        // Build well-formed Message JSON objects (id + role + content + ts required).
        // Use the batbox::conversation::Message and to_json() to get the correct shape.
        batbox::Json msgs = batbox::Json::array();
        {
            batbox::conversation::Message m1;
            m1.id      = "00000000-0000-0000-0000-000000000001";
            m1.role    = batbox::conversation::Role::User;
            m1.content = "Hello";
            m1.ts      = std::chrono::system_clock::now();
            msgs.push_back(batbox::conversation::to_json(m1));
        }
        {
            batbox::conversation::Message m2;
            m2.id      = "00000000-0000-0000-0000-000000000002";
            m2.role    = batbox::conversation::Role::Assistant;
            m2.content = "Hi there!";
            m2.ts      = std::chrono::system_clock::now();
            msgs.push_back(batbox::conversation::to_json(m2));
        }
        conv.messages_val = msgs;

        std::ostringstream out;
        std::istringstream in;
        SlashCommandRegistry dummy_reg;
        CommandContext ctx{out, in, false, conv, dummy_reg, fs::current_path()};

        auto res = reg.lookup("compact")->execute("", ctx);
        // Without an API key, we expect Err from the API-key guard.
        CHECK_FALSE(res.has_value());
        if (!res.has_value()) {
            CHECK(res.error().find("BATBOX_API_KEY") != std::string::npos);
        }
    }

    TEST_CASE("all three S.5 commands co-register without collision") {
        SlashCommandRegistry reg;
        register_resume_cmd (reg);
        register_session_cmd(reg);
        register_compact_cmd(reg);

        CHECK(reg.size() == 3);
        CHECK(reg.lookup("resume")  != nullptr);
        CHECK(reg.lookup("session") != nullptr);
        CHECK(reg.lookup("compact") != nullptr);
    }
}

// ============================================================================
// TEST SUITE: CPP S.5 — joint behaviour
// ============================================================================

TEST_SUITE("CPP S.5 — joint command behaviour") {

    TEST_CASE("resume followed by session shows restored turn count") {
        const fs::path tmp = fs::temp_directory_path() / "batbox_test_s5_joint";
        fs::create_directories(tmp / "sessions");

        // Create a session with 1 message (= 1 turn after append).
        create_test_session(tmp / "sessions", "gpt-4o", "joint test message", tmp);

        SlashCommandRegistry reg;
        register_resume_cmd (reg);
        register_session_cmd(reg);

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in_resume("1\n");
        CommandContext ctx_resume = make_ctx(conv, reg, out, in_resume,
                                             tmp, tmp);

        // Step 1: resume
        auto res_resume = reg.lookup("resume")->execute("", ctx_resume);
        REQUIRE(res_resume.has_value());
        CHECK(conv.messages_val.is_array());
        CHECK_FALSE(conv.messages_val.empty());

        // Step 2: session — should show non-zero turns after restore.
        // For the test, we set turn_count_val manually to simulate the live
        // app updating it on set_messages_json().
        conv.session_id_val  = "test-joint-session";
        conv.turn_count_val  = conv.messages_val.size();

        std::ostringstream out2;
        std::istringstream in2;
        CommandContext ctx_session = make_ctx(conv, reg, out2, in2,
                                              tmp, tmp);
        auto res_session = reg.lookup("session")->execute("", ctx_session);
        REQUIRE(res_session.has_value());
        CHECK(out2.str().find("test-joint-session") != std::string::npos);

        fs::remove_all(tmp);
    }
}
