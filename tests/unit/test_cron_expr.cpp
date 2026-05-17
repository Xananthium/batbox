// tests/unit/test_cron_expr.cpp
//
// doctest unit tests for CronExpr (5-field parser) and CronScheduler
// CRUD operations.
//
// CPP 5.17 acceptance criteria covered here:
//   [x] CronCreate accepts standard 5-field cron expression
//   [x] CronList returns all entries
//   [x] CronDelete removes by id
//   [x] Unit tests for cron-expr parser

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/tools/CronScheduler.hpp>
#include <batbox/tools/CronCreateTool.hpp>
#include <batbox/tools/CronDeleteTool.hpp>
#include <batbox/tools/CronListTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/permissions/PermissionMode.hpp>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <ctime>

namespace fs = std::filesystem;
using namespace batbox;
using namespace batbox::tools;
using namespace batbox::permissions;

// =============================================================================
// Test infrastructure
// =============================================================================

/// RAII temp-directory redirecting HOME so CronScheduler::default_path()
/// points to a safe test-only location.
struct ScopedHome {
    std::string original_home;
    fs::path    tmp_dir;

    ScopedHome() {
        const char* h = std::getenv("HOME");
        original_home = h ? h : "";
        tmp_dir = fs::temp_directory_path()
                / ("batbox_cron_test_" + std::to_string(
                       static_cast<unsigned long>(std::time(nullptr))));
        fs::create_directories(tmp_dir);
#if defined(_WIN32)
        _putenv_s("HOME", tmp_dir.string().c_str());
#else
        setenv("HOME", tmp_dir.string().c_str(), 1);
#endif
    }

    ~ScopedHome() {
        // Restore HOME.
        if (!original_home.empty()) {
#if defined(_WIN32)
            _putenv_s("HOME", original_home.c_str());
#else
            setenv("HOME", original_home.c_str(), 1);
#endif
        }
        // Clean up temp dir.
        std::error_code ec;
        fs::remove_all(tmp_dir, ec);
    }
};

/// Build a default ToolContext for tests.
static ToolContext make_ctx(PermissionMode mode = PermissionMode::Nuclear) {
    ToolContext ctx;
    ctx.cwd  = fs::temp_directory_path();
    ctx.mode = mode;
    return ctx;
}

// =============================================================================
// CronExpr: parse — wildcards and integers
// =============================================================================

TEST_SUITE("CronExpr::parse") {

    TEST_CASE("all wildcards parses successfully") {
        CronExpr expr;
        CHECK(CronExpr::parse("* * * * *", expr));
        CHECK(expr.minute.is_wildcard);
        CHECK(expr.hour.is_wildcard);
        CHECK(expr.dom.is_wildcard);
        CHECK(expr.month.is_wildcard);
        CHECK(expr.dow.is_wildcard);
    }

    TEST_CASE("integer fields parse correctly") {
        CronExpr expr;
        REQUIRE(CronExpr::parse("30 9 15 6 1", expr));
        CHECK_FALSE(expr.minute.is_wildcard);
        REQUIRE(expr.minute.values.size() == 1);
        CHECK(expr.minute.values[0] == 30);

        REQUIRE(expr.hour.values.size() == 1);
        CHECK(expr.hour.values[0] == 9);

        REQUIRE(expr.dom.values.size() == 1);
        CHECK(expr.dom.values[0] == 15);

        REQUIRE(expr.month.values.size() == 1);
        CHECK(expr.month.values[0] == 6);

        REQUIRE(expr.dow.values.size() == 1);
        CHECK(expr.dow.values[0] == 1);
    }

    TEST_CASE("comma-separated list") {
        CronExpr expr;
        REQUIRE(CronExpr::parse("0,15,30,45 * * * *", expr));
        CHECK_FALSE(expr.minute.is_wildcard);
        REQUIRE(expr.minute.values.size() == 4);
        CHECK(expr.minute.values[0] == 0);
        CHECK(expr.minute.values[1] == 15);
        CHECK(expr.minute.values[2] == 30);
        CHECK(expr.minute.values[3] == 45);
    }

    TEST_CASE("step syntax */15") {
        CronExpr expr;
        REQUIRE(CronExpr::parse("*/15 * * * *", expr));
        CHECK_FALSE(expr.minute.is_wildcard);
        REQUIRE(expr.minute.values.size() == 4);
        CHECK(expr.minute.values[0] == 0);
        CHECK(expr.minute.values[1] == 15);
        CHECK(expr.minute.values[2] == 30);
        CHECK(expr.minute.values[3] == 45);
    }

    TEST_CASE("range A-B") {
        CronExpr expr;
        REQUIRE(CronExpr::parse("* * * * 1-5", expr));
        REQUIRE(expr.dow.values.size() == 5);
        for (int i = 0; i < 5; ++i) {
            CHECK(expr.dow.values[i] == i + 1);
        }
    }

    TEST_CASE("range with step A-B/N") {
        CronExpr expr;
        REQUIRE(CronExpr::parse("0-30/10 * * * *", expr));
        // 0,10,20,30
        REQUIRE(expr.minute.values.size() == 4);
        CHECK(expr.minute.values[0] == 0);
        CHECK(expr.minute.values[1] == 10);
        CHECK(expr.minute.values[2] == 20);
        CHECK(expr.minute.values[3] == 30);
    }

    TEST_CASE("too few fields fails") {
        CronExpr expr;
        CHECK_FALSE(CronExpr::parse("* * * *", expr));
    }

    TEST_CASE("too many fields fails") {
        CronExpr expr;
        CHECK_FALSE(CronExpr::parse("* * * * * *", expr));
    }

    TEST_CASE("empty string fails") {
        CronExpr expr;
        CHECK_FALSE(CronExpr::parse("", expr));
    }

    TEST_CASE("out-of-range minute fails") {
        CronExpr expr;
        CHECK_FALSE(CronExpr::parse("60 * * * *", expr));
    }

    TEST_CASE("out-of-range hour fails") {
        CronExpr expr;
        CHECK_FALSE(CronExpr::parse("* 24 * * *", expr));
    }

    TEST_CASE("out-of-range month fails") {
        CronExpr expr;
        CHECK_FALSE(CronExpr::parse("* * * 13 *", expr));
    }

    TEST_CASE("zero-step fails") {
        CronExpr expr;
        CHECK_FALSE(CronExpr::parse("*/0 * * * *", expr));
    }

    TEST_CASE("non-numeric field fails") {
        CronExpr expr;
        CHECK_FALSE(CronExpr::parse("abc * * * *", expr));
    }
}

// =============================================================================
// CronField::matches
// =============================================================================

TEST_SUITE("CronField::matches") {

    TEST_CASE("wildcard matches any value") {
        CronField f;
        f.is_wildcard = true;
        CHECK(f.matches(0));
        CHECK(f.matches(59));
        CHECK(f.matches(100));
    }

    TEST_CASE("non-wildcard matches only listed values") {
        CronField f;
        f.is_wildcard = false;
        f.values = {5, 10, 15};
        CHECK(f.matches(5));
        CHECK(f.matches(10));
        CHECK(f.matches(15));
        CHECK_FALSE(f.matches(0));
        CHECK_FALSE(f.matches(7));
    }
}

// =============================================================================
// CronExpr::next_fire
// =============================================================================

TEST_SUITE("CronExpr::next_fire") {

    TEST_CASE("next fire after known time: every minute") {
        CronExpr expr;
        REQUIRE(CronExpr::parse("* * * * *", expr));

        // Pick a known UTC epoch second at a minute boundary.
        // 2020-01-01 00:00:00 UTC = 1577836800
        const std::time_t base = 1577836800;
        auto nf = expr.next_fire(base);
        REQUIRE(nf.has_value());
        // Should fire at base + 60 (next minute).
        CHECK(*nf == base + 60);
    }

    TEST_CASE("specific minute fires at correct minute") {
        CronExpr expr;
        REQUIRE(CronExpr::parse("30 * * * *", expr));

        // 2020-01-01 00:00:00 UTC = 1577836800 (minute=0)
        const std::time_t base = 1577836800;
        auto nf = expr.next_fire(base);
        REQUIRE(nf.has_value());
        // Should fire at 00:30:00 = base + 30*60
        CHECK(*nf == base + 30 * 60);
    }

    TEST_CASE("specific hour fires at correct hour") {
        CronExpr expr;
        REQUIRE(CronExpr::parse("0 9 * * *", expr));

        // 2020-01-01 00:00:00 UTC = 1577836800
        const std::time_t base = 1577836800;
        auto nf = expr.next_fire(base);
        REQUIRE(nf.has_value());
        // Should fire at 09:00:00 same day = base + 9*3600
        CHECK(*nf == base + 9 * 3600);
    }
}

// =============================================================================
// CronEntry serialisation
// =============================================================================

TEST_SUITE("CronEntry serialisation") {

    TEST_CASE("to_json / from_json round-trip") {
        CronEntry e;
        e.id         = "test-uuid-1234";
        e.expression = "0 8 * * 1-5";
        e.prompt     = "Daily standup";
        e.enabled    = true;
        e.created_at = "2026-01-01T00:00:00Z";

        const Json j = e.to_json();
        CHECK(j["id"].get<std::string>() == e.id);
        CHECK(j["expression"].get<std::string>() == e.expression);
        CHECK(j["prompt"].get<std::string>() == e.prompt);
        CHECK(j["enabled"].get<bool>() == e.enabled);
        CHECK(j["created_at"].get<std::string>() == e.created_at);

        CronEntry out;
        REQUIRE(CronEntry::from_json(j, out));
        CHECK(out.id         == e.id);
        CHECK(out.expression == e.expression);
        CHECK(out.prompt     == e.prompt);
        CHECK(out.enabled    == e.enabled);
        CHECK(out.created_at == e.created_at);
    }

    TEST_CASE("from_json missing id returns false") {
        Json j{{"expression", "* * * * *"}, {"prompt", "test"}};
        CronEntry e;
        CHECK_FALSE(CronEntry::from_json(j, e));
    }

    TEST_CASE("from_json missing expression returns false") {
        Json j{{"id", "abc"}, {"prompt", "test"}};
        CronEntry e;
        CHECK_FALSE(CronEntry::from_json(j, e));
    }

    TEST_CASE("from_json enabled defaults to true when absent") {
        Json j{{"id", "abc"}, {"expression", "* * * * *"}, {"prompt", "p"}};
        CronEntry e;
        REQUIRE(CronEntry::from_json(j, e));
        CHECK(e.enabled == true);
    }
}

// =============================================================================
// CronScheduler — CRUD
// =============================================================================

TEST_SUITE("CronScheduler CRUD") {

    TEST_CASE("create_entry stores entry and list_entries returns it") {
        ScopedHome home;
        auto sched = std::make_shared<CronScheduler>(CronScheduler::default_path());

        auto opt = sched->create_entry("* * * * *", "test prompt");
        REQUIRE(opt.has_value());
        CHECK_FALSE(opt->id.empty());
        CHECK(opt->expression == "* * * * *");
        CHECK(opt->prompt == "test prompt");
        CHECK(opt->enabled == true);
        CHECK_FALSE(opt->created_at.empty());

        const auto entries = sched->list_entries();
        REQUIRE(entries.size() == 1);
        CHECK(entries[0].id == opt->id);
    }

    TEST_CASE("create_entry with invalid expression returns nullopt") {
        ScopedHome home;
        auto sched = std::make_shared<CronScheduler>(CronScheduler::default_path());
        auto opt = sched->create_entry("not a cron expr", "prompt");
        CHECK_FALSE(opt.has_value());
    }

    TEST_CASE("create_entry with empty prompt returns nullopt") {
        ScopedHome home;
        auto sched = std::make_shared<CronScheduler>(CronScheduler::default_path());
        auto opt = sched->create_entry("* * * * *", "");
        CHECK_FALSE(opt.has_value());
    }

    TEST_CASE("delete_entry removes the entry") {
        ScopedHome home;
        auto sched = std::make_shared<CronScheduler>(CronScheduler::default_path());

        auto opt = sched->create_entry("0 9 * * *", "morning");
        REQUIRE(opt.has_value());

        CHECK(sched->delete_entry(opt->id));
        CHECK(sched->list_entries().empty());
    }

    TEST_CASE("delete_entry non-existent id returns false") {
        ScopedHome home;
        auto sched = std::make_shared<CronScheduler>(CronScheduler::default_path());
        CHECK_FALSE(sched->delete_entry("no-such-id"));
    }

    TEST_CASE("list_entries returns empty when file absent") {
        ScopedHome home;
        auto sched = std::make_shared<CronScheduler>(CronScheduler::default_path());
        CHECK(sched->list_entries().empty());
    }

    TEST_CASE("persistence: separate instance reads entries") {
        ScopedHome home;
        const auto cron_path = CronScheduler::default_path();
        {
            auto sched = std::make_shared<CronScheduler>(cron_path);
            auto opt = sched->create_entry("*/5 * * * *", "every 5 mins");
            REQUIRE(opt.has_value());
        }
        // New instance reads the same file.
        auto sched2 = std::make_shared<CronScheduler>(cron_path);
        const auto entries = sched2->list_entries();
        REQUIRE(entries.size() == 1);
        CHECK(entries[0].expression == "*/5 * * * *");
        CHECK(entries[0].prompt == "every 5 mins");
    }

    TEST_CASE("multiple entries survive save/load cycle") {
        ScopedHome home;
        auto sched = std::make_shared<CronScheduler>(CronScheduler::default_path());
        sched->create_entry("0 6 * * *", "morning");
        sched->create_entry("0 12 * * *", "noon");
        sched->create_entry("0 18 * * *", "evening");

        const auto entries = sched->list_entries();
        REQUIRE(entries.size() == 3);
    }

    TEST_CASE("create_entry with enabled=false persists as disabled") {
        ScopedHome home;
        auto sched = std::make_shared<CronScheduler>(CronScheduler::default_path());
        auto opt = sched->create_entry("* * * * *", "disabled task", false);
        REQUIRE(opt.has_value());
        CHECK_FALSE(opt->enabled);

        const auto entries = sched->list_entries();
        REQUIRE(entries.size() == 1);
        CHECK_FALSE(entries[0].enabled);
    }

    TEST_CASE("default_path is under ~/.batbox/") {
        ScopedHome home;
        const auto p = CronScheduler::default_path();
        const std::string ps = p.string();
        CHECK(ps.find(".batbox") != std::string::npos);
        CHECK(ps.find("cron.json") != std::string::npos);
    }
}

// =============================================================================
// CronCreateTool — ITool contract
// =============================================================================

TEST_SUITE("CronCreateTool ITool contract") {

    TEST_CASE("name is CronCreate") {
        ScopedHome home;
        auto sched = std::make_shared<CronScheduler>(CronScheduler::default_path());
        CronCreateTool tool(sched);
        CHECK(std::string(tool.name()) == "CronCreate");
    }

    TEST_CASE("is_read_only is false") {
        ScopedHome home;
        auto sched = std::make_shared<CronScheduler>(CronScheduler::default_path());
        CronCreateTool tool(sched);
        CHECK_FALSE(tool.is_read_only());
    }

    TEST_CASE("requires_confirmation is false") {
        ScopedHome home;
        auto sched = std::make_shared<CronScheduler>(CronScheduler::default_path());
        CronCreateTool tool(sched);
        CHECK_FALSE(tool.requires_confirmation());
    }

    TEST_CASE("schema_json has correct name and required fields") {
        ScopedHome home;
        auto sched = std::make_shared<CronScheduler>(CronScheduler::default_path());
        CronCreateTool tool(sched);
        const Json schema = tool.schema_json();
        CHECK(schema["name"].get<std::string>() == "CronCreate");
        {
            const auto& req = schema["parameters"]["required"];
            bool has_expr = false, has_prompt = false;
            for (const auto& v : req) {
                if (v.get<std::string>() == "expression") has_expr = true;
                if (v.get<std::string>() == "prompt")     has_prompt = true;
            }
            CHECK(has_expr);
            CHECK(has_prompt);
        }
    }

    TEST_CASE("run: missing expression returns error") {
        ScopedHome home;
        auto sched = std::make_shared<CronScheduler>(CronScheduler::default_path());
        CronCreateTool tool(sched);
        auto ctx = make_ctx();
        Json args{{"prompt", "test"}};
        const auto result = tool.run(args, ctx);
        CHECK(result.is_error);
        CHECK(result.body.find("expression") != std::string::npos);
    }

    TEST_CASE("run: missing prompt returns error") {
        ScopedHome home;
        auto sched = std::make_shared<CronScheduler>(CronScheduler::default_path());
        CronCreateTool tool(sched);
        auto ctx = make_ctx();
        Json args{{"expression", "* * * * *"}};
        const auto result = tool.run(args, ctx);
        CHECK(result.is_error);
        CHECK(result.body.find("prompt") != std::string::npos);
    }

    TEST_CASE("run: invalid expression returns error") {
        ScopedHome home;
        auto sched = std::make_shared<CronScheduler>(CronScheduler::default_path());
        CronCreateTool tool(sched);
        auto ctx = make_ctx();
        Json args{{"expression", "not valid"}, {"prompt", "p"}};
        const auto result = tool.run(args, ctx);
        CHECK(result.is_error);
    }

    TEST_CASE("run: valid args creates entry") {
        ScopedHome home;
        auto sched = std::make_shared<CronScheduler>(CronScheduler::default_path());
        CronCreateTool tool(sched);
        auto ctx = make_ctx();
        Json args{{"expression", "0 9 * * 1-5"}, {"prompt", "standup"}};
        const auto result = tool.run(args, ctx);
        CHECK_FALSE(result.is_error);
        CHECK(result.body.find("standup") != std::string::npos);
        REQUIRE(result.structured_payload.has_value());
        CHECK((*result.structured_payload)["expression"].get<std::string>() == "0 9 * * 1-5");
    }

    TEST_CASE("run: plan mode returns error") {
        ScopedHome home;
        auto sched = std::make_shared<CronScheduler>(CronScheduler::default_path());
        CronCreateTool tool(sched);
        auto ctx = make_ctx(PermissionMode::Plan);
        Json args{{"expression", "* * * * *"}, {"prompt", "test"}};
        const auto result = tool.run(args, ctx);
        CHECK(result.is_error);
        CHECK(result.body.find("Plan mode") != std::string::npos);
    }

    TEST_CASE("run: pre-cancelled returns error") {
        ScopedHome home;
        auto sched = std::make_shared<CronScheduler>(CronScheduler::default_path());
        CronCreateTool tool(sched);
        auto [src, tok] = batbox::CancelToken::make_root();
        src.request_stop();
        ToolContext ctx;
        ctx.cancel_token = std::move(tok);
        ctx.mode = PermissionMode::Nuclear;
        Json args{{"expression", "* * * * *"}, {"prompt", "test"}};
        const auto result = tool.run(args, ctx);
        CHECK(result.is_error);
        CHECK(result.body == "cancelled");
    }
}

// =============================================================================
// CronDeleteTool — ITool contract
// =============================================================================

TEST_SUITE("CronDeleteTool ITool contract") {

    TEST_CASE("name is CronDelete") {
        ScopedHome home;
        auto sched = std::make_shared<CronScheduler>(CronScheduler::default_path());
        CronDeleteTool tool(sched);
        CHECK(std::string(tool.name()) == "CronDelete");
    }

    TEST_CASE("is_read_only is false") {
        ScopedHome home;
        auto sched = std::make_shared<CronScheduler>(CronScheduler::default_path());
        CronDeleteTool tool(sched);
        CHECK_FALSE(tool.is_read_only());
    }

    TEST_CASE("requires_confirmation is false") {
        ScopedHome home;
        auto sched = std::make_shared<CronScheduler>(CronScheduler::default_path());
        CronDeleteTool tool(sched);
        CHECK_FALSE(tool.requires_confirmation());
    }

    TEST_CASE("schema_json has correct name and required id") {
        ScopedHome home;
        auto sched = std::make_shared<CronScheduler>(CronScheduler::default_path());
        CronDeleteTool tool(sched);
        const Json schema = tool.schema_json();
        CHECK(schema["name"].get<std::string>() == "CronDelete");
        {
            const auto& req = schema["parameters"]["required"];
            bool has_id = false;
            for (const auto& v : req) {
                if (v.get<std::string>() == "id") has_id = true;
            }
            CHECK(has_id);
        }
    }

    TEST_CASE("run: missing id returns error") {
        ScopedHome home;
        auto sched = std::make_shared<CronScheduler>(CronScheduler::default_path());
        CronDeleteTool tool(sched);
        auto ctx = make_ctx();
        const auto result = tool.run(Json::object(), ctx);
        CHECK(result.is_error);
        CHECK(result.body.find("id") != std::string::npos);
    }

    TEST_CASE("run: non-existent id returns error") {
        ScopedHome home;
        auto sched = std::make_shared<CronScheduler>(CronScheduler::default_path());
        CronDeleteTool tool(sched);
        auto ctx = make_ctx();
        Json args{{"id", "no-such-uuid"}};
        const auto result = tool.run(args, ctx);
        CHECK(result.is_error);
        CHECK(result.body.find("no-such-uuid") != std::string::npos);
    }

    TEST_CASE("run: valid id deletes entry") {
        ScopedHome home;
        auto sched = std::make_shared<CronScheduler>(CronScheduler::default_path());
        auto opt = sched->create_entry("* * * * *", "to delete");
        REQUIRE(opt.has_value());

        CronDeleteTool tool(sched);
        auto ctx = make_ctx();
        Json args{{"id", opt->id}};
        const auto result = tool.run(args, ctx);
        CHECK_FALSE(result.is_error);
        CHECK(result.body.find(opt->id) != std::string::npos);
        CHECK(sched->list_entries().empty());
    }

    TEST_CASE("run: plan mode returns error") {
        ScopedHome home;
        auto sched = std::make_shared<CronScheduler>(CronScheduler::default_path());
        CronDeleteTool tool(sched);
        auto ctx = make_ctx(PermissionMode::Plan);
        Json args{{"id", "any"}};
        const auto result = tool.run(args, ctx);
        CHECK(result.is_error);
    }
}

// =============================================================================
// CronListTool — ITool contract
// =============================================================================

TEST_SUITE("CronListTool ITool contract") {

    TEST_CASE("name is CronList") {
        ScopedHome home;
        auto sched = std::make_shared<CronScheduler>(CronScheduler::default_path());
        CronListTool tool(sched);
        CHECK(std::string(tool.name()) == "CronList");
    }

    TEST_CASE("is_read_only is true") {
        ScopedHome home;
        auto sched = std::make_shared<CronScheduler>(CronScheduler::default_path());
        CronListTool tool(sched);
        CHECK(tool.is_read_only());
    }

    TEST_CASE("requires_confirmation is false") {
        ScopedHome home;
        auto sched = std::make_shared<CronScheduler>(CronScheduler::default_path());
        CronListTool tool(sched);
        CHECK_FALSE(tool.requires_confirmation());
    }

    TEST_CASE("schema_json has correct name") {
        ScopedHome home;
        auto sched = std::make_shared<CronScheduler>(CronScheduler::default_path());
        CronListTool tool(sched);
        CHECK(tool.schema_json()["name"].get<std::string>() == "CronList");
    }

    TEST_CASE("run: empty returns ok with empty message") {
        ScopedHome home;
        auto sched = std::make_shared<CronScheduler>(CronScheduler::default_path());
        CronListTool tool(sched);
        auto ctx = make_ctx();
        const auto result = tool.run(Json::object(), ctx);
        CHECK_FALSE(result.is_error);
        REQUIRE(result.structured_payload.has_value());
        CHECK((*result.structured_payload)["count"].get<int>() == 0);
    }

    TEST_CASE("run: lists created entries") {
        ScopedHome home;
        auto sched = std::make_shared<CronScheduler>(CronScheduler::default_path());
        sched->create_entry("0 6 * * *", "breakfast");
        sched->create_entry("0 18 * * *", "dinner");

        CronListTool tool(sched);
        auto ctx = make_ctx();
        const auto result = tool.run(Json::object(), ctx);
        CHECK_FALSE(result.is_error);
        REQUIRE(result.structured_payload.has_value());
        CHECK((*result.structured_payload)["count"].get<int>() == 2);
        CHECK(result.body.find("breakfast") != std::string::npos);
        CHECK(result.body.find("dinner") != std::string::npos);
    }

    TEST_CASE("run: pre-cancelled returns error") {
        ScopedHome home;
        auto sched = std::make_shared<CronScheduler>(CronScheduler::default_path());
        CronListTool tool(sched);
        auto [src, tok] = batbox::CancelToken::make_root();
        src.request_stop();
        ToolContext ctx;
        ctx.cancel_token = std::move(tok);
        ctx.mode = PermissionMode::Nuclear;
        const auto result = tool.run(Json::object(), ctx);
        CHECK(result.is_error);
        CHECK(result.body == "cancelled");
    }

    TEST_CASE("run: allowed in plan mode (is_read_only)") {
        ScopedHome home;
        auto sched = std::make_shared<CronScheduler>(CronScheduler::default_path());
        CronListTool tool(sched);
        auto ctx = make_ctx(PermissionMode::Plan);
        const auto result = tool.run(Json::object(), ctx);
        // CronList is read-only — no plan-mode block in tool itself.
        CHECK_FALSE(result.is_error);
    }
}
