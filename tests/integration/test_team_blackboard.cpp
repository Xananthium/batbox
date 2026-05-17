// =============================================================================
// tests/integration/test_team_blackboard.cpp
//
// Integration tests for batbox::agents::Team + batbox::agents::TeamRegistry.
//
// Build standalone (no CMake, from repo root):
//   c++ -std=c++20 -Iinclude \
//       -Ibuild/vcpkg_installed/arm64-osx/include \
//       tests/integration/test_team_blackboard.cpp \
//       src/agents/Team.cpp \
//       src/core/Json.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       -o /tmp/test_team_blackboard && /tmp/test_team_blackboard
//
// Coverage:
//   1.  Team create and delete via TeamRegistry
//   2.  Members list is mutable (add / remove, idempotent add)
//   3.  Blackboard basic set / get round-trip
//   4.  Blackboard get on missing key returns nullopt
//   5.  Blackboard cas_kv — success when expected matches
//   6.  Blackboard cas_kv — failure when expected mismatches
//   7.  Blackboard cas_kv — insert when key absent and expected is null
//   8.  Blackboard erase_kv
//   9.  Blackboard thread-safety: 4 agents writing/reading concurrently
//   10. Broadcast enqueue + drain
//   11. TeamRegistry singleton identity
//   12. TeamRegistry idempotent create_team
//   13. TeamRegistry team_names snapshot
//   14. TeamRegistry get_team returns nullptr for unknown name
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/agents/Team.hpp>

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

using batbox::agents::Team;
using batbox::agents::TeamRegistry;
using batbox::Json;

// Helper: wipe teams created during tests so each test case starts clean.
static void cleanup_team(std::string_view name) {
    TeamRegistry::instance().delete_team(name);
}

// =============================================================================
// 1. Team create and delete via TeamRegistry
// =============================================================================
TEST_CASE("TeamRegistry create and delete") {
    const std::string team_name = "tc_test_create";
    cleanup_team(team_name);

    Team* t = TeamRegistry::instance().create_team(team_name);
    REQUIRE(t != nullptr);
    CHECK(t->name == team_name);

    Team* t2 = TeamRegistry::instance().get_team(team_name);
    CHECK(t2 == t);

    TeamRegistry::instance().delete_team(team_name);
    CHECK(TeamRegistry::instance().get_team(team_name) == nullptr);
}

// =============================================================================
// 2. Members list is mutable
// =============================================================================
TEST_CASE("Team membership add / remove / idempotent") {
    const std::string team_name = "tc_test_members";
    cleanup_team(team_name);

    Team* t = TeamRegistry::instance().create_team(team_name);

    // Initially empty.
    CHECK(t->members().empty());

    // Add two members.
    t->add_member("agent-1");
    t->add_member("agent-2");
    {
        auto m = t->members();
        REQUIRE(m.size() == 2);
        CHECK(std::find(m.begin(), m.end(), "agent-1") != m.end());
        CHECK(std::find(m.begin(), m.end(), "agent-2") != m.end());
    }

    // Idempotent add — size must not grow.
    t->add_member("agent-1");
    CHECK(t->members().size() == 2);

    // Remove one member.
    t->remove_member("agent-1");
    {
        auto m = t->members();
        CHECK(m.size() == 1);
        CHECK(m[0] == "agent-2");
    }

    // Remove non-existent — no-op (no crash).
    t->remove_member("agent-99");
    CHECK(t->members().size() == 1);

    cleanup_team(team_name);
}

// =============================================================================
// 3. Blackboard basic set / get round-trip
// =============================================================================
TEST_CASE("Blackboard set_kv / get_kv round-trip") {
    const std::string team_name = "tc_test_kv_basic";
    cleanup_team(team_name);

    Team* t = TeamRegistry::instance().create_team(team_name);

    t->set_kv("counter", Json(0));
    auto v = t->get_kv("counter");
    REQUIRE(v.has_value());
    CHECK(v->get<int>() == 0);

    // Overwrite.
    t->set_kv("counter", Json(42));
    v = t->get_kv("counter");
    REQUIRE(v.has_value());
    CHECK(v->get<int>() == 42);

    // JSON object value.
    t->set_kv("meta", Json({{"author", "agent-1"}, {"rev", 3}}));
    auto meta = t->get_kv("meta");
    REQUIRE(meta.has_value());
    CHECK(meta->at("author").get<std::string>() == "agent-1");
    CHECK(meta->at("rev").get<int>() == 3);

    cleanup_team(team_name);
}

// =============================================================================
// 4. Blackboard get on missing key returns nullopt
// =============================================================================
TEST_CASE("Blackboard get_kv missing key") {
    const std::string team_name = "tc_test_kv_miss";
    cleanup_team(team_name);

    Team* t = TeamRegistry::instance().create_team(team_name);
    CHECK_FALSE(t->get_kv("no_such_key").has_value());

    cleanup_team(team_name);
}

// =============================================================================
// 5. Blackboard cas_kv — success when expected matches
// =============================================================================
TEST_CASE("Blackboard cas_kv success") {
    const std::string team_name = "tc_test_cas_success";
    cleanup_team(team_name);

    Team* t = TeamRegistry::instance().create_team(team_name);

    t->set_kv("phase", Json("init"));
    bool swapped = t->cas_kv("phase", Json("init"), Json("running"));
    CHECK(swapped);
    auto v = t->get_kv("phase");
    REQUIRE(v.has_value());
    CHECK(v->get<std::string>() == "running");

    cleanup_team(team_name);
}

// =============================================================================
// 6. Blackboard cas_kv — failure when expected mismatches
// =============================================================================
TEST_CASE("Blackboard cas_kv failure on mismatch") {
    const std::string team_name = "tc_test_cas_fail";
    cleanup_team(team_name);

    Team* t = TeamRegistry::instance().create_team(team_name);

    t->set_kv("phase", Json("running"));
    bool swapped = t->cas_kv("phase", Json("init"), Json("done"));
    CHECK_FALSE(swapped);
    // Value must be unchanged.
    auto v = t->get_kv("phase");
    REQUIRE(v.has_value());
    CHECK(v->get<std::string>() == "running");

    cleanup_team(team_name);
}

// =============================================================================
// 7. Blackboard cas_kv — insert when key absent and expected is null
// =============================================================================
TEST_CASE("Blackboard cas_kv insert on absent key with null expected") {
    const std::string team_name = "tc_test_cas_insert";
    cleanup_team(team_name);

    Team* t = TeamRegistry::instance().create_team(team_name);

    // Key does not exist; expected is JSON null — should insert.
    bool inserted = t->cas_kv("new_key", Json(nullptr), Json(99));
    CHECK(inserted);
    auto v = t->get_kv("new_key");
    REQUIRE(v.has_value());
    CHECK(v->get<int>() == 99);

    // Key exists now; same expected=null should fail (current != null).
    bool second = t->cas_kv("new_key", Json(nullptr), Json(100));
    CHECK_FALSE(second);
    CHECK(t->get_kv("new_key")->get<int>() == 99);

    cleanup_team(team_name);
}

// =============================================================================
// 8. Blackboard erase_kv
// =============================================================================
TEST_CASE("Blackboard erase_kv") {
    const std::string team_name = "tc_test_erase";
    cleanup_team(team_name);

    Team* t = TeamRegistry::instance().create_team(team_name);

    t->set_kv("x", Json(1));
    t->set_kv("y", Json(2));
    CHECK(t->blackboard_size() == 2);

    t->erase_kv("x");
    CHECK(t->blackboard_size() == 1);
    CHECK_FALSE(t->get_kv("x").has_value());
    CHECK(t->get_kv("y").has_value());

    // Erase non-existent — no-op.
    t->erase_kv("no_such");
    CHECK(t->blackboard_size() == 1);

    cleanup_team(team_name);
}

// =============================================================================
// 9. Blackboard thread-safety: 4 agents writing/reading concurrently
// =============================================================================
TEST_CASE("Blackboard thread-safe under contention with 4 agents") {
    const std::string team_name = "tc_test_concurrent";
    cleanup_team(team_name);

    Team* t = TeamRegistry::instance().create_team(team_name);

    // Seed initial per-agent counters.
    for (int i = 0; i < 4; ++i) {
        t->set_kv("agent_" + std::to_string(i), Json(0));
    }

    constexpr int kIterations = 500;
    std::atomic<int> total_reads{0};

    auto worker = [&](int agent_idx) {
        const std::string my_key = "agent_" + std::to_string(agent_idx);
        for (int iter = 0; iter < kIterations; ++iter) {
            // Read other agents' keys.
            for (int j = 0; j < 4; ++j) {
                if (j == agent_idx) continue;
                auto v = t->get_kv("agent_" + std::to_string(j));
                if (v.has_value()) {
                    total_reads.fetch_add(1, std::memory_order_relaxed);
                }
            }
            // CAS-increment own key: read → expected → desired = expected+1.
            // Retry until successful (bounded by the iteration ceiling).
            for (int attempt = 0; attempt < 20; ++attempt) {
                auto cur = t->get_kv(my_key);
                if (!cur.has_value()) break;
                int cur_val = cur->get<int>();
                if (t->cas_kv(my_key, Json(cur_val), Json(cur_val + 1))) {
                    break;
                }
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(4);
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back(worker, i);
    }
    for (auto& th : threads) {
        th.join();
    }

    // Each agent should have incremented its counter at most kIterations times.
    // Values must be non-negative and readable without crash.
    for (int i = 0; i < 4; ++i) {
        auto v = t->get_kv("agent_" + std::to_string(i));
        REQUIRE(v.has_value());
        CHECK(v->get<int>() >= 0);
        CHECK(v->get<int>() <= kIterations);
    }

    // Read count: each of 4 agents reads 3 others per iteration.
    CHECK(total_reads.load() > 0);

    cleanup_team(team_name);
}

// =============================================================================
// 10. Broadcast enqueue + drain
// =============================================================================
TEST_CASE("Team broadcast enqueue and drain") {
    const std::string team_name = "tc_test_broadcast";
    cleanup_team(team_name);

    Team* t = TeamRegistry::instance().create_team(team_name);
    t->add_member("a1");
    t->add_member("a2");

    t->broadcast("hello team");
    t->broadcast("second message");

    auto msgs = t->drain_pending_broadcasts();
    REQUIRE(msgs.size() == 2);
    CHECK(msgs[0] == "hello team");
    CHECK(msgs[1] == "second message");

    // After drain, queue is empty.
    auto empty = t->drain_pending_broadcasts();
    CHECK(empty.empty());

    cleanup_team(team_name);
}

// =============================================================================
// 11. TeamRegistry singleton identity
// =============================================================================
TEST_CASE("TeamRegistry singleton identity") {
    TeamRegistry& r1 = TeamRegistry::instance();
    TeamRegistry& r2 = TeamRegistry::instance();
    CHECK(&r1 == &r2);
}

// =============================================================================
// 12. TeamRegistry idempotent create_team
// =============================================================================
TEST_CASE("TeamRegistry idempotent create_team") {
    const std::string team_name = "tc_test_idem";
    cleanup_team(team_name);

    Team* first  = TeamRegistry::instance().create_team(team_name);
    Team* second = TeamRegistry::instance().create_team(team_name);
    CHECK(first == second); // same pointer — same object

    cleanup_team(team_name);
}

// =============================================================================
// 13. TeamRegistry team_names snapshot
// =============================================================================
TEST_CASE("TeamRegistry team_names snapshot") {
    const std::string a = "tc_snap_a";
    const std::string b = "tc_snap_b";
    cleanup_team(a);
    cleanup_team(b);

    TeamRegistry::instance().create_team(a);
    TeamRegistry::instance().create_team(b);

    auto names = TeamRegistry::instance().team_names();
    CHECK(std::find(names.begin(), names.end(), a) != names.end());
    CHECK(std::find(names.begin(), names.end(), b) != names.end());

    cleanup_team(a);
    cleanup_team(b);
}

// =============================================================================
// 14. TeamRegistry get_team returns nullptr for unknown name
// =============================================================================
TEST_CASE("TeamRegistry get_team nullptr on unknown") {
    Team* t = TeamRegistry::instance().get_team("tc_no_such_team_xyz_123");
    CHECK(t == nullptr);
}
