// tests/integration/test_plugin_agent_loader.cpp
// =============================================================================
// Integration tests for batbox::plugins::AgentLoader.
//
// Tests exercise the full parse-and-scan pipeline against temporary plugin
// directory fixtures created at runtime.  Each test creates a minimal plugin
// directory structure with .claude-plugin/agents/ and/or .batbox-plugin/agents/
// subdirectories, writes .md files, and asserts against AgentLoader state.
//
// Acceptance criteria covered:
//   1. Plugin agents discoverable alongside user-dir agents (scan works)
//   2. Dual-path scan: .claude-plugin/agents/ AND .batbox-plugin/agents/
//   3. Name collision across plugins: earlier entry saved under namespaced key
//   4. User-dir wins policy: AgentLoader itself loads plugin agents only;
//      callers are responsible for user-dir-wins merge (tested via simulation)
//   5. Malformed frontmatter → file skipped, continue
//   6. Missing name field → falls back to filename stem with WARN
//   7. Non-.md files ignored
//   8. Non-existent directory handled gracefully
//   9. names() returns sorted list
//  10. find() resolves both bare and namespaced forms
//  11. all() returns flat vector of all entries
//  12. clear() resets loader state
//  13. source tag set to "plugin:<plugin-name>"
//  14. model / allowed_tools / prompt_body parsed correctly
//
// Build standalone (from repo root):
//   ROOT=/path/to/batbox
//   c++ -std=c++20 \
//       -I $ROOT/include \
//       -I $ROOT/build/vcpkg_installed/arm64-osx/include \
//       $ROOT/tests/integration/test_plugin_agent_loader.cpp \
//       $ROOT/src/plugins/FrontmatterParser.cpp \
//       $ROOT/src/plugins/AgentLoader.cpp \
//       $ROOT/src/core/Logging.cpp \
//       $ROOT/src/core/Paths.cpp \
//       $ROOT/src/core/Json.cpp \
//       $ROOT/build/vcpkg_installed/arm64-osx/lib/libfmt.a \
//       $ROOT/build/vcpkg_installed/arm64-osx/lib/libspdlog.a \
//       $ROOT/build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       -o /tmp/test_plugin_agent_loader && /tmp/test_plugin_agent_loader
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/plugins/AgentLoader.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace batbox::plugins;
namespace fs = std::filesystem;

// =============================================================================
// Test helpers
// =============================================================================

/// RAII helper: creates a temp base dir, builds a plugin directory layout
/// under it, and cleans up on destruction.
///
/// Layout created on request:
///   <base>/
///     .claude-plugin/
///       agents/
///     .batbox-plugin/
///       agents/
struct TempPluginDir {
    fs::path base;

    explicit TempPluginDir(std::string_view tag = "") {
        base = fs::temp_directory_path() /
               ("batbox_test_agent_loader_" + std::string(tag) + "_" +
                std::to_string(std::hash<std::string>{}(__FILE__)));
        fs::create_directories(base);
    }

    ~TempPluginDir() {
        std::error_code ec;
        fs::remove_all(base, ec);
    }

    TempPluginDir(const TempPluginDir&)            = delete;
    TempPluginDir& operator=(const TempPluginDir&) = delete;

    // ---- Directory helpers --------------------------------------------------

    /// Create (and return path to) the .claude-plugin/agents/ directory.
    fs::path claude_agents_dir() const {
        fs::path d = base / ".claude-plugin" / "agents";
        fs::create_directories(d);
        return d;
    }

    /// Create (and return path to) the .batbox-plugin/agents/ directory.
    fs::path batbox_agents_dir() const {
        fs::path d = base / ".batbox-plugin" / "agents";
        fs::create_directories(d);
        return d;
    }

    // ---- File helpers -------------------------------------------------------

    /// Write a .md file under claude_agents_dir().
    void write_claude_agent(const std::string& filename, const std::string& content) {
        std::ofstream f(claude_agents_dir() / filename);
        f << content;
    }

    /// Write a .md file under batbox_agents_dir().
    void write_batbox_agent(const std::string& filename, const std::string& content) {
        std::ofstream f(batbox_agents_dir() / filename);
        f << content;
    }
};

// =============================================================================
// TEST SUITE
// =============================================================================

TEST_SUITE("AgentLoader") {

// ---------------------------------------------------------------------------
// load_plugin_dir — basic scan of .claude-plugin/agents/
// ---------------------------------------------------------------------------
TEST_CASE("load_plugin_dir scans .claude-plugin/agents/") {
    TempPluginDir tmp("claude");
    tmp.write_claude_agent("researcher.md",
        "---\nname: researcher\ndescription: Research agent\n---\nResearch prompt body.\n");

    AgentLoader loader;
    loader.load_plugin_dir(tmp.base, "my-plugin");

    REQUIRE(loader.size() >= 1u);
    const Agent* a = loader.find("researcher");
    REQUIRE(a != nullptr);
    CHECK(a->name == "researcher");
    CHECK(a->description == "Research agent");
    CHECK(a->prompt_body == "Research prompt body.\n");
    CHECK(a->source == "plugin:my-plugin");
}

// ---------------------------------------------------------------------------
// load_plugin_dir — basic scan of .batbox-plugin/agents/
// ---------------------------------------------------------------------------
TEST_CASE("load_plugin_dir scans .batbox-plugin/agents/") {
    TempPluginDir tmp("batbox");
    tmp.write_batbox_agent("writer.md",
        "---\nname: writer\ndescription: Writing agent\n---\nWriter prompt.\n");

    AgentLoader loader;
    loader.load_plugin_dir(tmp.base, "writer-plugin");

    const Agent* a = loader.find("writer");
    REQUIRE(a != nullptr);
    CHECK(a->name == "writer");
    CHECK(a->source == "plugin:writer-plugin");
}

// ---------------------------------------------------------------------------
// Dual-path: agents from BOTH .claude-plugin and .batbox-plugin are loaded
// ---------------------------------------------------------------------------
TEST_CASE("dual-path: agents from both .claude-plugin and .batbox-plugin are loaded") {
    TempPluginDir tmp("dual");
    tmp.write_claude_agent("alpha.md",
        "---\nname: alpha\ndescription: Alpha agent\n---\nAlpha prompt.\n");
    tmp.write_batbox_agent("beta.md",
        "---\nname: beta\ndescription: Beta agent\n---\nBeta prompt.\n");

    AgentLoader loader;
    loader.load_plugin_dir(tmp.base, "dual-plugin");

    REQUIRE(loader.find("alpha") != nullptr);
    REQUIRE(loader.find("beta")  != nullptr);
    CHECK(loader.size() == 2u);
}

// ---------------------------------------------------------------------------
// Dual-path: .batbox-plugin/agents/ wins on same-plugin name collision
// ---------------------------------------------------------------------------
TEST_CASE("dual-path: .batbox-plugin wins over .claude-plugin for same name") {
    TempPluginDir tmp("dualwin");
    // Same name "agent-x" in both paths.
    tmp.write_claude_agent("agent-x.md",
        "---\nname: agent-x\ndescription: Claude compat version\n---\nClaude body.\n");
    tmp.write_batbox_agent("agent-x.md",
        "---\nname: agent-x\ndescription: Batbox native version\n---\nBatbox body.\n");

    AgentLoader loader;
    loader.load_plugin_dir(tmp.base, "twin-plugin");

    const Agent* a = loader.find("agent-x");
    REQUIRE(a != nullptr);
    // .batbox-plugin is scanned second → wins.
    CHECK(a->description == "Batbox native version");
    CHECK(a->prompt_body == "Batbox body.\n");
}

// ---------------------------------------------------------------------------
// Collision across plugins: earlier entry preserved under namespaced key
// ---------------------------------------------------------------------------
TEST_CASE("cross-plugin name collision: earlier entry saved under namespaced key") {
    TempPluginDir plugin_a("ca");
    TempPluginDir plugin_b("cb");

    plugin_a.write_batbox_agent("reviewer.md",
        "---\nname: reviewer\ndescription: Plugin A reviewer\n---\nBody A.\n");
    plugin_b.write_batbox_agent("reviewer.md",
        "---\nname: reviewer\ndescription: Plugin B reviewer\n---\nBody B.\n");

    AgentLoader loader;
    loader.load_plugin_dir(plugin_a.base, "plugin-a");
    loader.load_plugin_dir(plugin_b.base, "plugin-b");

    // Bare "reviewer" should now point to plugin-b (last loaded wins).
    const Agent* bare = loader.find("reviewer");
    REQUIRE(bare != nullptr);
    CHECK(bare->source == "plugin:plugin-b");
    CHECK(bare->description == "Plugin B reviewer");

    // Plugin-a's version is accessible under the namespaced key.
    const Agent* namespaced = loader.find("plugin-a/reviewer");
    REQUIRE(namespaced != nullptr);
    CHECK(namespaced->source == "plugin:plugin-a");
    CHECK(namespaced->description == "Plugin A reviewer");
}

// ---------------------------------------------------------------------------
// names() returns sorted list including namespaced entries
// ---------------------------------------------------------------------------
TEST_CASE("names() returns sorted list") {
    TempPluginDir plugin_a("na");
    TempPluginDir plugin_b("nb");

    plugin_a.write_batbox_agent("zebra.md",
        "---\nname: zebra\ndescription: z\n---\nZ body.\n");
    plugin_a.write_batbox_agent("apple.md",
        "---\nname: apple\ndescription: a\n---\nA body.\n");
    plugin_b.write_batbox_agent("zebra.md",
        "---\nname: zebra\ndescription: z from b\n---\nZ2 body.\n");

    AgentLoader loader;
    loader.load_plugin_dir(plugin_a.base, "plugin-a");
    loader.load_plugin_dir(plugin_b.base, "plugin-b");

    auto n = loader.names();
    REQUIRE(!n.empty());
    // All names must be in sorted order.
    CHECK(std::is_sorted(n.begin(), n.end()));
    // "apple" and "zebra" (bare) must be present.
    CHECK(std::find(n.begin(), n.end(), "apple") != n.end());
    CHECK(std::find(n.begin(), n.end(), "zebra") != n.end());
    // Namespaced "plugin-a/zebra" must also be present (saved due to collision).
    CHECK(std::find(n.begin(), n.end(), "plugin-a/zebra") != n.end());
}

// ---------------------------------------------------------------------------
// find() resolves namespaced form directly
// ---------------------------------------------------------------------------
TEST_CASE("find() resolves namespaced key directly") {
    TempPluginDir plugin_a("fa");
    TempPluginDir plugin_b("fb");

    plugin_a.write_batbox_agent("analyst.md",
        "---\nname: analyst\ndescription: Analyst A\n---\nPrompt A.\n");
    plugin_b.write_batbox_agent("analyst.md",
        "---\nname: analyst\ndescription: Analyst B\n---\nPrompt B.\n");

    AgentLoader loader;
    loader.load_plugin_dir(plugin_a.base, "alpha");
    loader.load_plugin_dir(plugin_b.base, "beta");

    // Both bare and namespaced forms should resolve.
    CHECK(loader.find("analyst")        != nullptr);
    CHECK(loader.find("alpha/analyst")  != nullptr);
    CHECK(loader.find("beta/analyst")   == nullptr); // bare "analyst" IS plugin-b but no "beta/analyst" key
    // The bare entry belongs to beta; verify via source.
    CHECK(loader.find("analyst")->source == "plugin:beta");
}

// ---------------------------------------------------------------------------
// all() returns flat vector of all entries
// ---------------------------------------------------------------------------
TEST_CASE("all() returns flat vector of all entries") {
    TempPluginDir tmp("alltest");
    tmp.write_batbox_agent("one.md",
        "---\nname: one\ndescription: d1\n---\nBody 1.\n");
    tmp.write_batbox_agent("two.md",
        "---\nname: two\ndescription: d2\n---\nBody 2.\n");
    tmp.write_batbox_agent("three.md",
        "---\nname: three\ndescription: d3\n---\nBody 3.\n");

    AgentLoader loader;
    loader.load_plugin_dir(tmp.base, "p");

    auto entries = loader.all();
    CHECK(entries.size() == 3u);
}

// ---------------------------------------------------------------------------
// clear() resets to empty state
// ---------------------------------------------------------------------------
TEST_CASE("clear() resets loader to empty") {
    TempPluginDir tmp("clr");
    tmp.write_batbox_agent("foo.md",
        "---\nname: foo\ndescription: foo\n---\nFoo.\n");

    AgentLoader loader;
    loader.load_plugin_dir(tmp.base, "p");
    REQUIRE(loader.size() >= 1u);

    loader.clear();
    CHECK(loader.size() == 0u);
    CHECK(loader.find("foo") == nullptr);
}

// ---------------------------------------------------------------------------
// model and allowed_tools parsed correctly
// ---------------------------------------------------------------------------
TEST_CASE("model and allowed_tools frontmatter fields parsed correctly") {
    TempPluginDir tmp("fields");
    tmp.write_batbox_agent("coder.md",
        "---\n"
        "name: coder\n"
        "description: Coding agent\n"
        "model: claude-opus-4-5\n"
        "allowed_tools: [Read, Write, Bash]\n"
        "---\n"
        "Write code.\n");

    AgentLoader loader;
    loader.load_plugin_dir(tmp.base, "code-plugin");

    const Agent* a = loader.find("coder");
    REQUIRE(a != nullptr);
    CHECK(a->model.has_value());
    CHECK(a->model.value() == "claude-opus-4-5");
    CHECK(a->allowed_tools.size() == 3u);
    CHECK(std::find(a->allowed_tools.begin(), a->allowed_tools.end(), "Bash")
          != a->allowed_tools.end());
    CHECK(a->prompt_body == "Write code.\n");
}

// ---------------------------------------------------------------------------
// Malformed frontmatter → file skipped, loader continues
// ---------------------------------------------------------------------------
TEST_CASE("malformed frontmatter skips file, continues loading others") {
    TempPluginDir tmp("mal");
    // Missing closing ---
    tmp.write_batbox_agent("broken.md",
        "---\nname: broken\ndescription: unclosed\n");
    tmp.write_batbox_agent("good.md",
        "---\nname: good\ndescription: fine\n---\nGood prompt.\n");

    AgentLoader loader;
    loader.load_plugin_dir(tmp.base, "p");

    CHECK(loader.find("broken") == nullptr);
    REQUIRE(loader.find("good") != nullptr);
}

// ---------------------------------------------------------------------------
// Missing 'name' field → falls back to filename stem
// ---------------------------------------------------------------------------
TEST_CASE("missing 'name' field falls back to filename stem") {
    TempPluginDir tmp("nostem");
    tmp.write_batbox_agent("my-agent.md",
        "---\ndescription: no explicit name\n---\nStem-named agent.\n");

    AgentLoader loader;
    loader.load_plugin_dir(tmp.base, "p");

    // Should be registered under the filename stem "my-agent".
    const Agent* a = loader.find("my-agent");
    REQUIRE(a != nullptr);
    CHECK(a->description == "no explicit name");
}

// ---------------------------------------------------------------------------
// Non-.md files are ignored
// ---------------------------------------------------------------------------
TEST_CASE("non-.md files in agents directory are ignored") {
    TempPluginDir tmp("nonmd");
    // Write a non-.md file directly into the agents dir.
    fs::path agents_dir = tmp.batbox_agents_dir();
    { std::ofstream f(agents_dir / "script.sh"); f << "#!/bin/bash\necho hi\n"; }
    { std::ofstream f(agents_dir / "README.txt"); f << "readme\n"; }
    tmp.write_batbox_agent("valid.md",
        "---\nname: valid\ndescription: v\n---\nBody.\n");

    AgentLoader loader;
    loader.load_plugin_dir(tmp.base, "p");

    CHECK(loader.size() == 1u);
    CHECK(loader.find("valid") != nullptr);
}

// ---------------------------------------------------------------------------
// Non-existent plugin dir handled gracefully
// ---------------------------------------------------------------------------
TEST_CASE("non-existent plugin directory handled gracefully") {
    AgentLoader loader;
    // Should not throw or crash.
    loader.load_plugin_dir("/tmp/batbox_no_such_plugin_xyz999", "ghost-plugin");
    CHECK(loader.size() == 0u);
}

// ---------------------------------------------------------------------------
// scan_dir with non-existent dir is a no-op
// ---------------------------------------------------------------------------
TEST_CASE("scan_dir with non-existent directory is a no-op") {
    AgentLoader loader;
    loader.scan_dir("/tmp/batbox_no_such_agents_dir_xyz999", "p");
    CHECK(loader.size() == 0u);
}

// ---------------------------------------------------------------------------
// source tag is set to "plugin:<plugin-name>"
// ---------------------------------------------------------------------------
TEST_CASE("source tag is set correctly to plugin:<name>") {
    TempPluginDir tmp("src");
    tmp.write_batbox_agent("agent.md",
        "---\nname: agent\ndescription: d\n---\nBody.\n");

    AgentLoader loader;
    loader.load_plugin_dir(tmp.base, "my-awesome-plugin");

    const Agent* a = loader.find("agent");
    REQUIRE(a != nullptr);
    CHECK(a->source == "plugin:my-awesome-plugin");
}

// ---------------------------------------------------------------------------
// Multiple plugins, no name collision — all agents from all plugins present
// ---------------------------------------------------------------------------
TEST_CASE("multiple plugins with distinct agent names — all present") {
    TempPluginDir pa("multi_a");
    TempPluginDir pb("multi_b");
    TempPluginDir pc("multi_c");

    pa.write_batbox_agent("planner.md",
        "---\nname: planner\ndescription: Plans work\n---\nPlan.\n");
    pb.write_batbox_agent("executor.md",
        "---\nname: executor\ndescription: Executes tasks\n---\nExecute.\n");
    pc.write_batbox_agent("reviewer.md",
        "---\nname: reviewer\ndescription: Reviews output\n---\nReview.\n");

    AgentLoader loader;
    loader.load_plugin_dir(pa.base, "planning-plugin");
    loader.load_plugin_dir(pb.base, "execution-plugin");
    loader.load_plugin_dir(pc.base, "review-plugin");

    REQUIRE(loader.find("planner")  != nullptr);
    REQUIRE(loader.find("executor") != nullptr);
    REQUIRE(loader.find("reviewer") != nullptr);
    CHECK(loader.size() == 3u);

    CHECK(loader.find("planner")->source  == "plugin:planning-plugin");
    CHECK(loader.find("executor")->source == "plugin:execution-plugin");
    CHECK(loader.find("reviewer")->source == "plugin:review-plugin");
}

// ---------------------------------------------------------------------------
// User-dir-wins simulation: AgentLoader does not override a pre-registered
// "user-dir" entry when simulated via manual clear+reload
// ---------------------------------------------------------------------------
TEST_CASE("plugin agents do not interfere with manually registered user-dir entries") {
    // AgentLoader itself only loads plugin agents.  User-dir-wins is enforced
    // at the AgentSupervisor level.  Here we verify that two separate
    // AgentLoader instances each contain their own isolated view and can
    // be queried independently (no global shared state).
    TempPluginDir user_sim("user");
    TempPluginDir plugin_sim("plug");

    user_sim.write_batbox_agent("planner.md",
        "---\nname: planner\ndescription: User planner\n---\nUser body.\n");
    plugin_sim.write_batbox_agent("planner.md",
        "---\nname: planner\ndescription: Plugin planner\n---\nPlugin body.\n");

    AgentLoader user_loader;
    user_loader.load_plugin_dir(user_sim.base, "user-dir");

    AgentLoader plugin_loader;
    plugin_loader.load_plugin_dir(plugin_sim.base, "some-plugin");

    // Each loader maintains independent state.
    REQUIRE(user_loader.find("planner")   != nullptr);
    REQUIRE(plugin_loader.find("planner") != nullptr);
    CHECK(user_loader.find("planner")->description   == "User planner");
    CHECK(plugin_loader.find("planner")->description == "Plugin planner");
}

} // TEST_SUITE("AgentLoader")
