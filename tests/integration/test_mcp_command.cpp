// tests/integration/test_mcp_command.cpp
//
// doctest integration-test suite for CPP S.11: /mcp slash command.
//
// Strategy
// --------
// McpCmd reads from ctx.mcp_registry (nullable McpServerRegistry*).
// Tests exercise:
//   (a) null-registry mode — graceful degradation for all sub-commands
//   (b) live-registry mode — using a real McpServerRegistry with add_transport()
//       injecting mock transports to avoid network I/O.
//
// A MinimalMockTransport implements IMcpTransport with configurable healthy()
// return values and pre-canned responses for resources/list and prompts/list.
//
// Coverage:
//   - registers under primary name "mcp" with no aliases
//   - requires_args is false
//   - /mcp with no args defaults to list
//   - /mcp list shows server names and health
//   - /mcp list with no servers reports "(none configured)"
//   - /mcp restart <name> calls restart() on the registry
//   - /mcp restart with no name returns an error
//   - /mcp restart unknown name returns an error
//   - /mcp resources with no servers reports "(no MCP servers configured)"
//   - /mcp prompts with no servers reports "(no MCP servers configured)"
//   - /mcp help prints usage
//   - /mcp <unknown> returns error
//   - null registry: /mcp list degrades gracefully
//   - null registry: /mcp restart degrades gracefully

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/mcp/IMcpTransport.hpp>
#include <batbox/mcp/McpServerRegistry.hpp>
#include <batbox/repl/CommandContext.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace batbox::commands;

// ============================================================================
// Forward declaration of the registration function (defined in McpCmd.cpp)
// ============================================================================

namespace batbox::commands {
    void register_mcp_cmd(SlashCommandRegistry& registry);
}

// ============================================================================
// MinimalMockTransport — IMcpTransport that records calls and returns canned data.
// ============================================================================

class MinimalMockTransport final : public batbox::mcp::IMcpTransport {
public:
    explicit MinimalMockTransport(bool initially_healthy = true)
        : healthy_(initially_healthy) {}

    batbox::Result<void> start(batbox::CancelToken /*ct*/) override {
        healthy_ = true;
        return {};
    }

    void stop() override {
        healthy_ = false;
    }

    [[nodiscard]] bool healthy() const override { return healthy_; }

    [[nodiscard]] batbox::Result<batbox::Json> request(
        std::string method,
        batbox::Json /*params*/,
        batbox::CancelToken /*ct*/) override
    {
        if (method == "resources/list") {
            batbox::Json response;
            response["resources"] = batbox::Json::array();
            return response;
        }
        if (method == "prompts/list") {
            batbox::Json response;
            response["prompts"] = batbox::Json::array();
            return response;
        }
        if (method == "initialize") {
            batbox::Json response;
            response["capabilities"] = batbox::Json::object();
            response["serverInfo"] = {{"name", "mock"}, {"version", "0.1"}};
            return response;
        }
        return batbox::Err(std::string("MinimalMockTransport: unsupported method: ") + method);
    }

    [[nodiscard]] batbox::Result<void> notify(
        std::string /*method*/,
        batbox::Json /*params*/) override
    {
        return {};
    }

    void on_notification(
        std::function<void(std::string, batbox::Json)> /*handler*/) override {}

private:
    bool healthy_;
};

// ============================================================================
// MockConversation — minimal ConversationHandle
// ============================================================================

struct MockConversation final : ConversationHandle {
    void reset_messages() override {}
    void inject_user_message(std::string_view /*text*/) override {}
    std::string last_assistant_message(std::size_t /*n*/ = 1) const override { return {}; }
};

// ============================================================================
// Fixture helpers
// ============================================================================

static CommandContext make_ctx(std::ostringstream&          out,
                                std::istringstream&          in,
                                MockConversation&            conv,
                                SlashCommandRegistry&        reg,
                                batbox::mcp::McpServerRegistry* mcp_reg = nullptr)
{
    CommandContext ctx{out, in, false, conv, reg, {}};
    ctx.mcp_registry = mcp_reg;
    return ctx;
}

// ============================================================================
// TEST SUITE: McpCmd — registration
// ============================================================================

TEST_SUITE("McpCmd — registration") {

    TEST_CASE("registers under primary name 'mcp'") {
        SlashCommandRegistry reg;
        register_mcp_cmd(reg);
        REQUIRE(reg.lookup("mcp") != nullptr);
        CHECK(reg.lookup("mcp")->name() == "mcp");
    }

    TEST_CASE("has no aliases") {
        SlashCommandRegistry reg;
        register_mcp_cmd(reg);
        CHECK(reg.lookup("mcp")->aliases().empty());
    }

    TEST_CASE("requires_args is false") {
        SlashCommandRegistry reg;
        register_mcp_cmd(reg);
        CHECK_FALSE(reg.lookup("mcp")->requires_args());
    }

    TEST_CASE("description is non-empty") {
        SlashCommandRegistry reg;
        register_mcp_cmd(reg);
        CHECK(!reg.lookup("mcp")->description().empty());
    }

    TEST_CASE("usage includes '/mcp'") {
        SlashCommandRegistry reg;
        register_mcp_cmd(reg);
        const std::string usage(reg.lookup("mcp")->usage());
        CHECK(usage.find("/mcp") != std::string::npos);
    }
}

// ============================================================================
// TEST SUITE: McpCmd — null registry (graceful degradation)
// ============================================================================

TEST_SUITE("McpCmd — null registry") {

    TEST_CASE("list with null registry prints unavailable message") {
        SlashCommandRegistry reg;
        register_mcp_cmd(reg);

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        auto ctx = make_ctx(out, in, conv, reg, nullptr);

        auto* cmd = reg.lookup("mcp");
        REQUIRE(cmd != nullptr);
        auto result = cmd->execute("list", ctx);
        CHECK(result.has_value());
        CHECK(!out.str().empty());
    }

    TEST_CASE("no-arg with null registry prints unavailable message") {
        SlashCommandRegistry reg;
        register_mcp_cmd(reg);

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        auto ctx = make_ctx(out, in, conv, reg, nullptr);

        auto* cmd = reg.lookup("mcp");
        REQUIRE(cmd != nullptr);
        auto result = cmd->execute("", ctx);
        CHECK(result.has_value());
    }

    TEST_CASE("restart with null registry prints unavailable message") {
        SlashCommandRegistry reg;
        register_mcp_cmd(reg);

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        auto ctx = make_ctx(out, in, conv, reg, nullptr);

        auto* cmd = reg.lookup("mcp");
        REQUIRE(cmd != nullptr);
        auto result = cmd->execute("restart someserver", ctx);
        CHECK(result.has_value());
    }
}

// ============================================================================
// TEST SUITE: McpCmd — list sub-command
// ============================================================================

TEST_SUITE("McpCmd — list") {

    TEST_CASE("list with empty registry reports '(none configured)'") {
        SlashCommandRegistry reg;
        register_mcp_cmd(reg);

        batbox::mcp::McpServerRegistry mcp_reg;
        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        auto ctx = make_ctx(out, in, conv, reg, &mcp_reg);

        auto* cmd = reg.lookup("mcp");
        REQUIRE(cmd != nullptr);
        auto result = cmd->execute("list", ctx);
        CHECK(result.has_value());
        CHECK(out.str().find("none configured") != std::string::npos);
    }

    TEST_CASE("list with healthy transport shows server name") {
        SlashCommandRegistry reg;
        register_mcp_cmd(reg);

        batbox::mcp::McpServerRegistry mcp_reg;
        mcp_reg.add_transport("test-server",
            std::make_unique<MinimalMockTransport>(true));

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        auto ctx = make_ctx(out, in, conv, reg, &mcp_reg);

        auto* cmd = reg.lookup("mcp");
        REQUIRE(cmd != nullptr);
        auto result = cmd->execute("list", ctx);
        CHECK(result.has_value());
        CHECK(out.str().find("test-server") != std::string::npos);
    }

    TEST_CASE("list with unhealthy transport shows unhealthy state") {
        SlashCommandRegistry reg;
        register_mcp_cmd(reg);

        batbox::mcp::McpServerRegistry mcp_reg;
        mcp_reg.add_transport("bad-server",
            std::make_unique<MinimalMockTransport>(false));

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        auto ctx = make_ctx(out, in, conv, reg, &mcp_reg);

        auto* cmd = reg.lookup("mcp");
        REQUIRE(cmd != nullptr);
        auto result = cmd->execute("list", ctx);
        CHECK(result.has_value());
        CHECK(out.str().find("bad-server") != std::string::npos);
        CHECK(out.str().find("unhealthy") != std::string::npos);
    }

    TEST_CASE("no-arg defaults to list behaviour") {
        SlashCommandRegistry reg;
        register_mcp_cmd(reg);

        batbox::mcp::McpServerRegistry mcp_reg;
        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        auto ctx = make_ctx(out, in, conv, reg, &mcp_reg);

        auto* cmd = reg.lookup("mcp");
        REQUIRE(cmd != nullptr);
        auto result = cmd->execute("", ctx);
        CHECK(result.has_value());
        // No-arg should produce list output.
        CHECK(!out.str().empty());
    }
}

// ============================================================================
// TEST SUITE: McpCmd — restart sub-command
// ============================================================================

TEST_SUITE("McpCmd — restart") {

    TEST_CASE("restart with no name returns error") {
        SlashCommandRegistry reg;
        register_mcp_cmd(reg);

        batbox::mcp::McpServerRegistry mcp_reg;
        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        auto ctx = make_ctx(out, in, conv, reg, &mcp_reg);

        auto* cmd = reg.lookup("mcp");
        REQUIRE(cmd != nullptr);
        auto result = cmd->execute("restart", ctx);
        CHECK_FALSE(result.has_value());
        CHECK(result.error().find("server name required") != std::string::npos);
    }

    TEST_CASE("restart unknown server returns error") {
        SlashCommandRegistry reg;
        register_mcp_cmd(reg);

        batbox::mcp::McpServerRegistry mcp_reg;
        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        auto ctx = make_ctx(out, in, conv, reg, &mcp_reg);

        auto* cmd = reg.lookup("mcp");
        REQUIRE(cmd != nullptr);
        auto result = cmd->execute("restart nonexistent", ctx);
        // Should return an error since the server is not in the registry.
        CHECK_FALSE(result.has_value());
    }

    TEST_CASE("restart known server calls restart and succeeds") {
        SlashCommandRegistry reg;
        register_mcp_cmd(reg);

        batbox::mcp::McpServerRegistry mcp_reg;
        mcp_reg.add_transport("my-server",
            std::make_unique<MinimalMockTransport>(true));

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        auto ctx = make_ctx(out, in, conv, reg, &mcp_reg);

        auto* cmd = reg.lookup("mcp");
        REQUIRE(cmd != nullptr);
        auto result = cmd->execute("restart my-server", ctx);
        CHECK(result.has_value());
        CHECK(out.str().find("my-server") != std::string::npos);
    }
}

// ============================================================================
// TEST SUITE: McpCmd — resources sub-command
// ============================================================================

TEST_SUITE("McpCmd — resources") {

    TEST_CASE("resources with empty registry reports no servers configured") {
        SlashCommandRegistry reg;
        register_mcp_cmd(reg);

        batbox::mcp::McpServerRegistry mcp_reg;
        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        auto ctx = make_ctx(out, in, conv, reg, &mcp_reg);

        auto* cmd = reg.lookup("mcp");
        REQUIRE(cmd != nullptr);
        auto result = cmd->execute("resources", ctx);
        CHECK(result.has_value());
        CHECK(out.str().find("No MCP servers configured") != std::string::npos);
    }

    TEST_CASE("resources with mock transport completes without crash") {
        SlashCommandRegistry reg;
        register_mcp_cmd(reg);

        batbox::mcp::McpServerRegistry mcp_reg;
        mcp_reg.add_transport("srv",
            std::make_unique<MinimalMockTransport>(true));

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        auto ctx = make_ctx(out, in, conv, reg, &mcp_reg);

        auto* cmd = reg.lookup("mcp");
        REQUIRE(cmd != nullptr);
        // The McpClient will call resources/list via the mock transport.
        // The result may be Ok or Err depending on whether initialize was done,
        // but the command must not crash.
        auto result = cmd->execute("resources", ctx);
        // Output should be produced either way.
        CHECK(!out.str().empty());
    }
}

// ============================================================================
// TEST SUITE: McpCmd — prompts sub-command
// ============================================================================

TEST_SUITE("McpCmd — prompts") {

    TEST_CASE("prompts with empty registry reports no servers configured") {
        SlashCommandRegistry reg;
        register_mcp_cmd(reg);

        batbox::mcp::McpServerRegistry mcp_reg;
        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        auto ctx = make_ctx(out, in, conv, reg, &mcp_reg);

        auto* cmd = reg.lookup("mcp");
        REQUIRE(cmd != nullptr);
        auto result = cmd->execute("prompts", ctx);
        CHECK(result.has_value());
        CHECK(out.str().find("No MCP servers configured") != std::string::npos);
    }
}

// ============================================================================
// TEST SUITE: McpCmd — help and unknown sub-commands
// ============================================================================

TEST_SUITE("McpCmd — help and unknown") {

    TEST_CASE("help sub-command prints usage") {
        SlashCommandRegistry reg;
        register_mcp_cmd(reg);

        batbox::mcp::McpServerRegistry mcp_reg;
        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        auto ctx = make_ctx(out, in, conv, reg, &mcp_reg);

        auto* cmd = reg.lookup("mcp");
        REQUIRE(cmd != nullptr);
        auto result = cmd->execute("help", ctx);
        CHECK(result.has_value());
        CHECK(out.str().find("/mcp") != std::string::npos);
    }

    TEST_CASE("unknown sub-command returns error") {
        SlashCommandRegistry reg;
        register_mcp_cmd(reg);

        batbox::mcp::McpServerRegistry mcp_reg;
        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        auto ctx = make_ctx(out, in, conv, reg, &mcp_reg);

        auto* cmd = reg.lookup("mcp");
        REQUIRE(cmd != nullptr);
        auto result = cmd->execute("foobar", ctx);
        CHECK_FALSE(result.has_value());
        CHECK(result.error().find("foobar") != std::string::npos);
    }

    TEST_CASE("--help flag prints usage") {
        SlashCommandRegistry reg;
        register_mcp_cmd(reg);

        batbox::mcp::McpServerRegistry mcp_reg;
        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        auto ctx = make_ctx(out, in, conv, reg, &mcp_reg);

        auto* cmd = reg.lookup("mcp");
        REQUIRE(cmd != nullptr);
        auto result = cmd->execute("--help", ctx);
        CHECK(result.has_value());
        CHECK(!out.str().empty());
    }
}
