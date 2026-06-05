// tests/integration/test_session_resume.cpp
// =============================================================================
// doctest integration tests for batbox::session::SessionStore (CPP 9.4).
//
// Covers:
//   1. new_session() creates a readable .json file + index entry
//   2. append_message() 100-message round-trip: all 100 messages retrieved
//   3. list_recent() returns correct count and ordering
//   4. load() round-trips session_id
//   5. resume_for_cwd() finds session by working directory
//   6. Thread-safety: two threads appending to different sessions concurrently
//   7. current_session_id() tracks last created session
//   8. Missing/absent index triggers background rebuild without crashing
//
// Build (standalone, from repo root):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/integration/test_session_resume.cpp \
//       src/session/SessionStore.cpp \
//       src/session/SessionFile.cpp \
//       src/session/SessionIndex.cpp \
//       src/session/SessionRecovery.cpp \
//       src/core/Uuid.cpp src/core/Logging.cpp src/core/Paths.cpp \
//       src/core/CancelToken.cpp src/core/Json.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libz.a \
//       build/vcpkg_installed/arm64-osx/lib/libspdlog.a \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       build/vcpkg_installed/arm64-osx/lib/libfmt.a \
//       -o /tmp/test_session_resume && /tmp/test_session_resume
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/session/SessionStore.hpp>
#include <batbox/core/Uuid.hpp>

#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace batbox;
using namespace batbox::session;

// =============================================================================
// Test helpers
// =============================================================================

/// RAII temporary directory.
struct TmpDir {
    fs::path path;

    TmpDir() {
        auto base = fs::temp_directory_path() / ("batbox_store_test_" + Uuid::v4().to_string());
        fs::create_directories(base);
        path = base;
    }

    ~TmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    fs::path operator/(const std::string& name) const { return path / name; }
};

/// Build a user message JSON object.
static Json user_msg(const std::string& text) {
    return Json{{"role", "user"}, {"content", text}};
}

/// Build an assistant message JSON object.
static Json assistant_msg(const std::string& text) {
    return Json{{"role", "assistant"}, {"content", text}};
}

// =============================================================================
// Suite 1: basic lifecycle
// =============================================================================
TEST_SUITE("SessionStore — basic lifecycle") {

    TEST_CASE("new_session creates a readable session file and returns a UUID") {
        TmpDir tmp;
        SessionStore store(tmp.path);

        auto res = store.new_session("gpt-4o", "/tmp/project");
        REQUIRE(res.has_value());
        const std::string sid = res.value();
        CHECK_FALSE(sid.empty());

        // Session file must exist.
        auto json_path = tmp.path / (sid + ".json");
        CHECK(fs::exists(json_path));

        // Must be loadable.
        auto loaded = store.load(sid);
        REQUIRE(loaded.has_value());
        CHECK(loaded->model_at_start == "gpt-4o");
        CHECK(loaded->messages.empty());
    }

    TEST_CASE("current_session_id returns nullopt before any new_session call") {
        TmpDir tmp;
        SessionStore store(tmp.path);
        CHECK_FALSE(store.current_session_id().has_value());
    }

    TEST_CASE("current_session_id returns the most recently created session") {
        TmpDir tmp;
        SessionStore store(tmp.path);

        auto r1 = store.new_session("model-a", "/tmp/a");
        REQUIRE(r1.has_value());
        CHECK(store.current_session_id() == r1.value());

        auto r2 = store.new_session("model-b", "/tmp/b");
        REQUIRE(r2.has_value());
        CHECK(store.current_session_id() == r2.value());
    }
}

// =============================================================================
// Suite 2: 100-message round-trip
// =============================================================================
TEST_SUITE("SessionStore — 100-message round-trip") {

    TEST_CASE("append and reload 100 messages preserves all content") {
        TmpDir tmp;
        SessionStore store(tmp.path);

        auto id_res = store.new_session("claude-3-5-sonnet", "/tmp/roundtrip");
        REQUIRE(id_res.has_value());
        const std::string sid = id_res.value();

        // Append 100 messages alternating user/assistant.
        for (int i = 0; i < 100; ++i) {
            Json msg = (i % 2 == 0)
                ? user_msg("user message " + std::to_string(i))
                : assistant_msg("assistant response " + std::to_string(i));
            auto r = store.append_message(sid, msg);
            REQUIRE_MESSAGE(r.has_value(), "append_message failed at i=" << i << ": " << (r.has_value() ? "" : r.error()));
        }

        // Reload the session and verify all 100 messages.
        auto loaded = store.load(sid);
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->messages.size() == 100);

        for (int i = 0; i < 100; ++i) {
            const auto& msg = loaded->messages[static_cast<size_t>(i)];
            REQUIRE(msg.contains("content"));
            REQUIRE(msg["content"].is_string());
            const std::string expected = ((i % 2 == 0) ? "user message " : "assistant response ") +
                                          std::to_string(i);
            CHECK_MESSAGE(msg["content"].get<std::string>() == expected,
                          "message " << i << " content mismatch");
        }
    }
}

// =============================================================================
// Suite 3: list_recent
// =============================================================================
TEST_SUITE("SessionStore — list_recent") {

    TEST_CASE("list_recent returns at most n entries") {
        TmpDir tmp;
        SessionStore store(tmp.path);

        // Create 5 sessions.
        for (int i = 0; i < 5; ++i) {
            auto r = store.new_session("model-" + std::to_string(i),
                                       "/tmp/proj_" + std::to_string(i));
            REQUIRE(r.has_value());
        }

        auto all = store.list_recent(10);
        REQUIRE(all.has_value());
        CHECK(all->size() <= 10);
        CHECK(all->size() >= 5); // All 5 should be present.

        auto limited = store.list_recent(3);
        REQUIRE(limited.has_value());
        CHECK(limited->size() == 3);
    }

    TEST_CASE("list_recent returns empty vector when no sessions exist") {
        TmpDir tmp;
        SessionStore store(tmp.path);

        auto res = store.list_recent(20);
        REQUIRE(res.has_value());
        CHECK(res->empty());
    }

    TEST_CASE("list_recent returns sessions sorted by updated_at descending") {
        TmpDir tmp;
        SessionStore store(tmp.path);

        std::vector<std::string> ids;
        for (int i = 0; i < 3; ++i) {
            auto r = store.new_session("m", "/tmp/p" + std::to_string(i));
            REQUIRE(r.has_value());
            ids.push_back(r.value());
            // Append a message to each so updated_at differs.
            (void)store.append_message(ids.back(), user_msg("hello " + std::to_string(i)));
        }

        auto recents = store.list_recent(10);
        REQUIRE(recents.has_value());
        REQUIRE(recents->size() >= 3);

        // Verify descending order of updated_at.
        for (size_t i = 1; i < recents->size(); ++i) {
            CHECK((*recents)[i - 1].updated_at >= (*recents)[i].updated_at);
        }
    }
}

// =============================================================================
// Suite 4: load()
// =============================================================================
TEST_SUITE("SessionStore — load") {

    TEST_CASE("load returns the correct session by id") {
        TmpDir tmp;
        SessionStore store(tmp.path);

        auto r = store.new_session("gpt-4o", "/tmp/load_test");
        REQUIRE(r.has_value());
        const std::string sid = r.value();

        (void)store.append_message(sid, user_msg("hello load"));

        auto loaded = store.load(sid);
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->messages.size() == 1);
        CHECK(loaded->messages[0]["content"] == "hello load");
        CHECK(loaded->model_at_start == "gpt-4o");
    }

    TEST_CASE("load returns Err for a nonexistent session id") {
        TmpDir tmp;
        SessionStore store(tmp.path);

        auto res = store.load(Uuid::v4().to_string());
        CHECK_FALSE(res.has_value());
    }
}

// =============================================================================
// Suite 5: resume_for_cwd()
// =============================================================================
TEST_SUITE("SessionStore — resume_for_cwd") {

    TEST_CASE("resume_for_cwd finds a session matching the working directory") {
        TmpDir tmp;
        // Use a real directory so fs::canonical works.
        fs::path project_dir = tmp.path / "my_project";
        fs::create_directories(project_dir);

        SessionStore store(tmp.path / "sessions");

        auto r = store.new_session("claude-3-5-sonnet", project_dir);
        REQUIRE(r.has_value());
        (void)store.append_message(r.value(), user_msg("first message"));

        auto resumed = store.resume_for_cwd(project_dir);
        REQUIRE(resumed.has_value());
        CHECK(resumed->model_at_start == "claude-3-5-sonnet");
        REQUIRE(resumed->messages.size() == 1);
        CHECK(resumed->messages[0]["content"] == "first message");
    }

    TEST_CASE("resume_for_cwd returns nullopt when no matching session exists") {
        TmpDir tmp;
        SessionStore store(tmp.path);

        (void)store.new_session("model", "/tmp/some_project");

        auto resumed = store.resume_for_cwd("/tmp/totally_different_project");
        CHECK_FALSE(resumed.has_value());
    }

    TEST_CASE("resume_for_cwd returns most recent matching session") {
        TmpDir tmp;
        fs::path project = tmp.path / "shared_project";
        fs::create_directories(project);

        SessionStore store(tmp.path / "sessions");

        auto r1 = store.new_session("model-v1", project);
        REQUIRE(r1.has_value());
        (void)store.append_message(r1.value(), user_msg("older session"));

        auto r2 = store.new_session("model-v2", project);
        REQUIRE(r2.has_value());
        (void)store.append_message(r2.value(), user_msg("newer session"));

        auto resumed = store.resume_for_cwd(project);
        REQUIRE(resumed.has_value());
        // The most recent session (model-v2) should be returned.
        // (list_recent returns sorted by updated_at desc, so first match wins)
        CHECK_FALSE(resumed->messages.empty());
    }
}

// =============================================================================
// Suite 6: thread safety
// =============================================================================
TEST_SUITE("SessionStore — thread safety") {

    TEST_CASE("concurrent appends to different sessions do not interleave or corrupt") {
        TmpDir tmp;
        SessionStore store(tmp.path);

        // Create two separate sessions.
        auto r1 = store.new_session("model-a", "/tmp/ta");
        auto r2 = store.new_session("model-b", "/tmp/tb");
        REQUIRE(r1.has_value());
        REQUIRE(r2.has_value());

        const std::string sid1 = r1.value();
        const std::string sid2 = r2.value();
        const int N = 50;

        // Launch two threads, each appending N messages to its own session.
        std::thread t1([&] {
            for (int i = 0; i < N; ++i) {
                auto res = store.append_message(sid1, user_msg("thread1 msg " + std::to_string(i)));
                CHECK(res.has_value());
            }
        });

        std::thread t2([&] {
            for (int i = 0; i < N; ++i) {
                auto res = store.append_message(sid2, user_msg("thread2 msg " + std::to_string(i)));
                CHECK(res.has_value());
            }
        });

        t1.join();
        t2.join();

        // Both sessions should have exactly N messages each.
        auto loaded1 = store.load(sid1);
        auto loaded2 = store.load(sid2);
        REQUIRE(loaded1.has_value());
        REQUIRE(loaded2.has_value());
        CHECK(loaded1->messages.size() == static_cast<size_t>(N));
        CHECK(loaded2->messages.size() == static_cast<size_t>(N));
    }
}

// =============================================================================
// Suite 7: recovery on missing index
// =============================================================================
TEST_SUITE("SessionStore — recovery") {

    TEST_CASE("store constructs successfully when sessions directory does not exist") {
        TmpDir tmp;
        // Pass a subdirectory that does not exist yet.
        fs::path new_dir = tmp.path / "brand_new_sessions";
        REQUIRE_FALSE(fs::exists(new_dir));

        // Construction must not throw.
        SessionStore store(new_dir);
        CHECK(fs::exists(new_dir));

        // Must be functional immediately.
        auto r = store.new_session("model", "/tmp/p");
        REQUIRE(r.has_value());
    }

    TEST_CASE("store remains functional when index is deleted between operations") {
        TmpDir tmp;
        SessionStore store(tmp.path);

        auto r = store.new_session("model", "/tmp/p");
        REQUIRE(r.has_value());
        (void)store.append_message(r.value(), user_msg("hello"));

        // Delete the index.
        fs::path idx = tmp.path / "index.json";
        std::error_code ec;
        fs::remove(idx, ec);

        // New operations must still succeed (load uses path map or file fallback).
        auto loaded = store.load(r.value());
        REQUIRE(loaded.has_value());
        CHECK(loaded->messages.size() == 1);

        // Creating another session must succeed.
        auto r2 = store.new_session("model2", "/tmp/q");
        REQUIRE(r2.has_value());
    }
}

// =============================================================================
// Suite 8: DIS-1020 — subagent journaling (agent_id + endpoint reference + retention)
//
// These cover the durability surface this child turns ON: a subagent's session
// log records WHO ran (agent_id) and on WHICH endpoint (a reference blob, never
// the raw api_key), the index carries the agent_id lookup key for the resume
// child, and the retention cap bounds always-on growth.  The SubAgent glue
// (set_session_identity + start_session before the loop) funnels through exactly
// this SessionStore path; test_subagent itself is unbuildable at HEAD due to
// pre-existing integration-test link-list rot unrelated to this change.
// =============================================================================

/// Build a tool-result message JSON object (mirrors Conversation's tool turns).
static Json tool_msg(const std::string& text) {
    return Json{{"role", "tool"}, {"content", text}, {"is_error", false}};
}

/// Build the endpoint REFERENCE blob exactly as SubAgent::run() does for a
/// subagent that ran on the local distill endpoint — note: NO api_key field.
static Json distill_endpoint_ref() {
    return Json{
        {"base_url", "http://127.0.0.1:11434/v1"},
        {"model", "qwen2.5-coder:7b"},
        {"use_distill_endpoint", true},
        {"api_key_ref", "cfg.distill"},
    };
}

/// Count session files (*.json / *.json.gz) in a sessions dir, excluding index.json.
static size_t count_session_files(const fs::path& dir) {
    size_t n = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (!e.is_regular_file()) continue;
        const auto name = e.path().filename().string();
        if (name == "index.json") continue;
        if (name.size() >= 5 && name.substr(name.size() - 5) == ".json") ++n;
        else if (name.size() >= 8 && name.substr(name.size() - 8) == ".json.gz") ++n;
    }
    return n;
}

TEST_SUITE("SessionStore — DIS-1020 subagent journaling") {

    TEST_CASE("subagent session records agent_id and endpoint reference, no raw api_key") {
        TmpDir tmp;
        SessionStore store(tmp.path);

        const std::string agent_id = "agent-" + Uuid::v4().to_string();
        auto res = store.new_session("qwen2.5-coder:7b", "/tmp/work",
                                     agent_id, distill_endpoint_ref());
        REQUIRE(res.has_value());
        const std::string sid = res.value();

        // A real subagent turn: user prompt → assistant → tool result.
        REQUIRE(store.append_message(sid, user_msg("engulf this source")).has_value());
        REQUIRE(store.append_message(sid, assistant_msg("calling a tool")).has_value());
        REQUIRE(store.append_message(sid, tool_msg("tool output 42")).has_value());

        // The on-disk log round-trips everything needed to resume.
        auto loaded = store.load(sid);
        REQUIRE(loaded.has_value());
        const SessionFile& sf = loaded.value();

        CHECK(sf.agent_id == agent_id);
        CHECK(sf.messages.size() == 3);
        // Tool *results* are persisted (not just calls) — required for safe resume.
        CHECK(sf.messages.back().at("role").get<std::string>() == "tool");

        // Endpoint reference round-trips with the resume-critical fields.
        REQUIRE(sf.endpoint.is_object());
        CHECK(sf.endpoint.at("base_url").get<std::string>() == "http://127.0.0.1:11434/v1");
        CHECK(sf.endpoint.at("model").get<std::string>() == "qwen2.5-coder:7b");
        CHECK(sf.endpoint.at("use_distill_endpoint").get<bool>() == true);
        CHECK(sf.endpoint.at("api_key_ref").get<std::string>() == "cfg.distill");

        // AC3 (Paulina refinement b): the raw credential is NEVER journalled.
        CHECK_FALSE(sf.endpoint.contains("api_key"));
    }

    TEST_CASE("index record carries agent_id (lookup key for the resume child)") {
        TmpDir tmp;
        SessionStore store(tmp.path);

        const std::string agent_id = "agent-lookup-1";
        auto res = store.new_session("m", "/tmp/work", agent_id, distill_endpoint_ref());
        REQUIRE(res.has_value());
        (void)store.append_message(res.value(), user_msg("hi"));

        auto recents = store.list_recent(16);
        REQUIRE(recents.has_value());
        bool found = false;
        for (const auto& rec : recents.value()) {
            if (rec.id.to_string() == res.value()) {
                CHECK(rec.agent_id == agent_id);
                found = true;
            }
        }
        CHECK(found);
    }

    TEST_CASE("main session is unchanged: empty agent_id, absent endpoint") {
        TmpDir tmp;
        SessionStore store(tmp.path);

        // Two-argument new_session (the main chat path) must be byte-compatible.
        auto res = store.new_session("gpt-4o", "/tmp/main");
        REQUIRE(res.has_value());
        (void)store.append_message(res.value(), user_msg("hello"));

        auto loaded = store.load(res.value());
        REQUIRE(loaded.has_value());
        CHECK(loaded->agent_id.empty());
        CHECK_FALSE(loaded->endpoint.is_object());  // null → omitted on disk

        auto recents = store.list_recent(8);
        REQUIRE(recents.has_value());
        for (const auto& rec : recents.value()) {
            if (rec.id.to_string() == res.value()) CHECK(rec.agent_id.empty());
        }
    }

    TEST_CASE("prune bounds growth at the cap and never deletes the active session") {
        TmpDir tmp;
        SessionStore store(tmp.path);

        constexpr size_t kCap   = 4;
        constexpr size_t kTotal = 12;

        std::string last_sid;
        for (size_t i = 0; i < kTotal; ++i) {
            auto r = store.new_session("m", "/tmp/p" + std::to_string(i),
                                       "agent-" + std::to_string(i),
                                       distill_endpoint_ref());
            REQUIRE(r.has_value());
            last_sid = r.value();
            (void)store.append_message(last_sid, user_msg("msg"));
        }
        REQUIRE(count_session_files(tmp.path) == kTotal);

        auto pruned = store.prune(kCap);
        REQUIRE(pruned.has_value());
        CHECK(pruned.value() > 0);  // it actually deleted something

        // Growth is bounded: at most the cap, plus the force-kept active session
        // if it happened to sort beyond the cap (timestamps are second-resolution).
        const size_t survivors = count_session_files(tmp.path);
        CHECK(survivors <= kCap + 1);
        CHECK(survivors < kTotal);

        // The active session is always recoverable after a prune.
        auto active = store.load(last_sid);
        CHECK(active.has_value());
    }

    TEST_CASE("prune is a no-op when under the cap") {
        TmpDir tmp;
        SessionStore store(tmp.path);
        for (int i = 0; i < 3; ++i) {
            REQUIRE(store.new_session("m", "/tmp/p").has_value());
        }
        auto pruned = store.prune(50);
        REQUIRE(pruned.has_value());
        CHECK(pruned.value() == 0);
        CHECK(count_session_files(tmp.path) == 3);
    }
}
