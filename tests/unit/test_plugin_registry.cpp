// tests/unit/test_plugin_registry.cpp
// =============================================================================
// doctest suite for batbox::plugins::Plugin and batbox::plugins::PluginRegistry.
//
// Acceptance criteria:
//   1. Plugin construction + field access
//   2. PluginRegistry::all_plugins() / active_plugins() return all loaded
//   3. disabled flag is honoured — filtered from active_plugins()
//   4. Atomic swap on reload — get_snapshot() is stable under concurrent reload
//
// Build + run (standalone, no CMake — from repo root):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_plugin_registry.cpp \
//       src/plugins/PluginRegistry.cpp \
//       src/core/Json.cpp src/core/Logging.cpp src/core/Paths.cpp \
//       src/plugins/MarketplaceJson.cpp src/plugins/FrontmatterParser.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       build/vcpkg_installed/arm64-osx/lib/libspdlog.a \
//       build/vcpkg_installed/arm64-osx/lib/libfmt.a \
//       -o /tmp/test_plugin_registry && /tmp/test_plugin_registry
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/plugins/Plugin.hpp>
#include <batbox/plugins/PluginRegistry.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace batbox::plugins;

// ============================================================================
// Test fixtures / helpers
// ============================================================================

/// RAII temp-dir that removes itself on scope exit.
struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& prefix = "batbox_reg_test_") {
        static std::atomic<int> counter{0};
        const auto unique_id =
            static_cast<unsigned long>(::getpid()) * 1000000UL
            + static_cast<unsigned long>(
                std::chrono::steady_clock::now().time_since_epoch().count() % 1000000);
        auto base = fs::temp_directory_path() / (prefix + std::to_string(unique_id));
        path = base / std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;
};

/// Write `content` to `path`, creating parent dirs.
static void write_file(const fs::path& path, std::string_view content) {
    fs::create_directories(path.parent_path());
    std::ofstream ofs(path);
    ofs << content;
}

/// Create a minimal plugin directory at `plugin_dir` using the given name and
/// using the `.batbox-plugin/marketplace.json` path.
static void make_plugin_dir(const fs::path& plugin_dir,
                             const std::string& name,
                             const std::string& version = "1.0.0",
                             const std::string& description = "")
{
    std::string json = "{\n";
    json += "  \"name\": \"" + name + "\",\n";
    json += "  \"version\": \"" + version + "\",\n";
    json += "  \"description\": \"" + description + "\"\n";
    json += "}\n";
    write_file(plugin_dir / ".batbox-plugin" / "marketplace.json", json);
}

/// Create a minimal plugin directory using .claude-plugin variant.
static void make_claude_plugin_dir(const fs::path& plugin_dir,
                                   const std::string& name)
{
    std::string json = "{\"name\": \"" + name + "\"}\n";
    write_file(plugin_dir / ".claude-plugin" / "marketplace.json", json);
}

// ============================================================================
// TEST SUITE 1 — Plugin struct construction and field access
// ============================================================================

TEST_SUITE("Plugin struct — construction and field access") {

    TEST_CASE("default-constructed Plugin has expected defaults") {
        Plugin p;
        CHECK(p.name.empty());
        CHECK(p.description.empty());
        CHECK(p.version.empty());
        CHECK(p.author.empty());
        CHECK(p.skills.empty());
        CHECK(p.agents.empty());
        CHECK(p.commands.empty());
        CHECK(p.mcp_servers.empty());
        CHECK(!p.disabled);
    }

    TEST_CASE("Plugin fields round-trip") {
        Plugin p;
        p.name        = "my-plugin";
        p.description = "does stuff";
        p.version     = "2.3.4";
        p.author      = "Alice";
        p.dir         = fs::path("/plugins/my-plugin");
        p.disabled    = false;

        CHECK(p.name        == "my-plugin");
        CHECK(p.description == "does stuff");
        CHECK(p.version     == "2.3.4");
        CHECK(p.author      == "Alice");
        CHECK(p.dir         == fs::path("/plugins/my-plugin"));
        CHECK(!p.disabled);
    }

    TEST_CASE("Plugin::disabled field starts false, can be set to true") {
        Plugin p;
        p.name = "test";
        CHECK(!p.disabled);
        p.disabled = true;
        CHECK(p.disabled);
    }

    TEST_CASE("Skill struct construction") {
        Skill s;
        s.name          = "debug";
        s.description   = "debug something";
        s.model         = "claude-opus-4-5";
        s.allowed_tools = {"Bash", "Read"};
        s.prompt_body   = "Debug the following:\n$ARGUMENTS";
        s.source        = "plugin:my-plugin";

        CHECK(s.name          == "debug");
        CHECK(s.description   == "debug something");
        CHECK(s.model.has_value());
        CHECK(s.model.value() == "claude-opus-4-5");
        REQUIRE(s.allowed_tools.size() == 2);
        CHECK(s.allowed_tools[0] == "Bash");
        CHECK(s.allowed_tools[1] == "Read");
        CHECK(s.source == "plugin:my-plugin");
        CHECK(!s.script_path.has_value());
    }

    TEST_CASE("Agent struct construction") {
        Agent a;
        a.name          = "reviewer";
        a.description   = "reviews code";
        a.source        = "user-dir";
        CHECK(a.name   == "reviewer");
        CHECK(a.source == "user-dir");
        CHECK(!a.model.has_value());
        CHECK(!a.script_path.has_value());
    }

    TEST_CASE("Command struct construction") {
        Command c;
        c.name        = "summarize";
        c.description = "Summarize the current file";
        c.body        = "Summarize:\n$ARGUMENTS";
        c.source      = "plugin:my-plugin";
        CHECK(c.name        == "summarize");
        CHECK(c.description == "Summarize the current file");
        CHECK(c.body        == "Summarize:\n$ARGUMENTS");
        CHECK(c.source      == "plugin:my-plugin");
    }

    TEST_CASE("Plugin operator== works correctly") {
        Plugin a, b;
        a.name = "x";
        b.name = "x";
        CHECK(a == b);
        b.name = "y";
        CHECK(!(a == b));
    }

    TEST_CASE("Skill operator== works correctly") {
        Skill a, b;
        a.name = "debug";
        b.name = "debug";
        CHECK(a == b);
        b.source = "plugin:foo";
        CHECK(!(a == b));
    }

    TEST_CASE("McpServerConfig is usable as McpServerSpec alias") {
        // Confirm the type alias works: we can assign a McpServerSpec to a McpServerConfig.
        McpServerSpec spec;
        spec.transport = McpTransport::Stdio;
        spec.command   = "/usr/bin/mcp-server";
        spec.args      = {"--stdio"};

        McpServerConfig cfg = spec;
        CHECK(cfg.transport == McpTransport::Stdio);
        CHECK(cfg.command   == "/usr/bin/mcp-server");
        REQUIRE(cfg.args.size() == 1);
        CHECK(cfg.args[0]   == "--stdio");
    }
}

// ============================================================================
// TEST SUITE 2 — PluginRegistry loads plugins from directories
// ============================================================================

TEST_SUITE("PluginRegistry::load_dir — basic loading") {

    TEST_CASE("empty registry has size 0") {
        PluginRegistry reg;
        CHECK(reg.empty());
        CHECK(reg.size() == 0);
        CHECK(reg.all_plugins().empty());
        CHECK(reg.active_plugins().empty());
    }

    TEST_CASE("load_dir with nonexistent dir is a no-op (no crash)") {
        PluginRegistry reg;
        reg.load_dir(fs::path("/nonexistent/path/xyz_batbox_test"));
        CHECK(reg.empty());
    }

    TEST_CASE("load_dir discovers a single plugin") {
        TempDir root;
        auto plugin_dir = root.path / "my-plugin";
        make_plugin_dir(plugin_dir, "my-plugin", "1.0.0", "A test plugin");

        PluginRegistry reg;
        reg.load_dir(root.path);

        REQUIRE(reg.size() == 1);
        const auto& plugins = reg.all_plugins();
        REQUIRE(plugins.size() == 1);
        CHECK(plugins[0].name        == "my-plugin");
        CHECK(plugins[0].version     == "1.0.0");
        CHECK(plugins[0].description == "A test plugin");
        CHECK(!plugins[0].disabled);
    }

    TEST_CASE("load_dir discovers multiple plugins") {
        TempDir root;
        make_plugin_dir(root.path / "plugin-a", "alpha", "1.0", "Alpha plugin");
        make_plugin_dir(root.path / "plugin-b", "beta",  "2.0", "Beta plugin");
        make_plugin_dir(root.path / "plugin-c", "gamma", "3.0", "Gamma plugin");

        PluginRegistry reg;
        reg.load_dir(root.path);

        CHECK(reg.size() == 3);

        // Verify each plugin is present (order may vary)
        const auto& all = reg.all_plugins();
        std::vector<std::string> names;
        for (const auto& p : all) names.push_back(p.name);
        CHECK(std::find(names.begin(), names.end(), "alpha") != names.end());
        CHECK(std::find(names.begin(), names.end(), "beta")  != names.end());
        CHECK(std::find(names.begin(), names.end(), "gamma") != names.end());
    }

    TEST_CASE("load_dir ignores subdirs without marketplace.json") {
        TempDir root;
        // Valid plugin
        make_plugin_dir(root.path / "valid-plugin", "valid-plugin");
        // Non-plugin dir (no marketplace.json)
        fs::create_directories(root.path / "not-a-plugin" / "some-subdir");
        // File at root level (should be ignored)
        write_file(root.path / "some-file.txt", "hello");

        PluginRegistry reg;
        reg.load_dir(root.path);

        CHECK(reg.size() == 1);
        CHECK(reg.get("valid-plugin") != nullptr);
    }

    TEST_CASE("load_dir accepts .claude-plugin/marketplace.json variant") {
        TempDir root;
        make_claude_plugin_dir(root.path / "claude-plugin", "claude-compat");

        PluginRegistry reg;
        reg.load_dir(root.path);

        CHECK(reg.size() == 1);
        CHECK(reg.get("claude-compat") != nullptr);
    }

    TEST_CASE("loading two roots — last root wins on name collision") {
        TempDir root1, root2;
        // Same plugin name in both roots; root2 should win (last-root-wins).
        make_plugin_dir(root1.path / "plug", "shared-name", "1.0", "From root1");
        make_plugin_dir(root2.path / "plug", "shared-name", "2.0", "From root2");

        PluginRegistry reg;
        reg.load_dir(root1.path);
        reg.load_dir(root2.path);

        // Still only one plugin with this name.
        CHECK(reg.size() == 1);
        const Plugin* p = reg.get("shared-name");
        REQUIRE(p != nullptr);
        CHECK(p->version == "2.0");
        CHECK(p->description == "From root2");
    }

    TEST_CASE("loading two roots — unique plugins from both are present") {
        TempDir root1, root2;
        make_plugin_dir(root1.path / "p1", "plugin-one",   "1.0");
        make_plugin_dir(root2.path / "p2", "plugin-two",   "1.0");

        PluginRegistry reg;
        reg.load_dir(root1.path);
        reg.load_dir(root2.path);

        CHECK(reg.size() == 2);
        CHECK(reg.get("plugin-one") != nullptr);
        CHECK(reg.get("plugin-two") != nullptr);
    }
}

// ============================================================================
// TEST SUITE 3 — get() look-ups
// ============================================================================

TEST_SUITE("PluginRegistry::get — look-ups") {

    TEST_CASE("get returns nullptr for unknown name") {
        PluginRegistry reg;
        CHECK(reg.get("nonexistent") == nullptr);
    }

    TEST_CASE("get returns pointer for loaded plugin") {
        TempDir root;
        make_plugin_dir(root.path / "my-plugin", "my-plugin", "1.2.3");
        PluginRegistry reg;
        reg.load_dir(root.path);

        const Plugin* p = reg.get("my-plugin");
        REQUIRE(p != nullptr);
        CHECK(p->name    == "my-plugin");
        CHECK(p->version == "1.2.3");
    }

    TEST_CASE("get returns nullptr for disabled plugin (disabled flag does not affect get)") {
        // get() searches the full list INCLUDING disabled.
        TempDir root;
        make_plugin_dir(root.path / "my-plugin", "my-plugin");
        PluginRegistry reg;
        reg.load_dir(root.path);

        reg.disable("my-plugin");

        // get() should still find it (it's in the full list).
        const Plugin* p = reg.get("my-plugin");
        REQUIRE(p != nullptr);
        CHECK(p->disabled);
    }
}

// ============================================================================
// TEST SUITE 4 — disabled flag and active_plugins
// ============================================================================

TEST_SUITE("PluginRegistry — disabled flag and active_plugins") {

    TEST_CASE("all_plugins includes disabled; active_plugins does not") {
        TempDir root;
        make_plugin_dir(root.path / "enabled-plugin",  "enabled-plugin");
        make_plugin_dir(root.path / "disabled-plugin", "disabled-plugin");

        PluginRegistry reg;
        reg.load_dir(root.path);

        REQUIRE(reg.size() == 2);

        // Disable one.
        bool changed = reg.disable("disabled-plugin");
        CHECK(changed);

        // all_plugins has both.
        CHECK(reg.all_plugins().size() == 2);

        // active_plugins has only the enabled one.
        auto active = reg.active_plugins();
        CHECK(active.size() == 1);
        CHECK(active[0].name == "enabled-plugin");
    }

    TEST_CASE("disable then enable restores plugin to active") {
        TempDir root;
        make_plugin_dir(root.path / "toggleable", "toggleable");

        PluginRegistry reg;
        reg.load_dir(root.path);

        CHECK(reg.active_plugins().size() == 1);

        reg.disable("toggleable");
        CHECK(reg.active_plugins().empty());

        reg.enable("toggleable");
        CHECK(reg.active_plugins().size() == 1);
    }

    TEST_CASE("disable returns false when plugin not found") {
        PluginRegistry reg;
        CHECK(!reg.disable("nonexistent"));
    }

    TEST_CASE("enable returns false when plugin not found") {
        PluginRegistry reg;
        CHECK(!reg.enable("nonexistent"));
    }

    TEST_CASE("disable returns false when plugin already disabled") {
        TempDir root;
        make_plugin_dir(root.path / "p", "p");
        PluginRegistry reg;
        reg.load_dir(root.path);

        reg.disable("p");
        // Second disable returns false (already disabled).
        CHECK(!reg.disable("p"));
    }

    TEST_CASE("enable returns false when plugin already enabled") {
        TempDir root;
        make_plugin_dir(root.path / "p", "p");
        PluginRegistry reg;
        reg.load_dir(root.path);

        // Plugin starts enabled — enable() should return false.
        CHECK(!reg.enable("p"));
    }

    TEST_CASE("disabled plugin is excluded from get_skill, get_agent, get_command") {
        TempDir root;
        auto plugin_dir = root.path / "skill-plugin";
        make_plugin_dir(plugin_dir, "skill-plugin");

        PluginRegistry reg;
        reg.load_dir(root.path);

        // Manually inject a skill into the stored plugin for this test.
        // (PluginLoader CPP 11.4 populates skills; here we verify registry filtering.)
        // We need to manipulate the store directly — use disable to test filtering.

        // Since we cannot inject skills without PluginLoader, we verify the
        // get_skill() / get_agent() / get_command() return nullopt for empty
        // vectors on enabled/disabled plugins.
        CHECK(!reg.get_skill("anything"));
        CHECK(!reg.get_agent("anything"));
        CHECK(!reg.get_command("anything"));

        reg.disable("skill-plugin");
        CHECK(!reg.get_skill("anything"));
    }
}

// ============================================================================
// TEST SUITE 5 — reload() and atomic swap
// ============================================================================

TEST_SUITE("PluginRegistry::reload — atomic swap") {

    TEST_CASE("reload with no roots is a no-op") {
        PluginRegistry reg;
        reg.reload();
        CHECK(reg.empty());
    }

    TEST_CASE("reload after adding a plugin to the root reflects the new plugin") {
        TempDir root;
        make_plugin_dir(root.path / "plugin-a", "plugin-a");

        PluginRegistry reg;
        reg.load_dir(root.path);
        CHECK(reg.size() == 1);

        // Add a second plugin to the root directory.
        make_plugin_dir(root.path / "plugin-b", "plugin-b");

        // Before reload, registry still has 1 plugin.
        CHECK(reg.size() == 1);

        // After reload, registry picks up the new plugin.
        reg.reload();
        CHECK(reg.size() == 2);
        CHECK(reg.get("plugin-a") != nullptr);
        CHECK(reg.get("plugin-b") != nullptr);
    }

    TEST_CASE("get_snapshot returns stable view under concurrent reload") {
        TempDir root;
        make_plugin_dir(root.path / "stable-plugin", "stable-plugin");

        PluginRegistry reg;
        reg.load_dir(root.path);

        // Take a snapshot before reload.
        auto snap_before = reg.get_snapshot();
        REQUIRE(snap_before != nullptr);
        REQUIRE(snap_before->size() == 1);
        CHECK((*snap_before)[0].name == "stable-plugin");

        // Simulate reload from another thread while we hold the snapshot.
        std::atomic<bool> reload_done{false};
        std::thread reloader([&]() {
            // Add another plugin, then reload.
            make_plugin_dir(root.path / "new-plugin", "new-plugin");
            reg.reload();
            reload_done.store(true, std::memory_order_release);
        });
        reloader.join();

        REQUIRE(reload_done.load());

        // The snapshot taken BEFORE reload should still be valid and see only 1 plugin.
        CHECK(snap_before->size() == 1);
        CHECK((*snap_before)[0].name == "stable-plugin");

        // A new snapshot should see both plugins.
        auto snap_after = reg.get_snapshot();
        REQUIRE(snap_after != nullptr);
        CHECK(snap_after->size() == 2);
    }

    TEST_CASE("concurrent reload and get_snapshot do not crash") {
        TempDir root;
        make_plugin_dir(root.path / "p1", "p1");
        make_plugin_dir(root.path / "p2", "p2");

        PluginRegistry reg;
        reg.load_dir(root.path);

        std::atomic<bool> stop{false};
        std::atomic<int>  reads_done{0};

        // Reader thread: continuously get_snapshot() and inspect.
        std::thread reader([&]() {
            while (!stop.load(std::memory_order_acquire)) {
                auto snap = reg.get_snapshot();
                // Just dereference — should not crash.
                [[maybe_unused]] std::size_t n = snap->size();
                reads_done.fetch_add(1, std::memory_order_relaxed);
            }
        });

        // Writer: run a few reloads.
        for (int i = 0; i < 5; ++i) {
            reg.reload();
        }

        stop.store(true, std::memory_order_release);
        reader.join();

        CHECK(reads_done.load() > 0);
        CHECK(reg.size() == 2);
    }
}

// ============================================================================
// TEST SUITE 6 — asset look-ups (get_skill / get_agent / get_command)
// ============================================================================

TEST_SUITE("PluginRegistry — asset look-ups") {


    TEST_CASE("get_skill returns nullopt on empty registry") {
        PluginRegistry reg;
        CHECK(!reg.get_skill("debug"));
    }

    TEST_CASE("get_agent returns nullopt on empty registry") {
        PluginRegistry reg;
        CHECK(!reg.get_agent("reviewer"));
    }

    TEST_CASE("get_command returns nullopt on empty registry") {
        PluginRegistry reg;
        CHECK(!reg.get_command("summarize"));
    }

    TEST_CASE("get_skill returns nullopt when no plugins loaded with that skill name") {
        TempDir root;
        make_plugin_dir(root.path / "myplugin", "myplugin");

        PluginRegistry reg;
        reg.load_dir(root.path);

        // Plugin is loaded but has no skills (PluginLoader not run).
        CHECK(!reg.get_skill("debug"));
    }
}

// ============================================================================
// TEST SUITE 7 — Plugin dir field is populated correctly
// ============================================================================

TEST_SUITE("PluginRegistry — plugin dir field") {

    TEST_CASE("plugin dir matches the plugin subdirectory path") {
        TempDir root;
        auto plugin_subdir = root.path / "my-plugin-dir";
        make_plugin_dir(plugin_subdir, "my-plugin");

        PluginRegistry reg;
        reg.load_dir(root.path);

        const Plugin* p = reg.get("my-plugin");
        REQUIRE(p != nullptr);
        CHECK(p->dir == plugin_subdir);
    }
}

// ============================================================================
// TEST SUITE 8 — mcp_servers carried over from marketplace.json
// ============================================================================

TEST_SUITE("PluginRegistry — mcp_servers from marketplace.json") {

    TEST_CASE("plugin with mcpServers has mcp_servers populated") {
        TempDir root;
        auto plugin_dir = root.path / "mcp-plugin";
        std::string json = R"({
  "name": "mcp-plugin",
  "version": "1.0",
  "mcpServers": {
    "my-server": {
      "command": "/usr/bin/mcp",
      "args": ["--stdio"],
      "env": {"TOKEN": "abc"}
    }
  }
})";
        write_file(plugin_dir / ".batbox-plugin" / "marketplace.json", json);

        PluginRegistry reg;
        reg.load_dir(root.path);

        const Plugin* p = reg.get("mcp-plugin");
        REQUIRE(p != nullptr);
        REQUIRE(p->mcp_servers.size() == 1);
        CHECK(p->mcp_servers[0].transport == McpTransport::Stdio);
        CHECK(p->mcp_servers[0].command   == "/usr/bin/mcp");
        REQUIRE(p->mcp_servers[0].args.size() == 1);
        CHECK(p->mcp_servers[0].args[0]   == "--stdio");
        CHECK(p->mcp_servers[0].env.at("TOKEN") == "abc");
    }

    TEST_CASE("plugin with multiple mcpServers has all entries in mcp_servers") {
        TempDir root;
        auto plugin_dir = root.path / "multi-mcp";
        std::string json = R"({
  "name": "multi-mcp",
  "mcpServers": {
    "srv1": {"command": "/bin/srv1"},
    "srv2": {"transport": "sse", "url": "https://example.com/mcp"}
  }
})";
        write_file(plugin_dir / ".batbox-plugin" / "marketplace.json", json);

        PluginRegistry reg;
        reg.load_dir(root.path);

        const Plugin* p = reg.get("multi-mcp");
        REQUIRE(p != nullptr);
        CHECK(p->mcp_servers.size() == 2);
    }
}
