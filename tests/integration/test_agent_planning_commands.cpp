// tests/integration/test_agent_planning_commands.cpp
//
// doctest integration tests for CPP S.9:
//   /agents  (AgentsCmd)
//   /plan    (PlanCmd)
//   /tasks   (TasksCmd)
//   /skills  (SkillsCmd)
//
// Strategy
// --------
// AgentsCmd   — tested with nullptr supervisor (headless path) + a stub snapshot.
//               A fake AgentSupervisor cannot be constructed (pimpl, no vtable),
//               so we verify the headless code path and registration contract.
//
// PlanCmd     — tested with a real PlanMode instance injected at construction
//               so state transitions can be verified without a running REPL.
//
// TasksCmd    — tested with a temporary tasks.json created in a tmpdir; the
//               test overrides the HOME env var so TaskStore::default_path()
//               resolves into the temp dir.
//
// SkillsCmd   — tested with nullptr skill_loader (ephemeral scan of empty dirs)
//               and directly with a pre-populated SkillLoader instance.
//
// Coverage:
//   AgentsCmd:
//     - registers under primary name "agents"
//     - identity contract (name, description, usage, aliases)
//     - headless path: supervisor==nullptr → informational message, Ok result
//   PlanCmd:
//     - registers under primary name "plan"
//     - identity contract
//     - headless path: plan_mode==nullptr → informational message, Ok result
//     - /plan (Inactive) → transitions to Planning
//     - /plan (already Planning) → shows banner, Ok result
//     - /plan status → shows state without transition
//     - /plan exit (Planning) → transitions to Inactive
//     - /plan exit (Inactive) → reports not active, Ok result
//     - unknown subcommand → Err
//   TasksCmd:
//     - registers under primary name "tasks"
//     - identity contract
//     - empty tasks.json → "No tasks found"
//     - populated tasks.json → renders task list
//     - --status filter → shows only matching tasks
//     - invalid --status → Err
//   SkillsCmd:
//     - registers under primary name "skills"
//     - identity contract
//     - no skills loaded → "No skills loaded" message
//     - /skills run <name> with known skill → outputs prompt body
//     - /skills run <name> with unknown skill → Err
//     - /skills info <name> → shows metadata
//     - /skills run (no name) → Err
//     - unknown subcommand → Err

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/agents/AgentSupervisor.hpp>
#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/conversation/PlanMode.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/plugins/SkillLoader.hpp>
#include <batbox/repl/CommandContext.hpp>
#include <batbox/tools/TaskStore.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using namespace batbox::commands;
using batbox::conversation::PlanMode;
using batbox::conversation::PlanState;
using batbox::plugins::Skill;
using batbox::plugins::SkillLoader;
using batbox::tools::TaskStore;
using batbox::tools::TaskCreateParams;

// ============================================================================
// Registration declarations (defined in each Cmd.cpp)
// ============================================================================

namespace batbox::commands {
    void register_agents_cmd(SlashCommandRegistry&, batbox::agents::AgentSupervisor*);
    void register_agents_cmd(SlashCommandRegistry&);
    void register_plan_cmd(SlashCommandRegistry&, batbox::conversation::PlanMode*);
    void register_plan_cmd(SlashCommandRegistry&);
    void register_tasks_cmd(SlashCommandRegistry&);
    void register_skills_cmd(SlashCommandRegistry&, batbox::plugins::SkillLoader*);
    void register_skills_cmd(SlashCommandRegistry&);
}

// ============================================================================
// MockConversation — minimal ConversationHandle for testing
// ============================================================================

struct MockConversation final : ConversationHandle {
    bool reset_called = false;
    std::string last_injected;

    void reset_messages() override { reset_called = true; }
    void inject_user_message(std::string_view text) override {
        last_injected = std::string(text);
    }
    std::string last_assistant_message(std::size_t /*n*/) const override { return {}; }
};

// ============================================================================
// Helpers
// ============================================================================

/// Build a CommandContext with a fresh registry and a MockConversation.
static CommandContext make_ctx(std::ostringstream& out,
                                std::istringstream& in,
                                MockConversation&   conv,
                                SlashCommandRegistry& reg)
{
    return CommandContext{out, in, false, conv, reg, fs::temp_directory_path()};
}

// ============================================================================
// AGENTS TESTS
// ============================================================================
TEST_SUITE("/agents — AgentsCmd") {

    TEST_CASE("registers under primary name 'agents'") {
        SlashCommandRegistry reg;
        register_agents_cmd(reg);
        REQUIRE(reg.lookup("agents") != nullptr);
        CHECK(reg.lookup("agents")->name() == "agents");
    }

    TEST_CASE("identity contract") {
        SlashCommandRegistry reg;
        register_agents_cmd(reg);
        ISlashCommand* cmd = reg.lookup("agents");
        REQUIRE(cmd != nullptr);
        CHECK(cmd->name() == "agents");
        CHECK_FALSE(cmd->description().empty());
        CHECK_FALSE(cmd->usage().empty());
        CHECK(cmd->aliases().empty());
        CHECK_FALSE(cmd->requires_args());
        CHECK(cmd->phase() == CommandPhase::Phase1);
    }

    TEST_CASE("headless path: supervisor==nullptr → informational message, Ok result") {
        SlashCommandRegistry reg;
        register_agents_cmd(reg);  // nullptr supervisor

        std::ostringstream out;
        std::istringstream in;
        MockConversation conv;
        CommandContext ctx = make_ctx(out, in, conv, reg);

        ISlashCommand* cmd = reg.lookup("agents");
        REQUIRE(cmd != nullptr);
        const auto result = cmd->execute("", ctx);

        CHECK(result.has_value());
        const std::string output = out.str();
        CHECK_FALSE(output.empty());
        // Should mention supervisor or context, not crash.
        CHECK(output.find("agent") != std::string::npos);
    }

    TEST_CASE("registers with explicit nullptr supervisor via overload") {
        SlashCommandRegistry reg;
        register_agents_cmd(reg, nullptr);
        REQUIRE(reg.lookup("agents") != nullptr);
    }
}

// ============================================================================
// PLAN TESTS
// ============================================================================
TEST_SUITE("/plan — PlanCmd") {

    TEST_CASE("registers under primary name 'plan'") {
        SlashCommandRegistry reg;
        register_plan_cmd(reg);
        REQUIRE(reg.lookup("plan") != nullptr);
        CHECK(reg.lookup("plan")->name() == "plan");
    }

    TEST_CASE("identity contract") {
        SlashCommandRegistry reg;
        register_plan_cmd(reg);
        ISlashCommand* cmd = reg.lookup("plan");
        REQUIRE(cmd != nullptr);
        CHECK(cmd->name() == "plan");
        CHECK_FALSE(cmd->description().empty());
        CHECK_FALSE(cmd->usage().empty());
        CHECK(cmd->aliases().empty());
        CHECK_FALSE(cmd->requires_args());
        CHECK(cmd->phase() == CommandPhase::Phase1);
    }

    TEST_CASE("headless path: plan_mode==nullptr → informational message, Ok result") {
        SlashCommandRegistry reg;
        register_plan_cmd(reg);  // nullptr plan_mode

        std::ostringstream out;
        std::istringstream in;
        MockConversation conv;
        CommandContext ctx = make_ctx(out, in, conv, reg);

        ISlashCommand* cmd = reg.lookup("plan");
        REQUIRE(cmd != nullptr);
        const auto result = cmd->execute("", ctx);

        CHECK(result.has_value());
        const std::string output = out.str();
        CHECK_FALSE(output.empty());
        CHECK(output.find("plan") != std::string::npos);
    }

    TEST_CASE("/plan (Inactive) transitions to Planning") {
        PlanMode pm;
        CHECK(pm.state() == PlanState::Inactive);

        SlashCommandRegistry reg;
        register_plan_cmd(reg, &pm);

        std::ostringstream out;
        std::istringstream in;
        MockConversation conv;
        CommandContext ctx = make_ctx(out, in, conv, reg);

        ISlashCommand* cmd = reg.lookup("plan");
        REQUIRE(cmd != nullptr);
        const auto result = cmd->execute("", ctx);

        CHECK(result.has_value());
        CHECK(pm.state() == PlanState::Planning);
        CHECK(pm.is_write_denied());
        const std::string output = out.str();
        CHECK(output.find("ask away") != std::string::npos);
    }

    TEST_CASE("/plan when already Planning shows status and returns Ok") {
        PlanMode pm;
        pm.enter_plan();

        SlashCommandRegistry reg;
        register_plan_cmd(reg, &pm);

        std::ostringstream out;
        std::istringstream in;
        MockConversation conv;
        CommandContext ctx = make_ctx(out, in, conv, reg);

        ISlashCommand* cmd = reg.lookup("plan");
        const auto result = cmd->execute("", ctx);

        CHECK(result.has_value());
        CHECK(pm.state() == PlanState::Planning);  // unchanged
        const std::string output = out.str();
        { bool found = (output.find("plan mode") != std::string::npos) || (output.find("Planning") != std::string::npos); CHECK(found); }
    }

    TEST_CASE("/plan status shows state without transition") {
        PlanMode pm;
        CHECK(pm.state() == PlanState::Inactive);

        SlashCommandRegistry reg;
        register_plan_cmd(reg, &pm);

        std::ostringstream out;
        std::istringstream in;
        MockConversation conv;
        CommandContext ctx = make_ctx(out, in, conv, reg);

        ISlashCommand* cmd = reg.lookup("plan");
        const auto result = cmd->execute("status", ctx);

        CHECK(result.has_value());
        CHECK(pm.state() == PlanState::Inactive);  // no transition
        const std::string output = out.str();
        CHECK(output.find("Inactive") != std::string::npos);
    }

    TEST_CASE("/plan exit from Planning transitions to Inactive") {
        PlanMode pm;
        pm.enter_plan();
        CHECK(pm.state() == PlanState::Planning);

        SlashCommandRegistry reg;
        register_plan_cmd(reg, &pm);

        std::ostringstream out;
        std::istringstream in;
        MockConversation conv;
        CommandContext ctx = make_ctx(out, in, conv, reg);

        ISlashCommand* cmd = reg.lookup("plan");
        const auto result = cmd->execute("exit", ctx);

        CHECK(result.has_value());
        CHECK(pm.state() == PlanState::Inactive);
    }

    TEST_CASE("/plan exit when Inactive reports not active and returns Ok") {
        PlanMode pm;
        CHECK(pm.state() == PlanState::Inactive);

        SlashCommandRegistry reg;
        register_plan_cmd(reg, &pm);

        std::ostringstream out;
        std::istringstream in;
        MockConversation conv;
        CommandContext ctx = make_ctx(out, in, conv, reg);

        ISlashCommand* cmd = reg.lookup("plan");
        const auto result = cmd->execute("exit", ctx);

        CHECK(result.has_value());
        CHECK(pm.state() == PlanState::Inactive);  // unchanged
        const std::string output = out.str();
        CHECK_FALSE(output.empty());
    }

    // TUI-PLAN-T4: unknown text after /plan is treated as an initial prompt,
    // not an error.  The text is forwarded via inject_user_message.
    TEST_CASE("/plan with initial prompt forwards to conversation and transitions to Planning") {
        PlanMode pm;
        SlashCommandRegistry reg;
        register_plan_cmd(reg, &pm);

        std::ostringstream out;
        std::istringstream in;
        MockConversation conv;
        CommandContext ctx = make_ctx(out, in, conv, reg);

        ISlashCommand* cmd = reg.lookup("plan");
        const auto result = cmd->execute("fix the auth bug", ctx);

        // /plan <prompt> must succeed (not return Err)
        CHECK(result.has_value());
        // Plan mode must be entered
        CHECK(pm.is_planning());
        CHECK(pm.state() == PlanState::Planning);
        // The initial prompt must be forwarded to the conversation
        CHECK(conv.last_injected == "fix the auth bug");
        // Banner must mention clarifying questions
        const std::string output = out.str();
        CHECK(output.find("ask away") != std::string::npos);
    }

    // TUI-PLAN-T4: /plan is_planning() == true assertion
    TEST_CASE("/plan (Inactive) sets is_planning() == true") {
        PlanMode pm;
        SlashCommandRegistry reg;
        register_plan_cmd(reg, &pm);

        std::ostringstream out;
        std::istringstream in;
        MockConversation conv;
        CommandContext ctx = make_ctx(out, in, conv, reg);

        ISlashCommand* cmd = reg.lookup("plan");
        const auto result = cmd->execute("", ctx);

        REQUIRE(result.has_value());
        CHECK(pm.is_planning());  // TUI-PLAN-T4: /plan must set is_planning() == true
    }

    // TUI-PLAN-T4: /plan with no prompt does NOT forward empty message
    TEST_CASE("/plan with no prompt does not inject empty message") {
        PlanMode pm;
        SlashCommandRegistry reg;
        register_plan_cmd(reg, &pm);

        std::ostringstream out;
        std::istringstream in;
        MockConversation conv;
        CommandContext ctx = make_ctx(out, in, conv, reg);

        ISlashCommand* cmd = reg.lookup("plan");
        (void)cmd->execute("", ctx);

        // No message should be injected when no initial prompt provided
        CHECK(conv.last_injected.empty());
    }

    // TUI-PLAN-T4: confirmation banner mentions AskUserQuestion
    TEST_CASE("/plan confirmation banner mentions AskUserQuestion") {
        PlanMode pm;
        SlashCommandRegistry reg;
        register_plan_cmd(reg, &pm);

        std::ostringstream out;
        std::istringstream in;
        MockConversation conv;
        CommandContext ctx = make_ctx(out, in, conv, reg);

        ISlashCommand* cmd = reg.lookup("plan");
        (void)cmd->execute("", ctx);

        const std::string output = out.str();
        CHECK(output.find("AskUserQuestion") != std::string::npos);
        CHECK(output.find("ExitPlanMode") != std::string::npos);
    }
}

// ============================================================================
// TASKS TESTS
// ============================================================================
TEST_SUITE("/tasks — TasksCmd") {

    // -------------------------------------------------------------------------
    // Helper: TempHome — scoped RAII that redirects HOME to a temp directory
    // and restores it on destruction.  Ensures TaskStore::default_path()
    // targets our temp dir.
    // -------------------------------------------------------------------------
    struct TempHome {
        fs::path dir;
        std::string old_home;

        TempHome() {
            dir = fs::temp_directory_path() / "batbox_tasks_test";
            fs::create_directories(dir / ".batbox");

            const char* h = std::getenv("HOME");
            old_home = h ? h : "";
            ::setenv("HOME", dir.string().c_str(), 1);
        }

        ~TempHome() {
            ::setenv("HOME", old_home.c_str(), 1);
            std::error_code ec;
            fs::remove_all(dir, ec);
        }

        [[nodiscard]] fs::path tasks_path() const {
            return dir / ".batbox" / "tasks.json";
        }
    };

    TEST_CASE("registers under primary name 'tasks'") {
        SlashCommandRegistry reg;
        register_tasks_cmd(reg);
        REQUIRE(reg.lookup("tasks") != nullptr);
        CHECK(reg.lookup("tasks")->name() == "tasks");
    }

    TEST_CASE("identity contract") {
        SlashCommandRegistry reg;
        register_tasks_cmd(reg);
        ISlashCommand* cmd = reg.lookup("tasks");
        REQUIRE(cmd != nullptr);
        CHECK(cmd->name() == "tasks");
        CHECK_FALSE(cmd->description().empty());
        CHECK_FALSE(cmd->usage().empty());
        CHECK(cmd->aliases().empty());
        CHECK_FALSE(cmd->requires_args());
        CHECK(cmd->phase() == CommandPhase::Phase1);
    }

    TEST_CASE("empty tasks.json (or missing file) → 'No tasks found'") {
        TempHome th;

        SlashCommandRegistry reg;
        register_tasks_cmd(reg);

        std::ostringstream out;
        std::istringstream in;
        MockConversation conv;
        CommandContext ctx = make_ctx(out, in, conv, reg);

        ISlashCommand* cmd = reg.lookup("tasks");
        REQUIRE(cmd != nullptr);
        const auto result = cmd->execute("", ctx);

        CHECK(result.has_value());
        const std::string output = out.str();
        CHECK(output.find("No tasks") != std::string::npos);
    }

    TEST_CASE("populated tasks.json renders task list") {
        TempHome th;
        TaskStore store(th.tasks_path());

        TaskCreateParams p1;
        p1.title       = "First task";
        p1.description = "Do the first thing";
        p1.status      = "pending";
        p1.tags        = {"alpha", "beta"};
        const auto t1  = store.create_task(p1);
        REQUIRE(t1.has_value());

        TaskCreateParams p2;
        p2.title  = "Second task";
        p2.status = "completed";
        const auto t2 = store.create_task(p2);
        REQUIRE(t2.has_value());

        SlashCommandRegistry reg;
        register_tasks_cmd(reg);

        std::ostringstream out;
        std::istringstream in;
        MockConversation conv;
        CommandContext ctx = make_ctx(out, in, conv, reg);

        ISlashCommand* cmd = reg.lookup("tasks");
        const auto result = cmd->execute("", ctx);

        CHECK(result.has_value());
        const std::string output = out.str();
        CHECK(output.find("First task")  != std::string::npos);
        CHECK(output.find("Second task") != std::string::npos);
        CHECK(output.find("pending")     != std::string::npos);
        CHECK(output.find("completed")   != std::string::npos);
        // Tags should appear.
        CHECK(output.find("#alpha") != std::string::npos);
        CHECK(output.find("#beta")  != std::string::npos);
        // Summary count.
        CHECK(output.find("2 task(s)") != std::string::npos);
    }

    TEST_CASE("--status filter shows only matching tasks") {
        TempHome th;
        TaskStore store(th.tasks_path());

        TaskCreateParams pp;
        pp.title  = "Pending one";
        pp.status = "pending";
        (void)store.create_task(pp);

        TaskCreateParams pc;
        pc.title  = "Completed one";
        pc.status = "completed";
        (void)store.create_task(pc);

        SlashCommandRegistry reg;
        register_tasks_cmd(reg);

        std::ostringstream out;
        std::istringstream in;
        MockConversation conv;
        CommandContext ctx = make_ctx(out, in, conv, reg);

        ISlashCommand* cmd = reg.lookup("tasks");
        const auto result = cmd->execute("--status pending", ctx);

        CHECK(result.has_value());
        const std::string output = out.str();
        CHECK(output.find("Pending one")   != std::string::npos);
        CHECK(output.find("Completed one") == std::string::npos);
    }

    TEST_CASE("invalid --status value returns Err") {
        SlashCommandRegistry reg;
        register_tasks_cmd(reg);

        std::ostringstream out;
        std::istringstream in;
        MockConversation conv;
        CommandContext ctx = make_ctx(out, in, conv, reg);

        ISlashCommand* cmd = reg.lookup("tasks");
        const auto result = cmd->execute("--status bogus", ctx);

        CHECK_FALSE(result.has_value());
        { bool ok = (result.error().find("bogus") != std::string::npos) || (result.error().find("invalid") != std::string::npos); CHECK(ok); }
    }
}

// ============================================================================
// SKILLS TESTS
// ============================================================================
TEST_SUITE("/skills — SkillsCmd") {

    // -------------------------------------------------------------------------
    // Helper: build a SkillLoader pre-populated with a synthetic skill.
    // -------------------------------------------------------------------------
    static SkillLoader make_loader_with_skill(const std::string& name,
                                               const std::string& description,
                                               const std::string& prompt_body)
    {
        // Create a temp dir with a skill .md file.
        const fs::path dir = fs::temp_directory_path() / "batbox_skills_test";
        fs::create_directories(dir);

        const fs::path md_path = dir / (name + ".md");
        {
            std::ofstream f(md_path);
            f << "---\n";
            f << "name: " << name << "\n";
            f << "description: " << description << "\n";
            f << "---\n";
            f << prompt_body << "\n";
        }

        SkillLoader loader;
        loader.scan_dir(dir, "test");
        return loader;
    }

    TEST_CASE("registers under primary name 'skills'") {
        SlashCommandRegistry reg;
        register_skills_cmd(reg);
        REQUIRE(reg.lookup("skills") != nullptr);
        CHECK(reg.lookup("skills")->name() == "skills");
    }

    TEST_CASE("identity contract") {
        SlashCommandRegistry reg;
        register_skills_cmd(reg);
        ISlashCommand* cmd = reg.lookup("skills");
        REQUIRE(cmd != nullptr);
        CHECK(cmd->name() == "skills");
        CHECK_FALSE(cmd->description().empty());
        CHECK_FALSE(cmd->usage().empty());
        CHECK(cmd->aliases().empty());
        CHECK_FALSE(cmd->requires_args());
        CHECK(cmd->phase() == CommandPhase::Phase1);
    }

    TEST_CASE("no skills loaded → 'No skills loaded' message") {
        // Create an empty loader (no dirs scanned).
        SkillLoader loader;  // no dirs, no bundled

        SlashCommandRegistry reg;
        register_skills_cmd(reg, &loader);

        std::ostringstream out;
        std::istringstream in;
        MockConversation conv;
        CommandContext ctx = make_ctx(out, in, conv, reg);

        ISlashCommand* cmd = reg.lookup("skills");
        REQUIRE(cmd != nullptr);
        const auto result = cmd->execute("", ctx);

        CHECK(result.has_value());
        const std::string output = out.str();
        CHECK(output.find("No skills") != std::string::npos);
    }

    TEST_CASE("loaded skills are listed with name and description") {
        SkillLoader loader = make_loader_with_skill(
            "remember", "Store information for later recall",
            "When asked to remember something, store it.");

        SlashCommandRegistry reg;
        register_skills_cmd(reg, &loader);

        std::ostringstream out;
        std::istringstream in;
        MockConversation conv;
        CommandContext ctx = make_ctx(out, in, conv, reg);

        ISlashCommand* cmd = reg.lookup("skills");
        const auto result = cmd->execute("", ctx);

        CHECK(result.has_value());
        const std::string output = out.str();
        CHECK(output.find("remember") != std::string::npos);
        CHECK(output.find("Store information") != std::string::npos);
    }

    TEST_CASE("/skills run <name> outputs prompt body") {
        SkillLoader loader = make_loader_with_skill(
            "debug", "Help debug an issue",
            "Analyze the error carefully and suggest fixes.");

        SlashCommandRegistry reg;
        register_skills_cmd(reg, &loader);

        std::ostringstream out;
        std::istringstream in;
        MockConversation conv;
        CommandContext ctx = make_ctx(out, in, conv, reg);

        ISlashCommand* cmd = reg.lookup("skills");
        const auto result = cmd->execute("run debug", ctx);

        CHECK(result.has_value());
        const std::string output = out.str();
        CHECK(output.find("Analyze the error carefully") != std::string::npos);
        CHECK(output.find("debug") != std::string::npos);
    }

    TEST_CASE("/skills run <unknown> returns Err") {
        SkillLoader loader;  // empty

        SlashCommandRegistry reg;
        register_skills_cmd(reg, &loader);

        std::ostringstream out;
        std::istringstream in;
        MockConversation conv;
        CommandContext ctx = make_ctx(out, in, conv, reg);

        ISlashCommand* cmd = reg.lookup("skills");
        const auto result = cmd->execute("run nonexistent-skill", ctx);

        CHECK_FALSE(result.has_value());
        { bool ok = (result.error().find("nonexistent-skill") != std::string::npos) || (result.error().find("not found") != std::string::npos); CHECK(ok); }
    }

    TEST_CASE("/skills run with no name argument returns Err") {
        SkillLoader loader;

        SlashCommandRegistry reg;
        register_skills_cmd(reg, &loader);

        std::ostringstream out;
        std::istringstream in;
        MockConversation conv;
        CommandContext ctx = make_ctx(out, in, conv, reg);

        ISlashCommand* cmd = reg.lookup("skills");
        const auto result = cmd->execute("run", ctx);

        CHECK_FALSE(result.has_value());
        { bool ok = (result.error().find("name") != std::string::npos) || (result.error().find("required") != std::string::npos); CHECK(ok); }
    }

    TEST_CASE("/skills info <name> shows skill metadata") {
        SkillLoader loader = make_loader_with_skill(
            "my-skill", "A test skill", "Do the thing.");

        SlashCommandRegistry reg;
        register_skills_cmd(reg, &loader);

        std::ostringstream out;
        std::istringstream in;
        MockConversation conv;
        CommandContext ctx = make_ctx(out, in, conv, reg);

        ISlashCommand* cmd = reg.lookup("skills");
        const auto result = cmd->execute("info my-skill", ctx);

        CHECK(result.has_value());
        const std::string output = out.str();
        CHECK(output.find("my-skill")    != std::string::npos);
        CHECK(output.find("A test skill") != std::string::npos);
        CHECK(output.find("Do the thing") != std::string::npos);
    }

    TEST_CASE("/skills info <unknown> returns Err") {
        SkillLoader loader;

        SlashCommandRegistry reg;
        register_skills_cmd(reg, &loader);

        std::ostringstream out;
        std::istringstream in;
        MockConversation conv;
        CommandContext ctx = make_ctx(out, in, conv, reg);

        ISlashCommand* cmd = reg.lookup("skills");
        const auto result = cmd->execute("info ghost-skill", ctx);

        CHECK_FALSE(result.has_value());
    }

    TEST_CASE("/skills unknown subcommand returns Err") {
        SkillLoader loader;

        SlashCommandRegistry reg;
        register_skills_cmd(reg, &loader);

        std::ostringstream out;
        std::istringstream in;
        MockConversation conv;
        CommandContext ctx = make_ctx(out, in, conv, reg);

        ISlashCommand* cmd = reg.lookup("skills");
        const auto result = cmd->execute("teleport anywhere", ctx);

        CHECK_FALSE(result.has_value());
        { bool ok = (result.error().find("teleport") != std::string::npos) || (result.error().find("unknown") != std::string::npos); CHECK(ok); }
    }

    TEST_CASE("registers with explicit nullptr loader via overload") {
        SlashCommandRegistry reg;
        register_skills_cmd(reg, nullptr);
        REQUIRE(reg.lookup("skills") != nullptr);
    }
}
