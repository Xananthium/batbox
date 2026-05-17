// tests/unit/test_todo_write.cpp
//
// doctest suite for batbox::tools::TodoWriteTool (CPP 5.15).
//
// Tests:
//   - name() / description() / schema_json() contract
//   - Successful single write
//   - Previous list returned correctly on subsequent write
//   - Status enum validation (pending / in_progress / completed)
//   - Empty content rejected
//   - Empty activeForm rejected
//   - At-most-one in_progress enforced
//   - Replaces full list (not partial update)
//   - All-completed clears stored list
//   - Cancellation returns error immediately
//   - is_read_only() == false, requires_confirmation() == false
//   - Different session keys are isolated
//   - Empty todos array is valid (clears list)

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/tools/TodoWriteTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/permissions/PermissionMode.hpp>

#include <filesystem>
#include <string>
#include <vector>

using namespace batbox;
using namespace batbox::tools;
using namespace batbox::permissions;

// =============================================================================
// Helpers
// =============================================================================

static ToolContext make_ctx(const std::string& session_id = "sess-1",
                            PermissionMode mode = PermissionMode::Default) {
    ToolContext ctx;
    ctx.cwd        = std::filesystem::current_path();
    ctx.mode       = mode;
    ctx.session_id = session_id;
    ctx.agent_id   = "";
    return ctx;
}

static Json make_item(std::string_view content,
                      std::string_view status,
                      std::string_view activeForm) {
    return Json{
        {"content",    content},
        {"status",     status},
        {"activeForm", activeForm}
    };
}

// RAII helper: clears the session store key on destruction so tests are isolated.
struct SessionGuard {
    explicit SessionGuard(const std::string& key) : key_(key) {}
    ~SessionGuard() { TodoWriteTool::clear_todos(key_); }
    std::string key_;
};

// =============================================================================
// TEST SUITE: Identity contract
// =============================================================================
TEST_SUITE("TodoWriteTool — identity") {

    TEST_CASE("name() returns 'TodoWrite'") {
        TodoWriteTool t;
        CHECK(t.name() == std::string_view("TodoWrite"));
    }

    TEST_CASE("description() is non-empty") {
        TodoWriteTool t;
        CHECK_FALSE(t.description().empty());
    }

    TEST_CASE("schema_json() contains name, description, parameters") {
        TodoWriteTool t;
        const Json s = t.schema_json();
        REQUIRE(s.is_object());
        REQUIRE(s.contains("name"));
        REQUIRE(s.contains("description"));
        REQUIRE(s.contains("parameters"));
        CHECK(s["name"].get<std::string>() == "TodoWrite");
        CHECK(s["parameters"]["type"].get<std::string>() == "object");
    }

    TEST_CASE("schema name matches name()") {
        TodoWriteTool t;
        const Json s = t.schema_json();
        CHECK(s["name"].get<std::string>() == std::string(t.name()));
    }

    TEST_CASE("schema todos items contain required fields") {
        TodoWriteTool t;
        const Json s = t.schema_json();
        const Json& items = s["parameters"]["properties"]["todos"]["items"];
        REQUIRE(items.is_object());
        REQUIRE(items.contains("properties"));
        const Json& props = items["properties"];
        CHECK(props.contains("content"));
        CHECK(props.contains("status"));
        CHECK(props.contains("activeForm"));
    }

    TEST_CASE("schema status enum has three allowed values") {
        TodoWriteTool t;
        const Json schema = t.schema_json();
        const Json& status_enum = schema
            ["parameters"]["properties"]["todos"]["items"]
            ["properties"]["status"]["enum"];
        REQUIRE(status_enum.is_array());
        CHECK(status_enum.size() == 3);
        CHECK(status_enum[0].get<std::string>() == "pending");
        CHECK(status_enum[1].get<std::string>() == "in_progress");
        CHECK(status_enum[2].get<std::string>() == "completed");
    }

    TEST_CASE("is_read_only() returns false") {
        TodoWriteTool t;
        CHECK_FALSE(t.is_read_only());
    }

    TEST_CASE("requires_confirmation() returns false") {
        TodoWriteTool t;
        CHECK_FALSE(t.requires_confirmation());
    }
}

// =============================================================================
// TEST SUITE: Successful writes
// =============================================================================
TEST_SUITE("TodoWriteTool — successful writes") {

    TEST_CASE("single item write returns ok with empty previous list") {
        SessionGuard g("sess-write-1");
        TodoWriteTool t;
        auto ctx = make_ctx("sess-write-1");

        Json args = Json{
            {"todos", Json::array({
                make_item("Write the report", "in_progress", "active")
            })}
        };

        ToolResult r = t.run(args, ctx);
        CHECK_FALSE(r.is_error);

        // Body is valid JSON
        const Json body = Json::parse(r.body);
        REQUIRE(body.contains("previous"));
        REQUIRE(body.contains("current"));
        CHECK(body["previous"].empty());
        CHECK(body["current"].size() == 1);
        CHECK(body["current"][0]["content"].get<std::string>() == "Write the report");
        CHECK(body["current"][0]["status"].get<std::string>()  == "in_progress");
    }

    TEST_CASE("previous list is returned correctly on second write") {
        SessionGuard g("sess-write-2");
        TodoWriteTool t;
        auto ctx = make_ctx("sess-write-2");

        // First write
        Json first_args = Json{{"todos", Json::array({
            make_item("Task A", "pending", "form-A"),
            make_item("Task B", "pending", "form-B")
        })}};
        ToolResult r1 = t.run(first_args, ctx);
        CHECK_FALSE(r1.is_error);

        // Second write
        Json second_args = Json{{"todos", Json::array({
            make_item("Task A", "completed", "form-A"),
            make_item("Task B", "in_progress", "form-B")
        })}};
        ToolResult r2 = t.run(second_args, ctx);
        CHECK_FALSE(r2.is_error);

        const Json body = Json::parse(r2.body);
        REQUIRE(body["previous"].size() == 2);
        CHECK(body["previous"][0]["content"].get<std::string>() == "Task A");
        CHECK(body["previous"][0]["status"].get<std::string>()  == "pending");
        CHECK(body["current"].size() == 2);
        CHECK(body["current"][1]["status"].get<std::string>() == "in_progress");
    }

    TEST_CASE("write with all statuses — pending, in_progress, completed") {
        SessionGuard g("sess-write-3");
        TodoWriteTool t;
        auto ctx = make_ctx("sess-write-3");

        Json args = Json{{"todos", Json::array({
            make_item("Done item",    "completed",  "form-1"),
            make_item("Current item", "in_progress","form-2"),
            make_item("Future item",  "pending",    "form-3")
        })}};

        ToolResult r = t.run(args, ctx);
        CHECK_FALSE(r.is_error);
        const Json body = Json::parse(r.body);
        CHECK(body["current"].size() == 3);
    }

    TEST_CASE("structured_payload matches body content") {
        SessionGuard g("sess-write-4");
        TodoWriteTool t;
        auto ctx = make_ctx("sess-write-4");

        Json args = Json{{"todos", Json::array({
            make_item("Item X", "pending", "form-x")
        })}};

        ToolResult r = t.run(args, ctx);
        CHECK_FALSE(r.is_error);
        REQUIRE(r.structured_payload.has_value());
        CHECK(r.structured_payload->contains("previous"));
        CHECK(r.structured_payload->contains("current"));
        CHECK((*r.structured_payload)["current"].size() == 1);
    }

    TEST_CASE("replaces full list — old items not preserved") {
        SessionGuard g("sess-write-5");
        TodoWriteTool t;
        auto ctx = make_ctx("sess-write-5");

        // Write 3 items
        ToolResult r1 = t.run(Json{{"todos", Json::array({
            make_item("A", "pending", "f"),
            make_item("B", "pending", "f"),
            make_item("C", "pending", "f")
        })}}, ctx);
        CHECK_FALSE(r1.is_error);

        // Overwrite with 1 item
        ToolResult r2 = t.run(Json{{"todos", Json::array({
            make_item("Only item", "in_progress", "f")
        })}}, ctx);
        CHECK_FALSE(r2.is_error);

        const Json body = Json::parse(r2.body);
        CHECK(body["previous"].size() == 3);
        CHECK(body["current"].size() == 1);
        CHECK(body["current"][0]["content"].get<std::string>() == "Only item");
    }

    TEST_CASE("empty todos array is valid — clears the list") {
        SessionGuard g("sess-write-6");
        TodoWriteTool t;
        auto ctx = make_ctx("sess-write-6");

        // Write then clear
        (void)t.run(Json{{"todos", Json::array({
            make_item("Something", "pending", "f")
        })}}, ctx);

        ToolResult r = t.run(Json{{"todos", Json::array()}}, ctx);
        CHECK_FALSE(r.is_error);

        const Json body = Json::parse(r.body);
        CHECK(body["current"].empty());

        // Store should also be empty
        CHECK(TodoWriteTool::get_todos("sess-write-6").empty());
    }

    TEST_CASE("all-completed clears stored list") {
        SessionGuard g("sess-write-7");
        TodoWriteTool t;
        auto ctx = make_ctx("sess-write-7");

        ToolResult r = t.run(Json{{"todos", Json::array({
            make_item("Done A", "completed", "f"),
            make_item("Done B", "completed", "f")
        })}}, ctx);
        CHECK_FALSE(r.is_error);

        // After all-completed, stored list should be empty
        const auto stored = TodoWriteTool::get_todos("sess-write-7");
        CHECK(stored.empty());

        // Body "current" should also be empty
        const Json body = Json::parse(r.body);
        CHECK(body["current"].empty());
    }

    TEST_CASE("get_todos returns current stored list") {
        SessionGuard g("sess-write-8");
        TodoWriteTool t;
        auto ctx = make_ctx("sess-write-8");

        (void)t.run(Json{{"todos", Json::array({
            make_item("Track me", "in_progress", "form-tm")
        })}}, ctx);

        const auto stored = TodoWriteTool::get_todos("sess-write-8");
        REQUIRE(stored.size() == 1);
        CHECK(stored[0].content    == "Track me");
        CHECK(stored[0].status     == "in_progress");
        CHECK(stored[0].activeForm == "form-tm");
    }
}

// =============================================================================
// TEST SUITE: Validation errors
// =============================================================================
TEST_SUITE("TodoWriteTool — validation errors") {

    TEST_CASE("missing 'todos' key returns error") {
        TodoWriteTool t;
        auto ctx = make_ctx("sess-val-1");
        ToolResult r = t.run(Json::object(), ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("todos") != std::string::npos);
    }

    TEST_CASE("'todos' not an array returns error") {
        TodoWriteTool t;
        auto ctx = make_ctx("sess-val-2");
        ToolResult r = t.run(Json{{"todos", "not_an_array"}}, ctx);
        CHECK(r.is_error);
    }

    TEST_CASE("item with invalid status returns error") {
        SessionGuard g("sess-val-3");
        TodoWriteTool t;
        auto ctx = make_ctx("sess-val-3");

        Json bad_item = Json{
            {"content",    "Task"},
            {"status",     "in_flight"},   // invalid
            {"activeForm", "form-x"}
        };
        ToolResult r = t.run(Json{{"todos", Json::array({bad_item})}}, ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("invalid") != std::string::npos);
    }

    TEST_CASE("item with empty content returns error") {
        SessionGuard g("sess-val-4");
        TodoWriteTool t;
        auto ctx = make_ctx("sess-val-4");

        Json bad_item = Json{
            {"content",    ""},       // empty
            {"status",     "pending"},
            {"activeForm", "form-x"}
        };
        ToolResult r = t.run(Json{{"todos", Json::array({bad_item})}}, ctx);
        CHECK(r.is_error);
    }

    TEST_CASE("item with empty activeForm returns error") {
        SessionGuard g("sess-val-5");
        TodoWriteTool t;
        auto ctx = make_ctx("sess-val-5");

        Json bad_item = Json{
            {"content",    "Some task"},
            {"status",     "pending"},
            {"activeForm", ""}         // empty
        };
        ToolResult r = t.run(Json{{"todos", Json::array({bad_item})}}, ctx);
        CHECK(r.is_error);
    }

    TEST_CASE("item missing content key returns error") {
        SessionGuard g("sess-val-6");
        TodoWriteTool t;
        auto ctx = make_ctx("sess-val-6");

        Json bad_item = Json{
            {"status",     "pending"},
            {"activeForm", "form-x"}
            // content absent
        };
        ToolResult r = t.run(Json{{"todos", Json::array({bad_item})}}, ctx);
        CHECK(r.is_error);
    }

    TEST_CASE("item missing status key returns error") {
        SessionGuard g("sess-val-7");
        TodoWriteTool t;
        auto ctx = make_ctx("sess-val-7");

        Json bad_item = Json{
            {"content",    "Task"},
            {"activeForm", "form-x"}
            // status absent
        };
        ToolResult r = t.run(Json{{"todos", Json::array({bad_item})}}, ctx);
        CHECK(r.is_error);
    }

    TEST_CASE("item missing activeForm key returns error") {
        SessionGuard g("sess-val-8");
        TodoWriteTool t;
        auto ctx = make_ctx("sess-val-8");

        Json bad_item = Json{
            {"content", "Task"},
            {"status",  "pending"}
            // activeForm absent
        };
        ToolResult r = t.run(Json{{"todos", Json::array({bad_item})}}, ctx);
        CHECK(r.is_error);
    }

    TEST_CASE("two in_progress items returns error") {
        SessionGuard g("sess-val-9");
        TodoWriteTool t;
        auto ctx = make_ctx("sess-val-9");

        Json args = Json{{"todos", Json::array({
            make_item("Task A", "in_progress", "f"),
            make_item("Task B", "in_progress", "f")   // second in_progress
        })}};
        ToolResult r = t.run(args, ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("in_progress") != std::string::npos);
    }

    TEST_CASE("three in_progress items returns error mentioning count") {
        SessionGuard g("sess-val-10");
        TodoWriteTool t;
        auto ctx = make_ctx("sess-val-10");

        Json args = Json{{"todos", Json::array({
            make_item("A", "in_progress", "f"),
            make_item("B", "in_progress", "f"),
            make_item("C", "in_progress", "f")
        })}};
        ToolResult r = t.run(args, ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("3") != std::string::npos);
    }

    TEST_CASE("non-object item in array returns error") {
        SessionGuard g("sess-val-11");
        TodoWriteTool t;
        auto ctx = make_ctx("sess-val-11");

        Json args = Json{{"todos", Json::array({"not_an_object"})}};
        ToolResult r = t.run(args, ctx);
        CHECK(r.is_error);
    }

    TEST_CASE("error does not mutate existing stored list") {
        SessionGuard g("sess-val-12");
        TodoWriteTool t;
        auto ctx = make_ctx("sess-val-12");

        // Store a good list
        (void)t.run(Json{{"todos", Json::array({
            make_item("Good item", "pending", "f")
        })}}, ctx);

        // Attempt bad write
        Json bad = Json{{"todos", Json::array({
            make_item("Bad", "flying", "f")  // invalid status
        })}};
        ToolResult r = t.run(bad, ctx);
        REQUIRE(r.is_error);

        // Original list must be intact
        const auto stored = TodoWriteTool::get_todos("sess-val-12");
        REQUIRE(stored.size() == 1);
        CHECK(stored[0].content == "Good item");
    }
}

// =============================================================================
// TEST SUITE: Session isolation
// =============================================================================
TEST_SUITE("TodoWriteTool — session isolation") {

    TEST_CASE("different session keys are isolated") {
        SessionGuard g1("sess-iso-1");
        SessionGuard g2("sess-iso-2");
        TodoWriteTool t;

        auto ctx1 = make_ctx("sess-iso-1");
        auto ctx2 = make_ctx("sess-iso-2");

        (void)t.run(Json{{"todos", Json::array({
            make_item("Session 1 task", "pending", "f")
        })}}, ctx1);

        (void)t.run(Json{{"todos", Json::array({
            make_item("Session 2 task", "in_progress", "f")
        })}}, ctx2);

        const auto s1 = TodoWriteTool::get_todos("sess-iso-1");
        const auto s2 = TodoWriteTool::get_todos("sess-iso-2");

        REQUIRE(s1.size() == 1);
        REQUIRE(s2.size() == 1);
        CHECK(s1[0].content == "Session 1 task");
        CHECK(s2[0].content == "Session 2 task");
    }

    TEST_CASE("session_id takes precedence over agent_id as key") {
        SessionGuard g("sid-over-aid");
        TodoWriteTool t;

        ToolContext ctx;
        ctx.cwd        = std::filesystem::current_path();
        ctx.session_id = "sid-over-aid";
        ctx.agent_id   = "agent-xyz";
        ctx.mode       = PermissionMode::Default;

        (void)t.run(Json{{"todos", Json::array({
            make_item("Keyed by session_id", "pending", "f")
        })}}, ctx);

        // Key should be session_id
        const auto by_sid = TodoWriteTool::get_todos("sid-over-aid");
        const auto by_aid = TodoWriteTool::get_todos("agent-xyz");

        CHECK(by_sid.size() == 1);
        CHECK(by_aid.empty());
    }

    TEST_CASE("empty session_id falls back to agent_id") {
        SessionGuard g("agent-fallback");
        TodoWriteTool t;

        ToolContext ctx;
        ctx.cwd        = std::filesystem::current_path();
        ctx.session_id = "";             // empty
        ctx.agent_id   = "agent-fallback";
        ctx.mode       = PermissionMode::Default;

        (void)t.run(Json{{"todos", Json::array({
            make_item("Keyed by agent_id", "pending", "f")
        })}}, ctx);

        const auto stored = TodoWriteTool::get_todos("agent-fallback");
        REQUIRE(stored.size() == 1);
        CHECK(stored[0].content == "Keyed by agent_id");
    }

    TEST_CASE("clear_todos removes only specified key") {
        SessionGuard g1("sess-clr-1");
        SessionGuard g2("sess-clr-2");
        TodoWriteTool t;

        auto ctx1 = make_ctx("sess-clr-1");
        auto ctx2 = make_ctx("sess-clr-2");
        (void)t.run(Json{{"todos", Json::array({make_item("A", "pending", "f")})}}, ctx1);
        (void)t.run(Json{{"todos", Json::array({make_item("B", "pending", "f")})}}, ctx2);

        TodoWriteTool::clear_todos("sess-clr-1");

        CHECK(TodoWriteTool::get_todos("sess-clr-1").empty());
        CHECK(TodoWriteTool::get_todos("sess-clr-2").size() == 1);
    }
}

// =============================================================================
// TEST SUITE: Cancellation
// =============================================================================
TEST_SUITE("TodoWriteTool — cancellation") {

    TEST_CASE("cancelled context returns error without mutating state") {
        SessionGuard g("sess-cancel-1");
        TodoWriteTool t;

        auto [src, tok] = CancelToken::make_root();
        src.request_stop();

        ToolContext ctx = make_ctx("sess-cancel-1");
        ctx.cancel_token = std::move(tok);

        Json args = Json{{"todos", Json::array({
            make_item("Should not be stored", "pending", "f")
        })}};

        ToolResult r = t.run(args, ctx);
        CHECK(r.is_error);
        CHECK(r.body == "cancelled");

        // Nothing should be stored
        CHECK(TodoWriteTool::get_todos("sess-cancel-1").empty());
    }
}

// =============================================================================
// TEST SUITE: TodoItem helpers
// =============================================================================
TEST_SUITE("TodoItem — from_json and to_json") {

    TEST_CASE("from_json succeeds with all valid fields") {
        Json j = make_item("Buy groceries", "pending", "shopping");
        TodoItem out;
        CHECK(TodoItem::from_json(j, out));
        CHECK(out.content    == "Buy groceries");
        CHECK(out.status     == "pending");
        CHECK(out.activeForm == "shopping");
    }

    TEST_CASE("from_json fails on non-object input") {
        TodoItem out;
        CHECK_FALSE(TodoItem::from_json(Json("string"), out));
        CHECK_FALSE(TodoItem::from_json(Json(42), out));
        CHECK_FALSE(TodoItem::from_json(Json::array(), out));
    }

    TEST_CASE("from_json fails when content is missing") {
        Json j = {{"status", "pending"}, {"activeForm", "f"}};
        TodoItem out;
        CHECK_FALSE(TodoItem::from_json(j, out));
    }

    TEST_CASE("from_json fails when status is missing") {
        Json j = {{"content", "Task"}, {"activeForm", "f"}};
        TodoItem out;
        CHECK_FALSE(TodoItem::from_json(j, out));
    }

    TEST_CASE("from_json fails when activeForm is missing") {
        Json j = {{"content", "Task"}, {"status", "pending"}};
        TodoItem out;
        CHECK_FALSE(TodoItem::from_json(j, out));
    }

    TEST_CASE("from_json fails on empty content") {
        Json j = {{"content", ""}, {"status", "pending"}, {"activeForm", "f"}};
        TodoItem out;
        CHECK_FALSE(TodoItem::from_json(j, out));
    }

    TEST_CASE("from_json fails on empty activeForm") {
        Json j = {{"content", "Task"}, {"status", "completed"}, {"activeForm", ""}};
        TodoItem out;
        CHECK_FALSE(TodoItem::from_json(j, out));
    }

    TEST_CASE("from_json accepts all three valid status values") {
        for (const char* s : {"pending", "in_progress", "completed"}) {
            Json j = {{"content", "Task"}, {"status", s}, {"activeForm", "f"}};
            TodoItem out;
            CHECK(TodoItem::from_json(j, out));
            CHECK(out.status == s);
        }
    }

    TEST_CASE("to_json produces correct fields") {
        TodoItem item;
        item.content    = "Review PR";
        item.status     = "in_progress";
        item.activeForm = "review-form";
        const Json j = item.to_json();
        CHECK(j["content"].get<std::string>()    == "Review PR");
        CHECK(j["status"].get<std::string>()     == "in_progress");
        CHECK(j["activeForm"].get<std::string>() == "review-form");
    }

    TEST_CASE("round-trip from_json → to_json preserves data") {
        Json original = make_item("Deploy app", "completed", "deploy-form");
        TodoItem out;
        REQUIRE(TodoItem::from_json(original, out));
        const Json round_tripped = out.to_json();
        CHECK(round_tripped == original);
    }
}
