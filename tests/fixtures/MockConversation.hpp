// tests/fixtures/MockConversation.hpp
//
// Shared MockConversation fixture — PEXT3 K3 DRY directive.
//
// Provides a single canonical implementation of ConversationHandle for use
// across all test executables that need a conversation stub but do not want
// to spin up a live inference engine.
//
// Design (per Karla K3 directives):
//   - POD-style struct: all fields are public, zero-initialized, directly
//     settable by the test without any setter API.
//   - Header-only: no .cpp required; every test that includes this file
//     gets its own TU-local definition (struct is not ODR-problematic across
//     translation units because it is defined inside test executables, not
//     shared libraries).
//   - No static state, no global registry, no GoogleMock/gtest — pure doctest
//     compatible.
//   - Superset of all 17 local MockConversation variants found across
//     tests/integration/ and tests/unit/.  Extra fields are zero-initialized
//     and harmless for tests that do not use them.
//
// Usage:
//   #include "fixtures/MockConversation.hpp"
//   // (requires ${PROJECT_SOURCE_DIR}/tests on the include path)
//
//   MockConversation conv;
//   conv.assistant_messages = {"hello", "world"};
//   conv.session_id_val = "abc-123";
//   CommandContext ctx{ out, in, false, conv, reg, cwd };
//
// Migrations still pending (PEXT4 work):
//   test_agent_planning_commands.cpp, test_ide_editor_commands.cpp,
//   test_mcp_command.cpp, test_memory_context_commands.cpp,
//   test_model_config_commands.cpp, test_permissions_advisor.cpp,
//   test_plugin_command_loader.cpp, test_review_commands.cpp,
//   test_session_commands.cpp, test_status_stats_commands.cpp,
//   test_theme_output_commands.cpp, test_commands_s1.cpp,
//   test_model_cmd_env_sourcing.cpp, test_model_cmd_live_mutation.cpp

#pragma once

#include <batbox/core/Json.hpp>
#include <batbox/repl/CommandContext.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace batbox::commands {

// ---------------------------------------------------------------------------
// MockConversation
//
// Satisfies the full ConversationHandle interface (S.1 + S.5 surface).
// All state is public and directly settable by tests.
// ---------------------------------------------------------------------------

struct MockConversation final : ConversationHandle {
    // -------------------------------------------------------------------------
    // S.1 observable state — set by method calls; inspectable by tests.
    // -------------------------------------------------------------------------

    /// Becomes true when reset_messages() is called.
    bool reset_called = false;

    /// Captures the most-recent text passed to inject_user_message().
    std::string last_injected;

    /// Pre-populated assistant message history (index 0 = oldest).
    /// Tests assign this directly; last_assistant_message() reads from it.
    std::vector<std::string> assistant_messages;

    // -------------------------------------------------------------------------
    // S.5 session-accessor state — return values for get_* overrides.
    // -------------------------------------------------------------------------

    std::string           session_id_val;
    std::filesystem::path session_file_val;
    std::size_t           turn_count_val = 0;
    std::string           model_name_val;
    batbox::Json          messages_val   = batbox::Json::array();

    // -------------------------------------------------------------------------
    // S.1 overrides — pure virtuals in ConversationHandle.
    // -------------------------------------------------------------------------

    void reset_messages() override {
        reset_called = true;
        messages_val = batbox::Json::array();
    }

    void inject_user_message(std::string_view text) override {
        last_injected = std::string(text);
    }

    std::string last_assistant_message(std::size_t n = 1) const override {
        if (n == 0 || n > assistant_messages.size()) return {};
        return assistant_messages[assistant_messages.size() - n];
    }

    // -------------------------------------------------------------------------
    // S.5 overrides — non-pure in ConversationHandle; default returns empty.
    // -------------------------------------------------------------------------

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

} // namespace batbox::commands
