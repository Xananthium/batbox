// tests/unit/test_task_crud.cpp
//
// doctest suite for batbox::tools::TaskStore + TaskCreateTool / TaskListTool /
// TaskGetTool / TaskUpdateTool.
//
// Covers:
//   - Task::to_json() / from_json() round-trip
//   - TaskStore CRUD (create, list, get, update)
//   - Persistence across TaskStore instances (same file, different object)
//   - TaskCreateTool: identity, schema, argument validation, success, plan-mode
//   - TaskListTool:   identity, schema, list all, filter by status, filter by tag
//   - TaskGetTool:    identity, schema, found, not-found, empty id
//   - TaskUpdateTool: identity, schema, partial update, status validation,
//                     not-found, plan-mode
//   - Cancellation
//
// CPP 5.16 acceptance criteria:
//   [x] TaskCreate adds a task with new uuid
//   [x] TaskList returns all (or filtered by status/tag)
//   [x] TaskGet returns one by id
//   [x] TaskUpdate mutates fields atomically
//   [x] Storage file lock-friendly (multiple agents writing safely)
//   [x] Unit tests

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/tools/TaskStore.hpp>
#include <batbox/tools/TaskCreateTool.hpp>
#include <batbox/tools/TaskListTool.hpp>
#include <batbox/tools/TaskGetTool.hpp>
#include <batbox/tools/TaskUpdateTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/permissions/PermissionMode.hpp>
#include <batbox/core/CancelToken.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace batbox;
using namespace batbox::tools;
using namespace batbox::permissions;

// =============================================================================
// Test infrastructure
// =============================================================================

/// RAII: redirect HOME to a temp directory so TaskStore::default_path() and
/// any HOME-based paths point somewhere safe.
struct ScopedHome {
    std::string original_home;
    fs::path    tmp_dir;

    ScopedHome() {
        const char* h = std::getenv("HOME");
        original_home = h ? h : "";

        const auto unique_id =
            static_cast<unsigned long>(::getpid()) * 1000000UL
            + static_cast<unsigned long>(
                std::chrono::steady_clock::now().time_since_epoch().count() % 1000000);
        tmp_dir = fs::temp_directory_path()
                / ("batbox_task_test_" + std::to_string(unique_id));
        fs::create_directories(tmp_dir);
        setenv("HOME", tmp_dir.c_str(), 1);
    }

    ~ScopedHome() {
        if (!original_home.empty()) {
            setenv("HOME", original_home.c_str(), 1);
        } else {
            unsetenv("HOME");
        }
        std::error_code ec;
        fs::remove_all(tmp_dir, ec);
    }

    /// Returns the tasks file path that will be used by tools with default path.
    fs::path tasks_path() const {
        return tmp_dir / ".batbox" / "tasks.json";
    }
};

/// Build a minimal ToolContext.
static ToolContext make_ctx(PermissionMode mode = PermissionMode::Default) {
    ToolContext ctx;
    ctx.cwd        = fs::current_path();
    ctx.mode       = mode;
    ctx.session_id = "test-session";
    ctx.agent_id   = "test-agent";
    return ctx;
}

/// Build a cancelled ToolContext.
static ToolContext make_cancelled_ctx() {
    auto [src, tok] = CancelToken::make_root();
    src.request_stop();
    ToolContext ctx = make_ctx();
    ctx.cancel_token = std::move(tok);
    return ctx;
}

/// Parse the body of a ToolResult as JSON (for structured assertions).
static Json parse_body(const ToolResult& r) {
    return Json::parse(r.body);
}

// =============================================================================
// TEST SUITE: Task serialisation
// =============================================================================
TEST_SUITE("Task — serialisation") {

    TEST_CASE("to_json / from_json round-trip with all fields") {
        Task orig;
        orig.id          = "abc-123";
        orig.title       = "My task";
        orig.description = "Some details";
        orig.status      = "in_progress";
        orig.parent_id   = "parent-456";
        orig.tags        = {"alpha", "beta"};
        orig.created_at  = "2024-01-01T00:00:00Z";
        orig.updated_at  = "2024-01-02T00:00:00Z";

        const Json j = orig.to_json();
        Task copy;
        REQUIRE(Task::from_json(j, copy));

        CHECK(copy.id          == orig.id);
        CHECK(copy.title       == orig.title);
        CHECK(copy.description == orig.description);
        CHECK(copy.status      == orig.status);
        CHECK(copy.parent_id   == orig.parent_id);
        CHECK(copy.tags        == orig.tags);
        CHECK(copy.created_at  == orig.created_at);
        CHECK(copy.updated_at  == orig.updated_at);
    }

    TEST_CASE("to_json produces expected JSON keys") {
        Task t;
        t.id         = "x";
        t.title      = "T";
        t.status     = "pending";
        t.created_at = "now";
        t.updated_at = "now";
        const Json j = t.to_json();
        CHECK(j.contains("id"));
        CHECK(j.contains("title"));
        CHECK(j.contains("description"));
        CHECK(j.contains("status"));
        CHECK(j.contains("parent_id"));
        CHECK(j.contains("tags"));
        CHECK(j.contains("created_at"));
        CHECK(j.contains("updated_at"));
    }

    TEST_CASE("from_json returns false for non-object") {
        Task t;
        CHECK_FALSE(Task::from_json(Json::array(), t));
        CHECK_FALSE(Task::from_json(Json("string"), t));
        CHECK_FALSE(Task::from_json(Json(42), t));
    }

    TEST_CASE("from_json returns false when required id missing") {
        Task t;
        Json j{{"title","T"},{"status","pending"},{"created_at","now"},{"updated_at","now"}};
        CHECK_FALSE(Task::from_json(j, t));
    }

    TEST_CASE("from_json returns false when required title missing") {
        Task t;
        Json j{{"id","x"},{"status","pending"},{"created_at","now"},{"updated_at","now"}};
        CHECK_FALSE(Task::from_json(j, t));
    }

    TEST_CASE("from_json returns false when created_at missing") {
        Task t;
        Json j{{"id","x"},{"title","T"},{"status","pending"},{"updated_at","now"}};
        CHECK_FALSE(Task::from_json(j, t));
    }

    TEST_CASE("from_json handles optional fields absent gracefully") {
        Task t;
        Json j{
            {"id","x"},{"title","T"},{"status","pending"},
            {"created_at","ts"},{"updated_at","ts"}
        };
        REQUIRE(Task::from_json(j, t));
        CHECK(t.description.empty());
        CHECK(t.parent_id.empty());
        CHECK(t.tags.empty());
    }

    TEST_CASE("tags array round-trips with multiple entries") {
        Task t;
        t.id = "x"; t.title = "T"; t.status = "pending";
        t.created_at = "ts"; t.updated_at = "ts";
        t.tags = {"one", "two", "three"};

        Task copy;
        REQUIRE(Task::from_json(t.to_json(), copy));
        CHECK(copy.tags.size() == 3);
        CHECK(copy.tags[0] == "one");
        CHECK(copy.tags[1] == "two");
        CHECK(copy.tags[2] == "three");
    }
}

// =============================================================================
// TEST SUITE: TaskStore — CRUD
// =============================================================================
TEST_SUITE("TaskStore — CRUD") {

    TEST_CASE("create_task returns task with non-empty id and correct fields") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());

        TaskCreateParams p;
        p.title       = "First task";
        p.description = "Details here";
        p.status      = "pending";
        p.tags        = {"tag1"};

        const auto result = store->create_task(p);
        REQUIRE(result.has_value());
        CHECK(!result->id.empty());
        CHECK(result->title       == "First task");
        CHECK(result->description == "Details here");
        CHECK(result->status      == "pending");
        CHECK(result->tags.size() == 1);
        CHECK(result->tags[0]     == "tag1");
        CHECK(!result->created_at.empty());
        CHECK(!result->updated_at.empty());
        CHECK(result->created_at  == result->updated_at);
    }

    TEST_CASE("create_task generates unique ids for each call") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());

        TaskCreateParams p;
        p.title = "Task A";
        const auto a = store->create_task(p);

        p.title = "Task B";
        const auto b = store->create_task(p);

        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        CHECK(a->id != b->id);
    }

    TEST_CASE("create_task with empty title returns nullopt") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskCreateParams p;
        p.title = "";
        CHECK_FALSE(store->create_task(p).has_value());
    }

    TEST_CASE("create_task with invalid status returns nullopt") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskCreateParams p;
        p.title  = "T";
        p.status = "unknown_status";
        CHECK_FALSE(store->create_task(p).has_value());
    }

    TEST_CASE("list_tasks returns empty vector when no tasks exist") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        CHECK(store->list_tasks().empty());
    }

    TEST_CASE("list_tasks returns all tasks when no filter applied") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());

        TaskCreateParams p;
        p.title = "T1"; p.status = "pending";
        (void)store->create_task(p);
        p.title = "T2"; p.status = "in_progress";
        (void)store->create_task(p);
        p.title = "T3"; p.status = "completed";
        (void)store->create_task(p);

        CHECK(store->list_tasks().size() == 3);
    }

    TEST_CASE("list_tasks filters by status") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());

        TaskCreateParams p;
        p.title = "T1"; p.status = "pending";
        (void)store->create_task(p);
        p.title = "T2"; p.status = "pending";
        (void)store->create_task(p);
        p.title = "T3"; p.status = "completed";
        (void)store->create_task(p);

        const auto pending = store->list_tasks("pending");
        CHECK(pending.size() == 2);
        for (const auto& t : pending) {
            CHECK(t.status == "pending");
        }

        const auto completed = store->list_tasks("completed");
        CHECK(completed.size() == 1);
    }

    TEST_CASE("list_tasks filters by tag") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());

        TaskCreateParams p;
        p.title = "T1"; p.tags = {"alpha", "shared"};
        (void)store->create_task(p);
        p.title = "T2"; p.tags = {"beta", "shared"};
        (void)store->create_task(p);
        p.title = "T3"; p.tags = {"gamma"};
        (void)store->create_task(p);

        const auto shared = store->list_tasks({}, "shared");
        CHECK(shared.size() == 2);

        const auto gamma = store->list_tasks({}, "gamma");
        CHECK(gamma.size() == 1);
        CHECK(gamma[0].title == "T3");
    }

    TEST_CASE("list_tasks applies both status and tag filters simultaneously") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());

        TaskCreateParams p;
        p.title = "T1"; p.status = "pending";   p.tags = {"work"};
        (void)store->create_task(p);
        p.title = "T2"; p.status = "completed"; p.tags = {"work"};
        (void)store->create_task(p);
        p.title = "T3"; p.status = "pending";   p.tags = {"home"};
        (void)store->create_task(p);

        // status=pending AND tag=work → only T1
        const auto result = store->list_tasks("pending", "work");
        CHECK(result.size() == 1);
        CHECK(result[0].title == "T1");
    }

    TEST_CASE("get_task returns nullopt for non-existent id") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        CHECK_FALSE(store->get_task("no-such-id").has_value());
    }

    TEST_CASE("get_task returns the task for a valid id") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());

        TaskCreateParams p;
        p.title = "Findable";
        const auto created = store->create_task(p);
        REQUIRE(created.has_value());

        const auto found = store->get_task(created->id);
        REQUIRE(found.has_value());
        CHECK(found->id    == created->id);
        CHECK(found->title == "Findable");
    }

    TEST_CASE("update_task returns false for non-existent id") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskUpdateParams params;
        params.title = "New title";
        CHECK_FALSE(store->update_task("no-such-id", params));
    }

    TEST_CASE("update_task mutates title and sets updated_at") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());

        TaskCreateParams cp;
        cp.title = "Original";
        const auto created = store->create_task(cp);
        REQUIRE(created.has_value());

        TaskUpdateParams up;
        up.title = "Updated";
        REQUIRE(store->update_task(created->id, up));

        const auto updated = store->get_task(created->id);
        REQUIRE(updated.has_value());
        CHECK(updated->title      == "Updated");
        CHECK(updated->updated_at >= created->updated_at);
    }

    TEST_CASE("update_task partial: only supplied fields change") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());

        TaskCreateParams cp;
        cp.title       = "T";
        cp.description = "Original desc";
        cp.status      = "pending";
        cp.tags        = {"a"};
        const auto created = store->create_task(cp);
        REQUIRE(created.has_value());

        // Update only status — title and description should remain.
        TaskUpdateParams up;
        up.status = "in_progress";
        REQUIRE(store->update_task(created->id, up));

        const auto updated = store->get_task(created->id);
        REQUIRE(updated.has_value());
        CHECK(updated->status      == "in_progress");
        CHECK(updated->title       == "T");
        CHECK(updated->description == "Original desc");
        CHECK(updated->tags        == std::vector<std::string>{"a"});
    }

    TEST_CASE("update_task tags replaces the tags array entirely") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());

        TaskCreateParams cp;
        cp.title = "T";
        cp.tags  = {"old"};
        const auto created = store->create_task(cp);
        REQUIRE(created.has_value());

        TaskUpdateParams up;
        up.tags = std::vector<std::string>{"new1", "new2"};
        REQUIRE(store->update_task(created->id, up));

        const auto updated = store->get_task(created->id);
        REQUIRE(updated.has_value());
        CHECK(updated->tags.size() == 2);
        CHECK(updated->tags[0]     == "new1");
        CHECK(updated->tags[1]     == "new2");
    }
}

// =============================================================================
// TEST SUITE: TaskStore — persistence
// =============================================================================
TEST_SUITE("TaskStore — persistence across instances") {

    TEST_CASE("tasks created by one instance are readable by a second instance") {
        ScopedHome guard;
        const fs::path tasks_file = guard.tasks_path();

        // Create tasks with instance A.
        {
            auto storeA = std::make_shared<TaskStore>(tasks_file);
            TaskCreateParams p;
            p.title = "Persisted Task";
            const auto created = storeA->create_task(p);
            REQUIRE(created.has_value());
        }

        // Read tasks with instance B (new object, same file).
        {
            auto storeB = std::make_shared<TaskStore>(tasks_file);
            const auto tasks = storeB->list_tasks();
            REQUIRE(tasks.size() == 1);
            CHECK(tasks[0].title == "Persisted Task");
        }
    }

    TEST_CASE("update performed by instance A is visible to instance B") {
        ScopedHome guard;
        const fs::path tasks_file = guard.tasks_path();

        std::string task_id;
        {
            auto storeA = std::make_shared<TaskStore>(tasks_file);
            TaskCreateParams p;
            p.title = "Draft";
            const auto created = storeA->create_task(p);
            REQUIRE(created.has_value());
            task_id = created->id;

            TaskUpdateParams up;
            up.status = "completed";
            REQUIRE(storeA->update_task(task_id, up));
        }

        {
            auto storeB = std::make_shared<TaskStore>(tasks_file);
            const auto found = storeB->get_task(task_id);
            REQUIRE(found.has_value());
            CHECK(found->status == "completed");
        }
    }

    TEST_CASE("load() on non-existent file returns empty vector") {
        ScopedHome guard;
        const fs::path no_file = guard.tasks_path();
        // File does not exist yet.
        auto store = std::make_shared<TaskStore>(no_file);
        CHECK(store->load().empty());
    }

    TEST_CASE("tasks file is valid JSON after multiple creates") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());

        for (int i = 0; i < 5; ++i) {
            TaskCreateParams p;
            p.title = "Task " + std::to_string(i);
            REQUIRE(store->create_task(p).has_value());
        }

        // The file must be valid JSON and contain an array of 5 tasks.
        std::ifstream in(guard.tasks_path(), std::ios::binary);
        REQUIRE(in);
        std::ostringstream buf;
        buf << in.rdbuf();
        const Json arr = Json::parse(buf.str());
        REQUIRE(arr.is_array());
        CHECK(arr.size() == 5);
    }
}

// =============================================================================
// TEST SUITE: TaskCreateTool
// =============================================================================
TEST_SUITE("TaskCreateTool — identity and schema") {

    TEST_CASE("name() returns \"TaskCreate\"") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskCreateTool t(store);
        CHECK(t.name() == std::string_view("TaskCreate"));
    }

    TEST_CASE("description() is non-empty") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskCreateTool t(store);
        CHECK(!t.description().empty());
    }

    TEST_CASE("is_read_only() returns false") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskCreateTool t(store);
        CHECK_FALSE(t.is_read_only());
    }

    TEST_CASE("requires_confirmation() returns false") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskCreateTool t(store);
        CHECK_FALSE(t.requires_confirmation());
    }

    TEST_CASE("schema_json() has required OpenAI fields") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskCreateTool t(store);
        Json s = t.schema_json();
        REQUIRE(s.is_object());
        CHECK(s["name"].get<std::string>() == "TaskCreate");
        CHECK(s.contains("description"));
        CHECK(s.contains("parameters"));
        CHECK(s["parameters"]["properties"].contains("title"));
    }
}

TEST_SUITE("TaskCreateTool — argument validation") {

    TEST_CASE("missing title returns error") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskCreateTool t(store);
        auto ctx = make_ctx();
        ToolResult r = t.run(Json::object(), ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("title") != std::string::npos);
    }

    TEST_CASE("empty title returns error") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskCreateTool t(store);
        auto ctx = make_ctx();
        ToolResult r = t.run(Json{{"title", ""}}, ctx);
        CHECK(r.is_error);
    }

    TEST_CASE("invalid status returns error") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskCreateTool t(store);
        auto ctx = make_ctx();
        ToolResult r = t.run(Json{{"title", "T"}, {"status", "unknown"}}, ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("unknown") != std::string::npos);
    }

    TEST_CASE("plan mode returns error") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskCreateTool t(store);
        auto ctx = make_ctx(PermissionMode::Plan);
        ToolResult r = t.run(Json{{"title", "T"}}, ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("Plan mode") != std::string::npos);
    }
}

TEST_SUITE("TaskCreateTool — success") {

    TEST_CASE("creates task with title only") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskCreateTool t(store);
        auto ctx = make_ctx();

        ToolResult r = t.run(Json{{"title", "My Task"}}, ctx);
        REQUIRE_FALSE(r.is_error);

        const Json body = parse_body(r);
        CHECK(!body["id"].get<std::string>().empty());
        CHECK(body["title"].get<std::string>() == "My Task");
        CHECK(body["status"].get<std::string>() == "pending");
    }

    TEST_CASE("creates task with all optional fields") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskCreateTool t(store);
        auto ctx = make_ctx();

        ToolResult r = t.run(Json{
            {"title",       "Full Task"},
            {"description", "Detailed desc"},
            {"status",      "in_progress"},
            {"parent_id",   "parent-abc"},
            {"tags",        Json::array({"x", "y"})}
        }, ctx);

        REQUIRE_FALSE(r.is_error);
        const Json body = parse_body(r);
        CHECK(body["title"].get<std::string>()       == "Full Task");
        CHECK(body["description"].get<std::string>() == "Detailed desc");
        CHECK(body["status"].get<std::string>()      == "in_progress");
        CHECK(body["parent_id"].get<std::string>()   == "parent-abc");
        REQUIRE(body["tags"].is_array());
        CHECK(body["tags"].size() == 2);
    }

    TEST_CASE("structured payload contains the task object") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskCreateTool t(store);
        auto ctx = make_ctx();

        ToolResult r = t.run(Json{{"title", "Payload Test"}}, ctx);
        REQUIRE_FALSE(r.is_error);
        REQUIRE(r.structured_payload.has_value());
        CHECK(r.structured_payload->at("title").get<std::string>() == "Payload Test");
    }

    TEST_CASE("cancellation returns error immediately") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskCreateTool t(store);
        auto ctx = make_cancelled_ctx();
        ToolResult r = t.run(Json{{"title", "T"}}, ctx);
        CHECK(r.is_error);
        CHECK(r.body == "cancelled");
    }
}

// =============================================================================
// TEST SUITE: TaskListTool
// =============================================================================
TEST_SUITE("TaskListTool — identity and schema") {

    TEST_CASE("name() returns \"TaskList\"") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskListTool t(store);
        CHECK(t.name() == std::string_view("TaskList"));
    }

    TEST_CASE("is_read_only() returns true") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskListTool t(store);
        CHECK(t.is_read_only());
    }

    TEST_CASE("requires_confirmation() returns false") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskListTool t(store);
        CHECK_FALSE(t.requires_confirmation());
    }

    TEST_CASE("schema_json() name matches name()") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskListTool t(store);
        CHECK(t.schema_json()["name"].get<std::string>() == "TaskList");
    }
}

TEST_SUITE("TaskListTool — run") {

    TEST_CASE("returns empty JSON array when no tasks") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskListTool t(store);
        auto ctx = make_ctx();
        ToolResult r = t.run(Json::object(), ctx);
        REQUIRE_FALSE(r.is_error);
        const Json body = parse_body(r);
        REQUIRE(body.is_array());
        CHECK(body.empty());
    }

    TEST_CASE("returns all tasks with no filter") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskListTool list_tool(store);
        TaskCreateTool create_tool(store);
        auto ctx = make_ctx();

        (void)create_tool.run(Json{{"title", "A"}}, ctx);
        (void)create_tool.run(Json{{"title", "B"}}, ctx);
        (void)create_tool.run(Json{{"title", "C"}}, ctx);

        ToolResult r = list_tool.run(Json::object(), ctx);
        REQUIRE_FALSE(r.is_error);
        CHECK(parse_body(r).size() == 3);
    }

    TEST_CASE("filters by status") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskListTool list_tool(store);
        TaskCreateTool create_tool(store);
        auto ctx = make_ctx();

        (void)create_tool.run(Json{{"title","A"},{"status","pending"}}, ctx);
        (void)create_tool.run(Json{{"title","B"},{"status","completed"}}, ctx);
        (void)create_tool.run(Json{{"title","C"},{"status","pending"}}, ctx);

        ToolResult r = list_tool.run(Json{{"status","pending"}}, ctx);
        REQUIRE_FALSE(r.is_error);
        CHECK(parse_body(r).size() == 2);
    }

    TEST_CASE("filters by tag") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskListTool list_tool(store);
        TaskCreateTool create_tool(store);
        auto ctx = make_ctx();

        (void)create_tool.run(Json{{"title","A"},{"tags",Json::array({"foo"})}}, ctx);
        (void)create_tool.run(Json{{"title","B"},{"tags",Json::array({"bar"})}}, ctx);

        ToolResult r = list_tool.run(Json{{"tag","foo"}}, ctx);
        REQUIRE_FALSE(r.is_error);
        const Json body = parse_body(r);
        CHECK(body.size() == 1);
        CHECK(body[0]["title"].get<std::string>() == "A");
    }

    TEST_CASE("allowed in Plan mode (is_read_only)") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskListTool t(store);
        auto ctx = make_ctx(PermissionMode::Plan);
        ToolResult r = t.run(Json::object(), ctx);
        CHECK_FALSE(r.is_error);
    }

    TEST_CASE("cancellation returns error") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskListTool t(store);
        auto ctx = make_cancelled_ctx();
        ToolResult r = t.run(Json::object(), ctx);
        CHECK(r.is_error);
        CHECK(r.body == "cancelled");
    }
}

// =============================================================================
// TEST SUITE: TaskGetTool
// =============================================================================
TEST_SUITE("TaskGetTool — identity and schema") {

    TEST_CASE("name() returns \"TaskGet\"") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskGetTool t(store);
        CHECK(t.name() == std::string_view("TaskGet"));
    }

    TEST_CASE("is_read_only() returns true") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskGetTool t(store);
        CHECK(t.is_read_only());
    }

    TEST_CASE("requires_confirmation() returns false") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskGetTool t(store);
        CHECK_FALSE(t.requires_confirmation());
    }
}

TEST_SUITE("TaskGetTool — run") {

    TEST_CASE("missing id returns error") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskGetTool t(store);
        auto ctx = make_ctx();
        ToolResult r = t.run(Json::object(), ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("id") != std::string::npos);
    }

    TEST_CASE("empty id string returns error") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskGetTool t(store);
        auto ctx = make_ctx();
        ToolResult r = t.run(Json{{"id", ""}}, ctx);
        CHECK(r.is_error);
    }

    TEST_CASE("unknown id returns error") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskGetTool t(store);
        auto ctx = make_ctx();
        ToolResult r = t.run(Json{{"id", "no-such-task"}}, ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("no-such-task") != std::string::npos);
    }

    TEST_CASE("valid id returns task JSON") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskGetTool get_tool(store);
        TaskCreateTool create_tool(store);
        auto ctx = make_ctx();

        ToolResult cr = create_tool.run(Json{{"title", "Findable"}}, ctx);
        REQUIRE_FALSE(cr.is_error);
        const std::string id = parse_body(cr)["id"].get<std::string>();

        ToolResult gr = get_tool.run(Json{{"id", id}}, ctx);
        REQUIRE_FALSE(gr.is_error);
        const Json body = parse_body(gr);
        CHECK(body["id"].get<std::string>()    == id);
        CHECK(body["title"].get<std::string>() == "Findable");
    }

    TEST_CASE("structured payload contains the task") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskGetTool get_tool(store);
        TaskCreateTool create_tool(store);
        auto ctx = make_ctx();

        ToolResult cr = create_tool.run(Json{{"title", "PayloadTask"}}, ctx);
        REQUIRE_FALSE(cr.is_error);
        const std::string id = parse_body(cr)["id"].get<std::string>();

        ToolResult gr = get_tool.run(Json{{"id", id}}, ctx);
        REQUIRE_FALSE(gr.is_error);
        REQUIRE(gr.structured_payload.has_value());
        CHECK(gr.structured_payload->at("title").get<std::string>() == "PayloadTask");
    }

    TEST_CASE("allowed in Plan mode (is_read_only)") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskGetTool get_tool(store);
        TaskCreateTool create_tool(store);

        auto default_ctx = make_ctx();
        ToolResult cr = create_tool.run(Json{{"title","T"}}, default_ctx);
        REQUIRE_FALSE(cr.is_error);
        const std::string id = parse_body(cr)["id"].get<std::string>();

        auto plan_ctx = make_ctx(PermissionMode::Plan);
        ToolResult gr = get_tool.run(Json{{"id", id}}, plan_ctx);
        CHECK_FALSE(gr.is_error);
    }

    TEST_CASE("cancellation returns error") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskGetTool t(store);
        auto ctx = make_cancelled_ctx();
        ToolResult r = t.run(Json{{"id", "any-id"}}, ctx);
        CHECK(r.is_error);
        CHECK(r.body == "cancelled");
    }
}

// =============================================================================
// TEST SUITE: TaskUpdateTool
// =============================================================================
TEST_SUITE("TaskUpdateTool — identity and schema") {

    TEST_CASE("name() returns \"TaskUpdate\"") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskUpdateTool t(store);
        CHECK(t.name() == std::string_view("TaskUpdate"));
    }

    TEST_CASE("is_read_only() returns false") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskUpdateTool t(store);
        CHECK_FALSE(t.is_read_only());
    }

    TEST_CASE("requires_confirmation() returns false") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskUpdateTool t(store);
        CHECK_FALSE(t.requires_confirmation());
    }

    TEST_CASE("schema_json() name matches name()") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskUpdateTool t(store);
        CHECK(t.schema_json()["name"].get<std::string>() == "TaskUpdate");
    }
}

TEST_SUITE("TaskUpdateTool — argument validation") {

    TEST_CASE("missing id returns error") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskUpdateTool t(store);
        auto ctx = make_ctx();
        ToolResult r = t.run(Json::object(), ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("id") != std::string::npos);
    }

    TEST_CASE("empty id returns error") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskUpdateTool t(store);
        auto ctx = make_ctx();
        ToolResult r = t.run(Json{{"id",""}}, ctx);
        CHECK(r.is_error);
    }

    TEST_CASE("empty title returns error when supplied") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskUpdateTool t(store);
        auto ctx = make_ctx();
        ToolResult r = t.run(Json{{"id","x"},{"title",""}}, ctx);
        CHECK(r.is_error);
    }

    TEST_CASE("invalid status value returns error") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskUpdateTool t(store);
        auto ctx = make_ctx();
        ToolResult r = t.run(Json{{"id","x"},{"status","bad_value"}}, ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("bad_value") != std::string::npos);
    }

    TEST_CASE("plan mode returns error") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskUpdateTool t(store);
        auto ctx = make_ctx(PermissionMode::Plan);
        ToolResult r = t.run(Json{{"id","x"},{"status","completed"}}, ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("Plan mode") != std::string::npos);
    }

    TEST_CASE("unknown id returns error") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskUpdateTool t(store);
        auto ctx = make_ctx();
        ToolResult r = t.run(Json{{"id","no-such"},{"title","New"}}, ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("no-such") != std::string::npos);
    }
}

TEST_SUITE("TaskUpdateTool — partial update success") {

    TEST_CASE("update status returns updated task JSON") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskUpdateTool update_tool(store);
        TaskCreateTool create_tool(store);
        auto ctx = make_ctx();

        ToolResult cr = create_tool.run(Json{{"title","T"},{"status","pending"}}, ctx);
        REQUIRE_FALSE(cr.is_error);
        const std::string id = parse_body(cr)["id"].get<std::string>();

        ToolResult ur = update_tool.run(
            Json{{"id",id},{"status","completed"}}, ctx);
        REQUIRE_FALSE(ur.is_error);

        const Json body = parse_body(ur);
        CHECK(body["status"].get<std::string>() == "completed");
        CHECK(body["title"].get<std::string>()  == "T");
    }

    TEST_CASE("update title keeps other fields unchanged") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskUpdateTool update_tool(store);
        TaskCreateTool create_tool(store);
        auto ctx = make_ctx();

        ToolResult cr = create_tool.run(Json{
            {"title","Original"},
            {"description","Desc"},
            {"status","pending"},
            {"tags",Json::array({"t1"})}
        }, ctx);
        REQUIRE_FALSE(cr.is_error);
        const std::string id = parse_body(cr)["id"].get<std::string>();

        ToolResult ur = update_tool.run(Json{{"id",id},{"title","Updated"}}, ctx);
        REQUIRE_FALSE(ur.is_error);
        const Json body = parse_body(ur);
        CHECK(body["title"].get<std::string>()       == "Updated");
        CHECK(body["description"].get<std::string>() == "Desc");
        CHECK(body["status"].get<std::string>()      == "pending");
        CHECK(body["tags"].size() == 1);
    }

    TEST_CASE("update tags replaces array") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskUpdateTool update_tool(store);
        TaskCreateTool create_tool(store);
        auto ctx = make_ctx();

        ToolResult cr = create_tool.run(Json{
            {"title","T"},{"tags",Json::array({"old1","old2"})}
        }, ctx);
        REQUIRE_FALSE(cr.is_error);
        const std::string id = parse_body(cr)["id"].get<std::string>();

        ToolResult ur = update_tool.run(
            Json{{"id",id},{"tags",Json::array({"new1","new2","new3"})}}, ctx);
        REQUIRE_FALSE(ur.is_error);
        const Json body = parse_body(ur);
        CHECK(body["tags"].size() == 3);
    }

    TEST_CASE("structured payload contains the updated task") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskUpdateTool update_tool(store);
        TaskCreateTool create_tool(store);
        auto ctx = make_ctx();

        ToolResult cr = create_tool.run(Json{{"title","T"}}, ctx);
        REQUIRE_FALSE(cr.is_error);
        const std::string id = parse_body(cr)["id"].get<std::string>();

        ToolResult ur = update_tool.run(Json{{"id",id},{"title","Updated"}}, ctx);
        REQUIRE_FALSE(ur.is_error);
        REQUIRE(ur.structured_payload.has_value());
        CHECK(ur.structured_payload->at("title").get<std::string>() == "Updated");
    }

    TEST_CASE("cancellation returns error") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskUpdateTool t(store);
        auto ctx = make_cancelled_ctx();
        ToolResult r = t.run(Json{{"id","x"},{"status","completed"}}, ctx);
        CHECK(r.is_error);
        CHECK(r.body == "cancelled");
    }
}

// =============================================================================
// TEST SUITE: Full CRUD round-trip
// =============================================================================
TEST_SUITE("CRUD round-trip") {

    TEST_CASE("create → list → get → update → list lifecycle") {
        ScopedHome guard;
        auto store = std::make_shared<TaskStore>(guard.tasks_path());
        TaskCreateTool create_tool(store);
        TaskListTool   list_tool(store);
        TaskGetTool    get_tool(store);
        TaskUpdateTool update_tool(store);
        auto ctx = make_ctx();

        // Create.
        ToolResult cr = create_tool.run(
            Json{{"title","Round-trip"},{"status","pending"}}, ctx);
        REQUIRE_FALSE(cr.is_error);
        const std::string id = parse_body(cr)["id"].get<std::string>();

        // List — must contain the task.
        ToolResult lr = list_tool.run(Json::object(), ctx);
        REQUIRE_FALSE(lr.is_error);
        {
            const Json arr = parse_body(lr);
            bool found = false;
            for (const auto& item : arr) {
                if (item["id"].get<std::string>() == id) { found = true; break; }
            }
            CHECK(found);
        }

        // Get — must return the task.
        ToolResult gr = get_tool.run(Json{{"id",id}}, ctx);
        REQUIRE_FALSE(gr.is_error);
        CHECK(parse_body(gr)["title"].get<std::string>() == "Round-trip");

        // Update — change status to completed.
        ToolResult ur = update_tool.run(
            Json{{"id",id},{"status","completed"}}, ctx);
        REQUIRE_FALSE(ur.is_error);
        CHECK(parse_body(ur)["status"].get<std::string>() == "completed");

        // List filtered by completed — must contain exactly this task.
        ToolResult lr2 = list_tool.run(Json{{"status","completed"}}, ctx);
        REQUIRE_FALSE(lr2.is_error);
        const Json arr2 = parse_body(lr2);
        CHECK(arr2.size() == 1);
        CHECK(arr2[0]["id"].get<std::string>() == id);
    }
}
