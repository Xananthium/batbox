// tests/integration/test_permissions_advisor.cpp
//
// doctest integration-test suite for CPP S.10:
//   /permissions (PermissionsCmd)
//   /hooks       (HooksCmd)
//   /advisor     (AdvisorCmd)
//
// Strategy
// --------
// All three commands operate against the filesystem or CommandContext state,
// so no inference engine or TUI is needed.
//
// PermissionsCmd tests write a temporary settings.json (via PermissionStore)
// and verify that add/remove round-trips work correctly.
//
// HooksCmd tests write a temporary settings.json with a "hooks" object and
// verify the formatted output lists the configured events and matchers.
//
// AdvisorCmd tests verify the ctx.advisor_mode toggle semantics and output.
//
// Coverage:
//   PermissionsCmd:
//     - registers under primary name "permissions"
//     - no-args → lists rules (empty)
//     - no-args → lists rules (populated)
//     - add allow rule → persisted to disk
//     - add deny rule → persisted to disk
//     - add ask rule → persisted to disk
//     - add duplicate rule → no-op, returns Ok
//     - remove rule → removed from all lists
//     - remove absent rule → no-op, returns Ok
//     - add with empty pattern → returns Err
//     - add with unknown kind → returns Err
//     - unknown subcommand → returns Err
//   HooksCmd:
//     - registers under primary name "hooks"
//     - no hooks file → summary says "No hooks configured"
//     - hooks in settings.json → summary lists event names + counts
//     - /hooks PreToolUse → shows matcher detail
//     - /hooks UnknownEvent → warns about unknown event name
//   AdvisorCmd:
//     - registers under primary name "advisor"
//     - no args → toggles off→on
//     - no args → toggles on→off
//     - /advisor on → enables
//     - /advisor on when already on → no-op message
//     - /advisor off → disables
//     - /advisor off when already off → no-op message
//     - /advisor status → prints state without toggling
//     - unknown subcommand → returns Err

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/permissions/PermissionStore.hpp>
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
// Registration declarations (defined in each Cmd.cpp)
// ============================================================================

namespace batbox::commands {
    void register_permissions_cmd(SlashCommandRegistry&);
    void register_hooks_cmd(SlashCommandRegistry&);
    void register_advisor_cmd(SlashCommandRegistry&);
}

// ============================================================================
// MockConversation
// ============================================================================

struct MockConversation final : ConversationHandle {
    void reset_messages() override {}
    void inject_user_message(std::string_view) override {}
    std::string last_assistant_message(std::size_t) const override { return {}; }
};

// ============================================================================
// Fixture helpers
// ============================================================================

struct Fixture {
    fs::path         tmp_dir;
    fs::path         settings_path;
    SlashCommandRegistry registry;
    MockConversation conv;
    std::ostringstream out;
    std::istringstream in;

    explicit Fixture(const std::string& dir_name) {
        tmp_dir      = fs::temp_directory_path() / dir_name;
        settings_path = tmp_dir / "settings.json";
        fs::create_directories(tmp_dir);

        register_permissions_cmd(registry);
        register_hooks_cmd(registry);
        register_advisor_cmd(registry);
    }

    ~Fixture() {
        std::error_code ec;
        fs::remove_all(tmp_dir, ec);
    }

    /// Build a CommandContext with config_dir pointing at tmp_dir.
    CommandContext make_ctx() {
        return CommandContext{
            .output       = out,
            .input        = in,
            .exit_requested = false,
            .conversation = conv,
            .registry     = registry,
            .cwd          = tmp_dir,
            .config_dir   = tmp_dir,
        };
    }

    /// Write a raw JSON string to settings_path.
    void write_settings(const std::string& json) {
        std::ofstream f(settings_path, std::ios::trunc);
        f << json;
        f.close();
    }

    /// Read settings_path and return as a string.
    std::string read_settings() {
        std::ifstream f(settings_path);
        std::ostringstream buf;
        buf << f.rdbuf();
        return buf.str();
    }
};

// ============================================================================
// TEST SUITE: PermissionsCmd — registration
// ============================================================================

TEST_SUITE("PermissionsCmd — registration") {

    TEST_CASE("registers under primary name 'permissions'") {
        SlashCommandRegistry reg;
        register_permissions_cmd(reg);
        REQUIRE(reg.lookup("permissions") != nullptr);
        CHECK(reg.lookup("permissions")->name() == "permissions");
    }

    TEST_CASE("has no aliases") {
        SlashCommandRegistry reg;
        register_permissions_cmd(reg);
        CHECK(reg.lookup("permissions")->aliases().empty());
    }

    TEST_CASE("requires_args is false") {
        SlashCommandRegistry reg;
        register_permissions_cmd(reg);
        CHECK_FALSE(reg.lookup("permissions")->requires_args());
    }
}

// ============================================================================
// TEST SUITE: PermissionsCmd — list
// ============================================================================

TEST_SUITE("PermissionsCmd — list rules") {

    TEST_CASE("no-args with no settings file shows empty rules message") {
        Fixture f("batbox_perm_list_empty");
        // No settings.json written → store starts empty.
        auto ctx = f.make_ctx();
        auto res = f.registry.lookup("permissions")->execute("", ctx);
        REQUIRE(res.has_value());
        CHECK(f.out.str().find("No permission rules") != std::string::npos);
    }

    TEST_CASE("no-args with populated rules lists all kinds") {
        Fixture f("batbox_perm_list_pop");
        // Pre-populate via PermissionStore.
        batbox::permissions::PermissionStore store(f.settings_path);
        REQUIRE(store.add_allow_rule("Bash(git *)").has_value());
        REQUIRE(store.add_deny_rule("Bash(rm -rf *)").has_value());
        REQUIRE(store.add_ask_rule("Bash(npm *)").has_value());

        auto ctx = f.make_ctx();
        auto res = f.registry.lookup("permissions")->execute("", ctx);
        REQUIRE(res.has_value());

        const std::string o = f.out.str();
        CHECK(o.find("Allow") != std::string::npos);
        CHECK(o.find("Bash(git *)") != std::string::npos);
        CHECK(o.find("Deny") != std::string::npos);
        CHECK(o.find("Bash(rm -rf *)") != std::string::npos);
        CHECK(o.find("Ask") != std::string::npos);
        CHECK(o.find("Bash(npm *)") != std::string::npos);
    }
}

// ============================================================================
// TEST SUITE: PermissionsCmd — add
// ============================================================================

TEST_SUITE("PermissionsCmd — add") {

    TEST_CASE("add allow rule persists to disk") {
        Fixture f("batbox_perm_add_allow");
        auto ctx = f.make_ctx();
        auto res = f.registry.lookup("permissions")->execute(
            "add allow Bash(git *)", ctx);
        REQUIRE(res.has_value());

        // Verify persisted.
        batbox::permissions::PermissionStore verify(f.settings_path);
        const auto& rules = verify.allow_rules();
        CHECK(std::find(rules.begin(), rules.end(), "Bash(git *)") != rules.end());
    }

    TEST_CASE("add deny rule persists to disk") {
        Fixture f("batbox_perm_add_deny");
        auto ctx = f.make_ctx();
        auto res = f.registry.lookup("permissions")->execute(
            "add deny Bash(rm -rf *)", ctx);
        REQUIRE(res.has_value());

        batbox::permissions::PermissionStore verify(f.settings_path);
        const auto& rules = verify.deny_rules();
        CHECK(std::find(rules.begin(), rules.end(), "Bash(rm -rf *)") != rules.end());
    }

    TEST_CASE("add ask rule persists to disk") {
        Fixture f("batbox_perm_add_ask");
        auto ctx = f.make_ctx();
        auto res = f.registry.lookup("permissions")->execute(
            "add ask Bash(npm *)", ctx);
        REQUIRE(res.has_value());

        batbox::permissions::PermissionStore verify(f.settings_path);
        const auto& rules = verify.ask_rules();
        CHECK(std::find(rules.begin(), rules.end(), "Bash(npm *)") != rules.end());
    }

    TEST_CASE("add duplicate rule returns Ok (no-op)") {
        Fixture f("batbox_perm_add_dup");
        auto ctx = f.make_ctx();

        // Add once.
        REQUIRE(f.registry.lookup("permissions")->execute(
            "add allow Read(./src/**)", ctx).has_value());

        // Add again — should succeed silently.
        f.out.str("");
        auto ctx2 = f.make_ctx();
        auto res = f.registry.lookup("permissions")->execute(
            "add allow Read(./src/**)", ctx2);
        CHECK(res.has_value());
    }

    TEST_CASE("add with empty pattern returns Err") {
        Fixture f("batbox_perm_add_empty");
        auto ctx = f.make_ctx();
        auto res = f.registry.lookup("permissions")->execute("add allow   ", ctx);
        CHECK_FALSE(res.has_value());
        CHECK(res.error().find("non-empty") != std::string::npos);
    }

    TEST_CASE("add with unknown kind returns Err") {
        Fixture f("batbox_perm_add_badkind");
        auto ctx = f.make_ctx();
        auto res = f.registry.lookup("permissions")->execute(
            "add permitall Bash(x)", ctx);
        CHECK_FALSE(res.has_value());
        CHECK(res.error().find("allow") != std::string::npos);
    }
}

// ============================================================================
// TEST SUITE: PermissionsCmd — remove
// ============================================================================

TEST_SUITE("PermissionsCmd — remove") {

    TEST_CASE("remove existing rule removes from all lists") {
        Fixture f("batbox_perm_remove");
        batbox::permissions::PermissionStore store(f.settings_path);
        REQUIRE(store.add_allow_rule("Bash(git *)").has_value());

        auto ctx = f.make_ctx();
        auto res = f.registry.lookup("permissions")->execute(
            "remove Bash(git *)", ctx);
        REQUIRE(res.has_value());

        batbox::permissions::PermissionStore verify(f.settings_path);
        const auto& rules = verify.allow_rules();
        CHECK(std::find(rules.begin(), rules.end(), "Bash(git *)") == rules.end());
    }

    TEST_CASE("remove absent rule returns Ok (no-op)") {
        Fixture f("batbox_perm_remove_absent");
        auto ctx = f.make_ctx();
        auto res = f.registry.lookup("permissions")->execute(
            "remove Bash(nonexistent *)", ctx);
        CHECK(res.has_value());
    }

    TEST_CASE("remove with empty pattern returns Err") {
        Fixture f("batbox_perm_remove_empty");
        auto ctx = f.make_ctx();
        auto res = f.registry.lookup("permissions")->execute("remove   ", ctx);
        CHECK_FALSE(res.has_value());
    }
}

// ============================================================================
// TEST SUITE: PermissionsCmd — errors
// ============================================================================

TEST_SUITE("PermissionsCmd — error paths") {

    TEST_CASE("unknown subcommand returns Err") {
        Fixture f("batbox_perm_unknown");
        auto ctx = f.make_ctx();
        auto res = f.registry.lookup("permissions")->execute("list --all", ctx);
        CHECK_FALSE(res.has_value());
        CHECK(res.error().find("unknown subcommand") != std::string::npos);
    }
}

// ============================================================================
// TEST SUITE: HooksCmd — registration
// ============================================================================

TEST_SUITE("HooksCmd — registration") {

    TEST_CASE("registers under primary name 'hooks'") {
        SlashCommandRegistry reg;
        register_hooks_cmd(reg);
        REQUIRE(reg.lookup("hooks") != nullptr);
        CHECK(reg.lookup("hooks")->name() == "hooks");
    }

    TEST_CASE("has no aliases") {
        SlashCommandRegistry reg;
        register_hooks_cmd(reg);
        CHECK(reg.lookup("hooks")->aliases().empty());
    }

    TEST_CASE("requires_args is false") {
        SlashCommandRegistry reg;
        register_hooks_cmd(reg);
        CHECK_FALSE(reg.lookup("hooks")->requires_args());
    }
}

// ============================================================================
// TEST SUITE: HooksCmd — display
// ============================================================================

TEST_SUITE("HooksCmd — display") {

    TEST_CASE("no settings file shows 'No hooks configured'") {
        Fixture f("batbox_hooks_no_file");
        auto ctx = f.make_ctx();
        auto res = f.registry.lookup("hooks")->execute("", ctx);
        REQUIRE(res.has_value());
        CHECK(f.out.str().find("No hooks configured") != std::string::npos);
    }

    TEST_CASE("settings file with no hooks key shows 'No hooks configured'") {
        Fixture f("batbox_hooks_no_key");
        f.write_settings(R"({ "permissions": { "allow": [] } })");
        auto ctx = f.make_ctx();
        auto res = f.registry.lookup("hooks")->execute("", ctx);
        REQUIRE(res.has_value());
        CHECK(f.out.str().find("No hooks configured") != std::string::npos);
    }

    TEST_CASE("configured hooks appear in summary listing") {
        Fixture f("batbox_hooks_summary");
        f.write_settings(R"({
            "hooks": {
                "PreToolUse": [
                    { "matcher": "Bash", "hooks": [
                        { "type": "command", "command": "echo pre" }
                    ]}
                ],
                "PostToolUse": [
                    { "matcher": ".*", "hooks": [
                        { "type": "command", "command": "echo post" },
                        { "type": "prompt",  "prompt": "summarize" }
                    ]}
                ]
            }
        })");
        auto ctx = f.make_ctx();
        auto res = f.registry.lookup("hooks")->execute("", ctx);
        REQUIRE(res.has_value());

        const std::string o = f.out.str();
        CHECK(o.find("PreToolUse") != std::string::npos);
        CHECK(o.find("PostToolUse") != std::string::npos);
        // Event count line.
        CHECK(o.find("2 events") != std::string::npos);
    }

    TEST_CASE("/hooks PreToolUse shows matcher and hooks detail") {
        Fixture f("batbox_hooks_detail");
        f.write_settings(R"({
            "hooks": {
                "PreToolUse": [
                    { "matcher": "Bash", "hooks": [
                        { "type": "command", "command": "scripts/pre-tool.sh" }
                    ]}
                ]
            }
        })");
        auto ctx = f.make_ctx();
        auto res = f.registry.lookup("hooks")->execute("PreToolUse", ctx);
        REQUIRE(res.has_value());

        const std::string o = f.out.str();
        CHECK(o.find("PreToolUse") != std::string::npos);
        CHECK(o.find("Bash") != std::string::npos);
        CHECK(o.find("scripts/pre-tool.sh") != std::string::npos);
        CHECK(o.find("shell command") != std::string::npos);
    }

    TEST_CASE("/hooks with http hook type shows url") {
        Fixture f("batbox_hooks_http");
        f.write_settings(R"({
            "hooks": {
                "PostToolUse": [
                    { "matcher": ".*", "hooks": [
                        { "type": "http", "url": "https://hooks.example.com/notify" }
                    ]}
                ]
            }
        })");
        auto ctx = f.make_ctx();
        auto res = f.registry.lookup("hooks")->execute("PostToolUse", ctx);
        REQUIRE(res.has_value());

        const std::string o = f.out.str();
        CHECK(o.find("HTTP") != std::string::npos);
        CHECK(o.find("https://hooks.example.com/notify") != std::string::npos);
    }

    TEST_CASE("/hooks with unknown event name warns but does not error") {
        Fixture f("batbox_hooks_unknown_event");
        f.write_settings(R"({ "hooks": {} })");
        auto ctx = f.make_ctx();
        auto res = f.registry.lookup("hooks")->execute("SomeUnknownEvent", ctx);
        REQUIRE(res.has_value());  // must not return Err
        CHECK(f.out.str().find("not a standard hook event") != std::string::npos);
    }

    TEST_CASE("/hooks detail for unconfigured event shows 'No hooks' message") {
        Fixture f("batbox_hooks_unconfigured");
        f.write_settings(R"({
            "hooks": {
                "PreToolUse": []
            }
        })");
        auto ctx = f.make_ctx();
        auto res = f.registry.lookup("hooks")->execute("PostToolUse", ctx);
        REQUIRE(res.has_value());
        CHECK(f.out.str().find("No hooks configured for event") != std::string::npos);
    }
}

// ============================================================================
// TEST SUITE: AdvisorCmd — registration
// ============================================================================

TEST_SUITE("AdvisorCmd — registration") {

    TEST_CASE("registers under primary name 'advisor'") {
        SlashCommandRegistry reg;
        register_advisor_cmd(reg);
        REQUIRE(reg.lookup("advisor") != nullptr);
        CHECK(reg.lookup("advisor")->name() == "advisor");
    }

    TEST_CASE("has no aliases") {
        SlashCommandRegistry reg;
        register_advisor_cmd(reg);
        CHECK(reg.lookup("advisor")->aliases().empty());
    }

    TEST_CASE("requires_args is false") {
        SlashCommandRegistry reg;
        register_advisor_cmd(reg);
        CHECK_FALSE(reg.lookup("advisor")->requires_args());
    }
}

// ============================================================================
// TEST SUITE: AdvisorCmd — toggle semantics
// ============================================================================

TEST_SUITE("AdvisorCmd — toggle semantics") {

    TEST_CASE("no args toggles off→on and sets ctx.advisor_mode = true") {
        Fixture f("batbox_adv_toggle_on");
        auto ctx = f.make_ctx();
        REQUIRE_FALSE(ctx.advisor_mode);

        auto res = f.registry.lookup("advisor")->execute("", ctx);
        REQUIRE(res.has_value());
        CHECK(ctx.advisor_mode == true);
        CHECK(f.out.str().find("enabled") != std::string::npos);
    }

    TEST_CASE("no args toggles on→off and sets ctx.advisor_mode = false") {
        Fixture f("batbox_adv_toggle_off");
        auto ctx = f.make_ctx();
        ctx.advisor_mode = true;

        auto res = f.registry.lookup("advisor")->execute("", ctx);
        REQUIRE(res.has_value());
        CHECK(ctx.advisor_mode == false);
        CHECK(f.out.str().find("disabled") != std::string::npos);
    }

    TEST_CASE("/advisor on enables advisor mode") {
        Fixture f("batbox_adv_on");
        auto ctx = f.make_ctx();
        REQUIRE_FALSE(ctx.advisor_mode);

        auto res = f.registry.lookup("advisor")->execute("on", ctx);
        REQUIRE(res.has_value());
        CHECK(ctx.advisor_mode == true);
        CHECK(f.out.str().find("enabled") != std::string::npos);
    }

    TEST_CASE("/advisor on when already on prints 'already on'") {
        Fixture f("batbox_adv_on_noop");
        auto ctx = f.make_ctx();
        ctx.advisor_mode = true;

        auto res = f.registry.lookup("advisor")->execute("on", ctx);
        REQUIRE(res.has_value());
        CHECK(ctx.advisor_mode == true);
        CHECK(f.out.str().find("already on") != std::string::npos);
    }

    TEST_CASE("/advisor off disables advisor mode") {
        Fixture f("batbox_adv_off");
        auto ctx = f.make_ctx();
        ctx.advisor_mode = true;

        auto res = f.registry.lookup("advisor")->execute("off", ctx);
        REQUIRE(res.has_value());
        CHECK(ctx.advisor_mode == false);
        CHECK(f.out.str().find("disabled") != std::string::npos);
    }

    TEST_CASE("/advisor off when already off prints 'already off'") {
        Fixture f("batbox_adv_off_noop");
        auto ctx = f.make_ctx();
        REQUIRE_FALSE(ctx.advisor_mode);

        auto res = f.registry.lookup("advisor")->execute("off", ctx);
        REQUIRE(res.has_value());
        CHECK(ctx.advisor_mode == false);
        CHECK(f.out.str().find("already off") != std::string::npos);
    }

    TEST_CASE("/advisor status reports state without changing it") {
        Fixture f("batbox_adv_status_off");
        auto ctx = f.make_ctx();
        REQUIRE_FALSE(ctx.advisor_mode);

        auto res = f.registry.lookup("advisor")->execute("status", ctx);
        REQUIRE(res.has_value());
        CHECK(ctx.advisor_mode == false);  // unchanged
        CHECK(f.out.str().find("OFF") != std::string::npos);
    }

    TEST_CASE("/advisor status when active reports ON") {
        Fixture f("batbox_adv_status_on");
        auto ctx = f.make_ctx();
        ctx.advisor_mode = true;

        auto res = f.registry.lookup("advisor")->execute("status", ctx);
        REQUIRE(res.has_value());
        CHECK(ctx.advisor_mode == true);  // unchanged
        CHECK(f.out.str().find("ON") != std::string::npos);
    }

    TEST_CASE("/advisor with whitespace-only arg toggles (treated as no-arg)") {
        Fixture f("batbox_adv_ws");
        auto ctx = f.make_ctx();
        auto res = f.registry.lookup("advisor")->execute("   ", ctx);
        REQUIRE(res.has_value());
        CHECK(ctx.advisor_mode == true);  // toggled from false
    }

    TEST_CASE("/advisor unknown subcommand returns Err") {
        Fixture f("batbox_adv_unknown");
        auto ctx = f.make_ctx();
        auto res = f.registry.lookup("advisor")->execute("suggest", ctx);
        CHECK_FALSE(res.has_value());
        CHECK(res.error().find("unknown subcommand") != std::string::npos);
    }
}
