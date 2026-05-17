// tests/integration/test_status_stats_commands.cpp
// =============================================================================
// Integration test for CPP S.4: /status, /stats, /usage, /cost slash commands.
//
// Strategy
// --------
// All four commands are tested against a MockConversation that fully implements
// ConversationHandle virtual methods.  The UsageTracker is a real instance;
// SidecarManager and McpServerRegistry are left null (headless mode) so no live
// processes are required.
//
// Coverage:
//   StatusCmd   — registers under "status"; no aliases
//                 reports ✓/✗ for model, session, permission mode
//                 sidecar null → "(n/a)" path
//                 mcp_registry null → "(n/a)" path
//                 tool-dep rows present (rg, python3)
//
//   StatsCmd    — registers under "stats"; no aliases
//                 shows turns from conversation.get_turn_count()
//                 shows token counts from UsageTracker::session_total()
//                 usage_tracker null → "(n/a)" paths
//                 agent_supervisor null → "(n/a)" path
//
//   UsageCmd    — registers under "usage"; no aliases
//                 shows prompt/completion/total token counts
//                 usage_tracker null → "No usage data available" path
//
//   CostCmd     — registers under "cost"; no aliases
//                 shows token counts + estimated cost
//                 zero cost → note about locally hosted / zero-cost model
//                 usage_tracker null → "No cost data available" path
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/repl/CommandContext.hpp>
#include <batbox/inference/UsageTracker.hpp>
#include <batbox/inference/ModelPricing.hpp>

#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using namespace batbox::commands;

// ============================================================================
// MockConversation — minimal ConversationHandle for command testing
// ============================================================================

struct MockConversation final : ConversationHandle {
    // S.1 state
    bool                     reset_called   = false;
    std::string              last_injected;
    std::vector<std::string> assistant_messages;

    // S.5 state
    std::string              session_id_val;
    std::filesystem::path    session_file_val;
    std::size_t              turn_count_val = 0;
    std::string              model_name_val;
    batbox::Json             messages_val   = batbox::Json::array();

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
    void register_status_cmd(SlashCommandRegistry&);
    void register_stats_cmd (SlashCommandRegistry&);
    void register_usage_cmd (SlashCommandRegistry&);
    void register_cost_cmd  (SlashCommandRegistry&);
}

// ============================================================================
// Helper: make a minimal CommandContext (all subsystem pointers null)
// ============================================================================

static CommandContext make_ctx(MockConversation&     conv,
                               SlashCommandRegistry& reg,
                               std::ostream&         out,
                               std::istream&         in)
{
    CommandContext ctx{out, in, false, conv, reg, fs::current_path()};
    // All nullable pointers default to nullptr — headless / test mode.
    return ctx;
}

// ============================================================================
// TEST SUITE: StatusCmd — /status
// ============================================================================

TEST_SUITE("StatusCmd — /status") {

    TEST_CASE("registers under primary name 'status' with no aliases") {
        SlashCommandRegistry reg;
        register_status_cmd(reg);
        REQUIRE(reg.lookup("status") != nullptr);
        CHECK(reg.lookup("status")->name() == "status");
        CHECK(reg.lookup("status")->aliases().empty());
    }

    TEST_CASE("requires_args is false") {
        SlashCommandRegistry reg;
        register_status_cmd(reg);
        CHECK_FALSE(reg.lookup("status")->requires_args());
    }

    TEST_CASE("no model — outputs cross for Model row") {
        SlashCommandRegistry reg;
        register_status_cmd(reg);

        MockConversation conv;  // model_name_val empty by default
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(conv, reg, out, in);

        auto res = reg.lookup("status")->execute("", ctx);
        REQUIRE(res.has_value());

        const std::string text = out.str();
        // Should contain the cross mark and "(none)" for model.
        CHECK(text.find("✗") != std::string::npos);
        CHECK(text.find("(none)") != std::string::npos);
    }

    TEST_CASE("model set — outputs checkmark and model name") {
        SlashCommandRegistry reg;
        register_status_cmd(reg);

        MockConversation conv;
        conv.model_name_val = "gpt-4o";
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(conv, reg, out, in);

        auto res = reg.lookup("status")->execute("", ctx);
        REQUIRE(res.has_value());

        const std::string text = out.str();
        CHECK(text.find("✓") != std::string::npos);
        CHECK(text.find("gpt-4o") != std::string::npos);
    }

    TEST_CASE("no session — session row shows (none)") {
        SlashCommandRegistry reg;
        register_status_cmd(reg);

        MockConversation conv;  // session_id_val empty
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(conv, reg, out, in);

        auto res = reg.lookup("status")->execute("", ctx);
        REQUIRE(res.has_value());
        CHECK(out.str().find("(none)") != std::string::npos);
    }

    TEST_CASE("active session — session row shows truncated UUID") {
        SlashCommandRegistry reg;
        register_status_cmd(reg);

        MockConversation conv;
        conv.session_id_val = "aabbccdd-eeff-1122-3344-556677889900";
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(conv, reg, out, in);

        auto res = reg.lookup("status")->execute("", ctx);
        REQUIRE(res.has_value());
        // First 8 chars should be present.
        CHECK(out.str().find("aabbccdd") != std::string::npos);
    }

    TEST_CASE("permission_mode_str null — shows (n/a)") {
        SlashCommandRegistry reg;
        register_status_cmd(reg);

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(conv, reg, out, in);
        // permission_mode_str is nullptr by default.

        auto res = reg.lookup("status")->execute("", ctx);
        REQUIRE(res.has_value());
        CHECK(out.str().find("(n/a)") != std::string::npos);
    }

    TEST_CASE("permission_mode_str set — shows mode value") {
        SlashCommandRegistry reg;
        register_status_cmd(reg);

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(conv, reg, out, in);
        ctx.permission_mode_str = "nuclear";

        auto res = reg.lookup("status")->execute("", ctx);
        REQUIRE(res.has_value());
        CHECK(out.str().find("nuclear") != std::string::npos);
    }

    TEST_CASE("sidecar_manager null — sidecar row shows (n/a)") {
        SlashCommandRegistry reg;
        register_status_cmd(reg);

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(conv, reg, out, in);
        // sidecar_manager is nullptr by default.

        auto res = reg.lookup("status")->execute("", ctx);
        REQUIRE(res.has_value());
        // "(n/a)" should appear at least once.
        CHECK(out.str().find("(n/a)") != std::string::npos);
    }

    TEST_CASE("mcp_registry null — MCP servers row shows (n/a)") {
        SlashCommandRegistry reg;
        register_status_cmd(reg);

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(conv, reg, out, in);

        auto res = reg.lookup("status")->execute("", ctx);
        REQUIRE(res.has_value());
        CHECK(out.str().find("(n/a)") != std::string::npos);
    }

    TEST_CASE("tool dep rows are always present") {
        SlashCommandRegistry reg;
        register_status_cmd(reg);

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(conv, reg, out, in);

        auto res = reg.lookup("status")->execute("", ctx);
        REQUIRE(res.has_value());

        const std::string text = out.str();
        CHECK(text.find("rg (ripgrep)") != std::string::npos);
        CHECK(text.find("python3")      != std::string::npos);
    }

    TEST_CASE("all four S.4 commands co-register without collision") {
        SlashCommandRegistry reg;
        register_status_cmd(reg);
        register_stats_cmd (reg);
        register_usage_cmd (reg);
        register_cost_cmd  (reg);

        CHECK(reg.size() == 4);
        CHECK(reg.lookup("status") != nullptr);
        CHECK(reg.lookup("stats")  != nullptr);
        CHECK(reg.lookup("usage")  != nullptr);
        CHECK(reg.lookup("cost")   != nullptr);
    }
}

// ============================================================================
// TEST SUITE: StatsCmd — /stats
// ============================================================================

TEST_SUITE("StatsCmd — /stats") {

    TEST_CASE("registers under primary name 'stats' with no aliases") {
        SlashCommandRegistry reg;
        register_stats_cmd(reg);
        REQUIRE(reg.lookup("stats") != nullptr);
        CHECK(reg.lookup("stats")->name() == "stats");
        CHECK(reg.lookup("stats")->aliases().empty());
    }

    TEST_CASE("requires_args is false") {
        SlashCommandRegistry reg;
        register_stats_cmd(reg);
        CHECK_FALSE(reg.lookup("stats")->requires_args());
    }

    TEST_CASE("turn count shown from conversation") {
        SlashCommandRegistry reg;
        register_stats_cmd(reg);

        MockConversation conv;
        conv.turn_count_val = 7;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(conv, reg, out, in);

        auto res = reg.lookup("stats")->execute("", ctx);
        REQUIRE(res.has_value());
        CHECK(out.str().find("7") != std::string::npos);
    }

    TEST_CASE("usage_tracker null — token rows show (n/a)") {
        SlashCommandRegistry reg;
        register_stats_cmd(reg);

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(conv, reg, out, in);
        // usage_tracker is nullptr by default.

        auto res = reg.lookup("stats")->execute("", ctx);
        REQUIRE(res.has_value());
        CHECK(out.str().find("(n/a)") != std::string::npos);
    }

    TEST_CASE("usage_tracker populated — token rows show real values") {
        SlashCommandRegistry reg;
        register_stats_cmd(reg);

        MockConversation conv;
        conv.turn_count_val = 3;

        batbox::inference::UsageTracker tracker;
        {
            batbox::inference::UsageDelta d;
            d.prompt_tokens     = 100;
            d.completion_tokens = 50;
            d.total_tokens      = 150;
            d.cost_usd          = 0.0012;
            tracker.add(d);
        }

        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(conv, reg, out, in);
        ctx.usage_tracker = &tracker;

        auto res = reg.lookup("stats")->execute("", ctx);
        REQUIRE(res.has_value());

        const std::string text = out.str();
        // Token counts should appear somewhere in the output.
        CHECK(text.find("100") != std::string::npos);
        CHECK(text.find("50")  != std::string::npos);
        CHECK(text.find("150") != std::string::npos);
    }

    TEST_CASE("agent_supervisor null — agents spawned shows (n/a)") {
        SlashCommandRegistry reg;
        register_stats_cmd(reg);

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(conv, reg, out, in);

        auto res = reg.lookup("stats")->execute("", ctx);
        REQUIRE(res.has_value());
        CHECK(out.str().find("(n/a)") != std::string::npos);
    }
}

// ============================================================================
// TEST SUITE: UsageCmd — /usage
// ============================================================================

TEST_SUITE("UsageCmd — /usage") {

    TEST_CASE("registers under primary name 'usage' with no aliases") {
        SlashCommandRegistry reg;
        register_usage_cmd(reg);
        REQUIRE(reg.lookup("usage") != nullptr);
        CHECK(reg.lookup("usage")->name() == "usage");
        CHECK(reg.lookup("usage")->aliases().empty());
    }

    TEST_CASE("requires_args is false") {
        SlashCommandRegistry reg;
        register_usage_cmd(reg);
        CHECK_FALSE(reg.lookup("usage")->requires_args());
    }

    TEST_CASE("usage_tracker null — shows friendly 'No usage data' message") {
        SlashCommandRegistry reg;
        register_usage_cmd(reg);

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(conv, reg, out, in);

        auto res = reg.lookup("usage")->execute("", ctx);
        REQUIRE(res.has_value());
        CHECK(out.str().find("No usage data") != std::string::npos);
    }

    TEST_CASE("usage_tracker with zero values — shows 0 counts") {
        SlashCommandRegistry reg;
        register_usage_cmd(reg);

        MockConversation conv;
        batbox::inference::UsageTracker tracker;  // all zeros

        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(conv, reg, out, in);
        ctx.usage_tracker = &tracker;

        auto res = reg.lookup("usage")->execute("", ctx);
        REQUIRE(res.has_value());

        const std::string text = out.str();
        CHECK(text.find("Prompt tokens")     != std::string::npos);
        CHECK(text.find("Completion tokens") != std::string::npos);
        CHECK(text.find("Total tokens")      != std::string::npos);
    }

    TEST_CASE("usage_tracker with accumulated values — shows correct counts") {
        SlashCommandRegistry reg;
        register_usage_cmd(reg);

        MockConversation conv;
        batbox::inference::UsageTracker tracker;
        {
            batbox::inference::UsageDelta d;
            d.prompt_tokens     = 1234;
            d.completion_tokens = 456;
            d.total_tokens      = 1690;
            d.cost_usd          = 0.0;
            tracker.add(d);
        }

        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(conv, reg, out, in);
        ctx.usage_tracker = &tracker;

        auto res = reg.lookup("usage")->execute("", ctx);
        REQUIRE(res.has_value());

        const std::string text = out.str();
        // Numbers will be formatted with space thousands separator.
        CHECK(text.find("1 234") != std::string::npos);
        CHECK(text.find("456")   != std::string::npos);
        CHECK(text.find("1 690") != std::string::npos);
    }

    TEST_CASE("multiple add() calls accumulate correctly") {
        SlashCommandRegistry reg;
        register_usage_cmd(reg);

        MockConversation conv;
        batbox::inference::UsageTracker tracker;
        {
            batbox::inference::UsageDelta d;
            d.prompt_tokens     = 500;
            d.completion_tokens = 200;
            d.total_tokens      = 700;
            tracker.add(d);
        }
        {
            batbox::inference::UsageDelta d;
            d.prompt_tokens     = 300;
            d.completion_tokens = 100;
            d.total_tokens      = 400;
            tracker.add(d);
        }

        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(conv, reg, out, in);
        ctx.usage_tracker = &tracker;

        auto res = reg.lookup("usage")->execute("", ctx);
        REQUIRE(res.has_value());

        const std::string text = out.str();
        // 500+300=800, 200+100=300, 700+400=1100
        CHECK(text.find("800") != std::string::npos);
        CHECK(text.find("300") != std::string::npos);
        CHECK(text.find("1 100") != std::string::npos);
    }
}

// ============================================================================
// TEST SUITE: CostCmd — /cost
// ============================================================================

TEST_SUITE("CostCmd — /cost") {

    TEST_CASE("registers under primary name 'cost' with no aliases") {
        SlashCommandRegistry reg;
        register_cost_cmd(reg);
        REQUIRE(reg.lookup("cost") != nullptr);
        CHECK(reg.lookup("cost")->name() == "cost");
        CHECK(reg.lookup("cost")->aliases().empty());
    }

    TEST_CASE("requires_args is false") {
        SlashCommandRegistry reg;
        register_cost_cmd(reg);
        CHECK_FALSE(reg.lookup("cost")->requires_args());
    }

    TEST_CASE("usage_tracker null — shows friendly 'No cost data' message") {
        SlashCommandRegistry reg;
        register_cost_cmd(reg);

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(conv, reg, out, in);

        auto res = reg.lookup("cost")->execute("", ctx);
        REQUIRE(res.has_value());
        CHECK(out.str().find("No cost data") != std::string::npos);
    }

    TEST_CASE("cost_usd zero — shows $0.0000 and zero-cost note") {
        SlashCommandRegistry reg;
        register_cost_cmd(reg);

        MockConversation conv;
        batbox::inference::UsageTracker tracker;  // cost_usd starts at 0

        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(conv, reg, out, in);
        ctx.usage_tracker = &tracker;

        auto res = reg.lookup("cost")->execute("", ctx);
        REQUIRE(res.has_value());

        const std::string text = out.str();
        CHECK(text.find("$0.0000") != std::string::npos);
        CHECK(text.find("zero-cost") != std::string::npos);
    }

    TEST_CASE("cost_usd non-zero — shows formatted cost") {
        SlashCommandRegistry reg;
        register_cost_cmd(reg);

        MockConversation conv;
        batbox::inference::UsageTracker tracker;
        {
            batbox::inference::UsageDelta d;
            d.prompt_tokens     = 1000;
            d.completion_tokens = 500;
            d.total_tokens      = 1500;
            d.cost_usd          = 0.0125;  // pre-computed cost
            tracker.add(d);
        }

        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(conv, reg, out, in);
        ctx.usage_tracker = &tracker;

        auto res = reg.lookup("cost")->execute("", ctx);
        REQUIRE(res.has_value());

        const std::string text = out.str();
        CHECK(text.find("$0.0125") != std::string::npos);
        // Zero-cost note should NOT appear when cost > 0.
        CHECK(text.find("zero-cost") == std::string::npos);
    }

    TEST_CASE("token counts shown in cost output") {
        SlashCommandRegistry reg;
        register_cost_cmd(reg);

        MockConversation conv;
        batbox::inference::UsageTracker tracker;
        {
            batbox::inference::UsageDelta d;
            d.prompt_tokens     = 2000;
            d.completion_tokens = 800;
            d.total_tokens      = 2800;
            d.cost_usd          = 0.0050;
            tracker.add(d);
        }

        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(conv, reg, out, in);
        ctx.usage_tracker = &tracker;

        auto res = reg.lookup("cost")->execute("", ctx);
        REQUIRE(res.has_value());

        const std::string text = out.str();
        CHECK(text.find("2 000") != std::string::npos);
        CHECK(text.find("800")   != std::string::npos);
        CHECK(text.find("2 800") != std::string::npos);
    }

    TEST_CASE("ModelPricing cost function produces non-negative result") {
        // Verify the underlying ModelPricing::cost is callable and non-negative.
        const double cost = batbox::inference::ModelPricing::cost(
            "gpt-4o", 1000, 500);
        CHECK(cost >= 0.0);
    }
}

// ============================================================================
// TEST SUITE: CPP S.4 — joint command behaviour
// ============================================================================

TEST_SUITE("CPP S.4 — joint command behaviour") {

    TEST_CASE("all four commands return Ok with an empty session") {
        SlashCommandRegistry reg;
        register_status_cmd(reg);
        register_stats_cmd (reg);
        register_usage_cmd (reg);
        register_cost_cmd  (reg);

        MockConversation conv;  // empty session

        for (const char* cmd_name : {"status", "stats", "usage", "cost"}) {
            std::ostringstream out;
            std::istringstream in;
            CommandContext ctx = make_ctx(conv, reg, out, in);

            auto* cmd = reg.lookup(cmd_name);
            REQUIRE(cmd != nullptr);
            auto res = cmd->execute("", ctx);
            CHECK(res.has_value());
        }
    }

    TEST_CASE("all four commands return Ok with a populated tracker") {
        SlashCommandRegistry reg;
        register_status_cmd(reg);
        register_stats_cmd (reg);
        register_usage_cmd (reg);
        register_cost_cmd  (reg);

        MockConversation conv;
        conv.model_name_val = "gpt-4o";
        conv.session_id_val = "test-session-12345";
        conv.turn_count_val = 5;

        batbox::inference::UsageTracker tracker;
        {
            batbox::inference::UsageDelta d;
            d.prompt_tokens     = 500;
            d.completion_tokens = 200;
            d.total_tokens      = 700;
            d.cost_usd          = 0.0030;
            tracker.add(d);
        }

        for (const char* cmd_name : {"status", "stats", "usage", "cost"}) {
            std::ostringstream out;
            std::istringstream in;
            CommandContext ctx = make_ctx(conv, reg, out, in);
            ctx.usage_tracker = &tracker;

            auto* cmd = reg.lookup(cmd_name);
            REQUIRE(cmd != nullptr);
            auto res = cmd->execute("", ctx);
            CHECK(res.has_value());
        }
    }

    TEST_CASE("status output contains all expected section headers") {
        SlashCommandRegistry reg;
        register_status_cmd(reg);

        MockConversation conv;
        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(conv, reg, out, in);

        auto res = reg.lookup("status")->execute("", ctx);
        REQUIRE(res.has_value());

        const std::string text = out.str();
        CHECK(text.find("Model")            != std::string::npos);
        CHECK(text.find("Session")          != std::string::npos);
        CHECK(text.find("Permission mode")  != std::string::npos);
        CHECK(text.find("Sidecar")          != std::string::npos);
        CHECK(text.find("MCP servers")      != std::string::npos);
        CHECK(text.find("Tool dependencies") != std::string::npos);
    }

    TEST_CASE("stats output contains all expected section headers") {
        SlashCommandRegistry reg;
        register_stats_cmd(reg);

        MockConversation conv;
        batbox::inference::UsageTracker tracker;

        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(conv, reg, out, in);
        ctx.usage_tracker = &tracker;

        auto res = reg.lookup("stats")->execute("", ctx);
        REQUIRE(res.has_value());

        const std::string text = out.str();
        CHECK(text.find("Turns")              != std::string::npos);
        CHECK(text.find("Prompt tokens")      != std::string::npos);
        CHECK(text.find("Completion tokens")  != std::string::npos);
        CHECK(text.find("Total tokens")       != std::string::npos);
        CHECK(text.find("Session cost")       != std::string::npos);
        CHECK(text.find("Agents spawned")     != std::string::npos);
    }

    TEST_CASE("usage output contains all expected token rows") {
        SlashCommandRegistry reg;
        register_usage_cmd(reg);

        MockConversation conv;
        batbox::inference::UsageTracker tracker;

        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(conv, reg, out, in);
        ctx.usage_tracker = &tracker;

        auto res = reg.lookup("usage")->execute("", ctx);
        REQUIRE(res.has_value());

        const std::string text = out.str();
        CHECK(text.find("Prompt tokens")     != std::string::npos);
        CHECK(text.find("Completion tokens") != std::string::npos);
        CHECK(text.find("Total tokens")      != std::string::npos);
    }

    TEST_CASE("cost output contains estimated cost row") {
        SlashCommandRegistry reg;
        register_cost_cmd(reg);

        MockConversation conv;
        batbox::inference::UsageTracker tracker;

        std::ostringstream out;
        std::istringstream in;
        CommandContext ctx = make_ctx(conv, reg, out, in);
        ctx.usage_tracker = &tracker;

        auto res = reg.lookup("cost")->execute("", ctx);
        REQUIRE(res.has_value());

        const std::string text = out.str();
        CHECK(text.find("Estimated cost") != std::string::npos);
    }
}
