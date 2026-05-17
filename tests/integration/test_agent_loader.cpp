// tests/integration/test_agent_loader.cpp
// =============================================================================
// Integration tests for batbox::agents::AgentLoader.
//
// Tests exercise the full parse-and-scan pipeline against fixture files
// (tests/fixtures/agents/) plus synthetic temp directories created at runtime.
//
// Acceptance criteria (CPP 6.3):
//   AC1: Loads N agent files into map — names() returns correct sorted list.
//   AC2: Cycle in [[ref]] graph: detection emits error, has_cycle_error() true.
//   AC3: Missing agents dir: empty map, no exception.
//   AC4: Integration test with fixture agents (senior-dev, qa-dev, junior-dev).
//
// Build standalone (from repo root):
//   ROOT=/path/to/batbox
//   c++ -std=c++20 \
//       -I $ROOT/include \
//       -I $ROOT/build/vcpkg_installed/arm64-osx/include \
//       $ROOT/tests/integration/test_agent_loader.cpp \
//       $ROOT/src/plugins/FrontmatterParser.cpp \
//       $ROOT/src/agents/AgentLoader.cpp \
//       $ROOT/src/core/Logging.cpp \
//       $ROOT/src/core/Paths.cpp \
//       $ROOT/src/core/Json.cpp \
//       $ROOT/build/vcpkg_installed/arm64-osx/lib/libfmt.a \
//       $ROOT/build/vcpkg_installed/arm64-osx/lib/libspdlog.a \
//       $ROOT/build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       -o /tmp/test_agent_loader && /tmp/test_agent_loader
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/agents/AgentLoader.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace batbox;
using namespace batbox::agents;

namespace {

// ---------------------------------------------------------------------------
// project_root() — derive the repo root from this file's compile-time path.
// __FILE__ is absolute when compiled via CMake; walk up 3 levels:
//   tests/integration/test_agent_loader.cpp → tests/integration → tests → root
// ---------------------------------------------------------------------------
[[nodiscard]] std::filesystem::path project_root() {
    namespace fs = std::filesystem;
    return fs::path(__FILE__)
               .parent_path()  // tests/integration
               .parent_path()  // tests
               .parent_path(); // project root
}

// ---------------------------------------------------------------------------
// TempAgentDir — RAII helper: creates a temp dir, populates .md files,
// removes everything on destruction.
// ---------------------------------------------------------------------------
struct TempAgentDir {
    std::filesystem::path path;

    explicit TempAgentDir() {
        namespace fs = std::filesystem;
        // Use a static atomic counter combined with the address of this object
        // so each TempAgentDir gets a unique path, even within the same test case.
        static std::atomic<int> counter{0};
        int id = counter.fetch_add(1, std::memory_order_relaxed);
        path = fs::temp_directory_path() / ("batbox_test_agents_" +
               std::to_string(id) + "_" +
               std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(path);
    }

    ~TempAgentDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    // Write a .md file into this temp dir.
    void write(const std::string& filename, const std::string& content) {
        std::ofstream f(path / filename);
        f << content;
    }

    TempAgentDir(const TempAgentDir&)            = delete;
    TempAgentDir& operator=(const TempAgentDir&) = delete;
};

} // anonymous namespace

// =============================================================================
// TEST SUITE
// =============================================================================

TEST_SUITE("AgentLoader") {

// ---------------------------------------------------------------------------
// AC4: Integration test with fixture agents
// ---------------------------------------------------------------------------
TEST_CASE("load_from parses real-world fixture agents") {
    namespace fs = std::filesystem;

    fs::path fixtures = project_root() / "tests" / "fixtures" / "agents";
    REQUIRE(fs::exists(fixtures)); // fixtures dir must exist

    AgentLoader loader;
    auto specs = loader.load_from(fixtures);

    REQUIRE(!specs.empty());

    // senior-dev fixture should be present
    auto sd = loader.get("senior-dev");
    REQUIRE(sd.has_value());
    CHECK(sd->name == "senior-dev");
    CHECK(sd->description == "Senior developer agent for production-quality code");
    CHECK(sd->model.has_value());
    CHECK(sd->model.value() == "claude-opus-4-5");
    CHECK(!sd->prompt_body.empty());

    // allowed_tools for senior-dev (block list: Read, Write, Edit, Bash)
    auto& tools = sd->allowed_tools;
    CHECK(std::find(tools.begin(), tools.end(), "Read")  != tools.end());
    CHECK(std::find(tools.begin(), tools.end(), "Write") != tools.end());
    CHECK(std::find(tools.begin(), tools.end(), "Bash")  != tools.end());

    // qa-dev fixture (flow-style list)
    auto qd = loader.get("qa-dev");
    REQUIRE(qd.has_value());
    CHECK(qd->name == "qa-dev");
    CHECK(qd->allowed_tools.size() == 2);
    CHECK(std::find(qd->allowed_tools.begin(), qd->allowed_tools.end(), "Read") != qd->allowed_tools.end());

    // junior-dev fixture
    auto jd = loader.get("junior-dev");
    REQUIRE(jd.has_value());
    CHECK(jd->name == "junior-dev");
    // No model override
    CHECK(!jd->model.has_value());
}

// ---------------------------------------------------------------------------
// AC1: Loads N agent files — names() returns correct sorted list
// ---------------------------------------------------------------------------
TEST_CASE("names() returns sorted list of loaded agent names") {
    TempAgentDir tmp;
    tmp.write("zzz.md", "---\nname: zzz\ndescription: last\n---\nBody Z\n");
    tmp.write("aaa.md", "---\nname: aaa\ndescription: first\n---\nBody A\n");
    tmp.write("mmm.md", "---\nname: mmm\ndescription: middle\n---\nBody M\n");

    AgentLoader loader;
    loader.load_from(tmp.path);

    auto n = loader.names();
    REQUIRE(n.size() == 3);
    CHECK(n[0] == "aaa");
    CHECK(n[1] == "mmm");
    CHECK(n[2] == "zzz");
}

// ---------------------------------------------------------------------------
// AC3: Missing agents directory → empty map, no exception
// ---------------------------------------------------------------------------
TEST_CASE("missing agents directory returns empty map and does not throw") {
    AgentLoader loader;
    auto specs = loader.load_from("/tmp/batbox_agents_dir_definitely_does_not_exist_xyz789");
    CHECK(specs.empty());
    CHECK(loader.size() == 0);
    CHECK(loader.names().empty());
}

// ---------------------------------------------------------------------------
// AC2: [[ref]] cycle detection — has_cycle_error() returns true, error logged
// ---------------------------------------------------------------------------
TEST_CASE("cycle in [[ref]] graph is detected") {
    TempAgentDir tmp;
    // A references B, B references A — simple cycle.
    tmp.write("agent-a.md",
        "---\nname: agent-a\ndescription: references b\n---\nSee [[agent-b]]\n");
    tmp.write("agent-b.md",
        "---\nname: agent-b\ndescription: references a\n---\nSee [[agent-a]]\n");

    AgentLoader loader;
    loader.load_from(tmp.path);

    // Both agents must still be accessible despite the cycle.
    REQUIRE(loader.get("agent-a").has_value());
    REQUIRE(loader.get("agent-b").has_value());

    // Cycle must be flagged.
    CHECK(loader.has_cycle_error());
}

// ---------------------------------------------------------------------------
// No cycle → has_cycle_error() is false
// ---------------------------------------------------------------------------
TEST_CASE("no cycle: has_cycle_error() is false") {
    TempAgentDir tmp;
    // junior-dev references senior-dev (no cycle — senior-dev has no refs).
    tmp.write("senior-dev.md",
        "---\nname: senior-dev\ndescription: top-level\n---\nNo refs here.\n");
    tmp.write("junior-dev.md",
        "---\nname: junior-dev\ndescription: references senior\n---\nSee [[senior-dev]]\n");

    AgentLoader loader;
    loader.load_from(tmp.path);

    CHECK(!loader.has_cycle_error());
    CHECK(loader.size() == 2);
}

// ---------------------------------------------------------------------------
// Three-node cycle detection
// ---------------------------------------------------------------------------
TEST_CASE("three-node [[ref]] cycle is detected") {
    TempAgentDir tmp;
    tmp.write("a.md", "---\nname: a\ndescription: a\n---\n[[b]]\n");
    tmp.write("b.md", "---\nname: b\ndescription: b\n---\n[[c]]\n");
    tmp.write("c.md", "---\nname: c\ndescription: c\n---\n[[a]]\n");

    AgentLoader loader;
    loader.load_from(tmp.path);

    CHECK(loader.has_cycle_error());
    CHECK(loader.size() == 3);
}

// ---------------------------------------------------------------------------
// Mtime-based duplicate resolution: newer file wins
// ---------------------------------------------------------------------------
TEST_CASE("duplicate agent names: newer mtime wins") {
    namespace fs = std::filesystem;
    TempAgentDir dir_a;
    TempAgentDir dir_b;

    dir_a.write("agent.md",
        "---\nname: my-agent\ndescription: from-dir-a\n---\nBody A\n");
    dir_b.write("agent.md",
        "---\nname: my-agent\ndescription: from-dir-b\n---\nBody B\n");

    // Give dir_b's file a later mtime so it wins.
    std::error_code ec;
    auto future_time = fs::file_time_type::clock::now() + std::chrono::seconds(10);
    fs::last_write_time(dir_b.path / "agent.md", future_time, ec);
    REQUIRE(!ec);

    AgentLoader loader;
    loader.load_from(dir_a.path);
    loader.load_from(dir_b.path);

    REQUIRE(loader.size() == 1);
    auto spec = loader.get("my-agent");
    REQUIRE(spec.has_value());
    // dir_b has the later mtime, so "from-dir-b" wins.
    CHECK(spec->description == "from-dir-b");
    CHECK(spec->prompt_body == "Body B\n");
}

// ---------------------------------------------------------------------------
// Mtime-based duplicate resolution: older file loses
// ---------------------------------------------------------------------------
TEST_CASE("duplicate agent names: older mtime loses") {
    namespace fs = std::filesystem;
    TempAgentDir dir_a;
    TempAgentDir dir_b;

    dir_a.write("agent.md",
        "---\nname: my-agent\ndescription: from-dir-a\n---\nBody A\n");
    dir_b.write("agent.md",
        "---\nname: my-agent\ndescription: from-dir-b\n---\nBody B\n");

    // Give dir_a's file a later mtime so it wins over dir_b.
    std::error_code ec;
    auto future_time = fs::file_time_type::clock::now() + std::chrono::seconds(20);
    fs::last_write_time(dir_a.path / "agent.md", future_time, ec);
    REQUIRE(!ec);

    AgentLoader loader;
    loader.load_from(dir_a.path);
    loader.load_from(dir_b.path);

    REQUIRE(loader.size() == 1);
    auto spec = loader.get("my-agent");
    REQUIRE(spec.has_value());
    // dir_a was given the later mtime, so "from-dir-a" wins.
    CHECK(spec->description == "from-dir-a");
}

// ---------------------------------------------------------------------------
// reload() clears and re-scans (no standard user dirs in test env —
// verifies the clear + reload() contract)
// ---------------------------------------------------------------------------
TEST_CASE("reload() clears agents_ and re-runs load()") {
    AgentLoader loader;

    // Prime the loader with one agent via load_from.
    TempAgentDir tmp;
    tmp.write("agent.md",
        "---\nname: my-agent\ndescription: initial\n---\nBody\n");
    loader.load_from(tmp.path);
    REQUIRE(loader.size() == 1);

    // reload() calls load() which scans the default ~/.batbox/agents/.
    // In the test environment that dir may not exist, so after reload() the
    // agent loaded from tmp is gone (cleared) and load() returns empty or
    // whatever real agents the developer has installed.
    loader.reload();
    // We can't assert a specific count, but reload() must not throw.
    CHECK(loader.get("my-agent") == std::nullopt); // was cleared
}

// ---------------------------------------------------------------------------
// get() returns nullopt for unknown agent
// ---------------------------------------------------------------------------
TEST_CASE("get() returns nullopt for unknown agent name") {
    AgentLoader loader;
    CHECK(!loader.get("nonexistent-agent").has_value());
}

// ---------------------------------------------------------------------------
// Non-.md files in agents dir are ignored
// ---------------------------------------------------------------------------
TEST_CASE("non-.md files are ignored during scan") {
    TempAgentDir tmp;
    tmp.write("script.sh",  "#!/bin/bash\necho hi\n");
    tmp.write("notes.txt",  "some notes\n");
    tmp.write("agent.md",   "---\nname: one\ndescription: one\n---\nBody\n");

    AgentLoader loader;
    loader.load_from(tmp.path);

    CHECK(loader.size() == 1);
    CHECK(loader.get("one").has_value());
}

// ---------------------------------------------------------------------------
// Malformed frontmatter is silently skipped
// ---------------------------------------------------------------------------
TEST_CASE("malformed frontmatter is skipped, valid file still loads") {
    TempAgentDir tmp;
    tmp.write("broken.md",
        "---\nname: broken\ndescription: no closing delimiter\n");
    tmp.write("good.md",
        "---\nname: good\ndescription: fine\n---\nBody\n");

    AgentLoader loader;
    loader.load_from(tmp.path);

    CHECK(!loader.get("broken").has_value());
    CHECK(loader.get("good").has_value());
}

// ---------------------------------------------------------------------------
// Missing "name" field is silently skipped
// ---------------------------------------------------------------------------
TEST_CASE("missing 'name' field causes file to be skipped") {
    TempAgentDir tmp;
    tmp.write("no_name.md",
        "---\ndescription: no name here\n---\nBody\n");
    tmp.write("valid.md",
        "---\nname: valid-agent\ndescription: has name\n---\nBody\n");

    AgentLoader loader;
    loader.load_from(tmp.path);

    CHECK(loader.size() == 1);
    CHECK(loader.get("valid-agent").has_value());
}

// ---------------------------------------------------------------------------
// File with no frontmatter (no leading ---) treated as body-only → no name → skipped
// ---------------------------------------------------------------------------
TEST_CASE("file with no frontmatter is skipped (no name field)") {
    TempAgentDir tmp;
    tmp.write("no_fm.md", "# No Frontmatter\n\nJust raw markdown.\n");

    AgentLoader loader;
    loader.load_from(tmp.path);

    CHECK(loader.size() == 0);
}

// ---------------------------------------------------------------------------
// size() reflects count
// ---------------------------------------------------------------------------
TEST_CASE("size() reflects total loaded agent count") {
    TempAgentDir tmp;
    for (int i = 0; i < 5; ++i) {
        std::string name = "agent" + std::to_string(i);
        tmp.write(name + ".md",
            "---\nname: " + name + "\ndescription: d\n---\nBody\n");
    }

    AgentLoader loader;
    loader.load_from(tmp.path);
    CHECK(loader.size() == 5);
}

// ---------------------------------------------------------------------------
// source_path is set to the .md file's filesystem path
// ---------------------------------------------------------------------------
TEST_CASE("source_path is set to the .md file path") {
    TempAgentDir tmp;
    tmp.write("my-agent.md",
        "---\nname: my-agent\ndescription: d\n---\nBody\n");

    AgentLoader loader;
    loader.load_from(tmp.path);

    auto spec = loader.get("my-agent");
    REQUIRE(spec.has_value());
    CHECK(spec->source_path == tmp.path / "my-agent.md");
}

// ---------------------------------------------------------------------------
// load_from returns the AgentSpec vector directly (not just via side effects)
// ---------------------------------------------------------------------------
TEST_CASE("load_from returns vector of parsed AgentSpecs") {
    TempAgentDir tmp;
    tmp.write("a.md", "---\nname: a\ndescription: first\n---\nBody A\n");
    tmp.write("b.md", "---\nname: b\ndescription: second\n---\nBody B\n");

    AgentLoader loader;
    auto specs = loader.load_from(tmp.path);

    REQUIRE(specs.size() == 2);
    // Returned vector is alphabetically sorted.
    CHECK(specs[0].name == "a");
    CHECK(specs[1].name == "b");
}

// ---------------------------------------------------------------------------
// [[ref]] to a non-existent agent does not produce a cycle or crash
// ---------------------------------------------------------------------------
TEST_CASE("[[ref]] to non-existent agent: no cycle, no crash") {
    TempAgentDir tmp;
    tmp.write("orphan.md",
        "---\nname: orphan\ndescription: references ghost\n---\nSee [[ghost]]\n");

    AgentLoader loader;
    loader.load_from(tmp.path);

    CHECK(!loader.has_cycle_error());
    CHECK(loader.get("orphan").has_value());
}

} // TEST_SUITE("AgentLoader")
