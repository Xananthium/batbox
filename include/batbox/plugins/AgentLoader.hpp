// include/batbox/plugins/AgentLoader.hpp
// =============================================================================
// batbox::plugins::AgentLoader — scan plugin agent directories, parse .md
// files into batbox::plugins::Agent structs, deduplicate by name, and expose
// a query API for use by PluginLoader (CPP 11.7) and AgentSupervisor.
//
// Design (per ned-cpp.md §2.C11):
//   Agent definitions inside plugins live under the plugin's assets directory:
//
//     <plugin-dir>/.claude-plugin/agents/*.md   (claude-code compat name)
//     <plugin-dir>/.batbox-plugin/agents/*.md   (batbox-native name)
//
//   Both paths are scanned in the order above for each plugin directory.
//   Files present under EITHER name are loaded; duplicates across the two
//   paths within the same plugin follow last-scanned-wins semantics.
//
//   This class does NOT scan user-home agent directories
//   (that is batbox::agents::AgentLoader, CPP 6.3).
//   It is specifically the plugin-side loader that feeds agents/*.md entries
//   from marketplace plugin bundles into the system.
//
// Agent struct:
//   Declared in batbox::plugins::Plugin (Plugin.hpp); AgentLoader.hpp is
//   self-contained (includes Plugin.hpp) so callers only need one include.
//
// Collision policy:
//   When two plugins expose the same agent name, the later-scanned plugin
//   wins (same last-scan-wins rule used by SkillLoader).
//   Collision resolution with user-dir agents (CPP 6.3) is handled at the
//   AgentSupervisor level: user-dir entries always win.  AgentLoader itself
//   does NOT know about user-dir agents.
//
// Namespace-prefix on conflict:
//   When load_plugin_dir() detects that a name is already registered from a
//   *different* plugin, the incoming agent is stored under both its bare name
//   (overwriting) AND under the namespaced form "<plugin-name>/<agent-name>".
//   This preserves the prior entry under a stable qualified name.
//
// Graceful failure:
//   Malformed frontmatter, missing required "name" key, or unreadable files
//   log a BATBOX_LOG_WARN and are silently skipped.
//
// Build standalone (from repo root):
//   c++ -std=c++20 -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/integration/test_plugin_agent_loader.cpp \
//       src/plugins/FrontmatterParser.cpp \
//       src/plugins/AgentLoader.cpp \
//       src/core/Logging.cpp \
//       src/core/Paths.cpp \
//       src/core/Json.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libfmt.a \
//       build/vcpkg_installed/arm64-osx/lib/libspdlog.a \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       -o /tmp/test_plugin_agent_loader && /tmp/test_plugin_agent_loader
// =============================================================================

#pragma once

#include <batbox/plugins/Plugin.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace batbox::plugins {

// =============================================================================
// AgentLoader
// =============================================================================

/// Scans plugin directories for agent .md files and parses them into
/// batbox::plugins::Agent structs.
///
/// Typical lifecycle (called by PluginLoader, CPP 11.7):
///   1. Construct an AgentLoader.
///   2. Call load_plugin_dir() once per discovered plugin directory.
///   3. Call names() for autocomplete or find() to look up an agent spec.
///   4. Call all() to retrieve the full flat list for AgentSupervisor.
///
/// Thread safety: not thread-safe. All load/query calls must be serialised by
/// the caller.  Once loading is complete, names(), find(), and all() are safe
/// for concurrent read access.
class AgentLoader {
public:
    AgentLoader()  = default;
    ~AgentLoader() = default;

    // Non-copyable; movable.
    AgentLoader(const AgentLoader&)            = delete;
    AgentLoader& operator=(const AgentLoader&) = delete;
    AgentLoader(AgentLoader&&)                 = default;
    AgentLoader& operator=(AgentLoader&&)      = default;

    // -------------------------------------------------------------------------
    // Loading API
    // -------------------------------------------------------------------------

    /// Scan a plugin directory for agents under both dual-name paths:
    ///   <plugin_dir>/.claude-plugin/agents/*.md
    ///   <plugin_dir>/.batbox-plugin/agents/*.md
    ///
    /// Each .md file is parsed via FrontmatterParser.  The agent's source is
    /// set to "plugin:<plugin_name>".
    ///
    /// When an incoming agent name is already registered from a DIFFERENT
    /// plugin, the previous agent is also stored under its namespaced form
    /// "<previous-plugin-name>/<agent-name>" before being overwritten.
    ///
    /// @param plugin_dir   Absolute path to the plugin root directory.
    /// @param plugin_name  Canonical plugin name (from marketplace.json "name").
    void load_plugin_dir(const std::filesystem::path& plugin_dir,
                         std::string_view             plugin_name);

    /// Scan a single agents/ subdirectory directly (called by load_plugin_dir
    /// for each of the two dual-name paths).
    ///
    /// @param agents_dir   Path to the agents/ directory (must exist + be a dir).
    /// @param plugin_name  Plugin name used as the source tag.
    void scan_dir(const std::filesystem::path& agents_dir,
                  std::string_view             plugin_name);

    /// Clear all loaded agents, resetting to the empty state.
    void clear();

    // -------------------------------------------------------------------------
    // Query API
    // -------------------------------------------------------------------------

    /// Return a sorted list of all loaded agent names (bare + namespaced).
    [[nodiscard]] std::vector<std::string> names() const;

    /// Look up an agent by name.  Returns nullptr if not found.
    /// Both bare names (e.g. "researcher") and namespaced names
    /// (e.g. "my-plugin/researcher") are resolvable.
    [[nodiscard]] const Agent* find(std::string_view name) const;

    /// Return a flat vector of all loaded Agent structs (one per entry in the
    /// internal map, including both bare and namespaced forms of ambiguous names).
    [[nodiscard]] std::vector<Agent> all() const;

    /// Return the total number of entries in the internal map.
    [[nodiscard]] std::size_t size() const noexcept { return agents_.size(); }

private:
    // -------------------------------------------------------------------------
    // Internal helpers
    // -------------------------------------------------------------------------

    /// Parse one .md file into an Agent struct.
    /// Returns nullopt on failure; caller should log and skip.
    [[nodiscard]] std::optional<Agent> parse_agent_file(
        const std::filesystem::path& path,
        std::string_view             plugin_name) const;

    /// Insert or replace an Agent in agents_ by name.
    /// If the name already belongs to a different plugin, first saves the
    /// existing entry under "<existing-source-plugin>/<name>" (the namespaced
    /// fallback), then overwrites the bare entry.
    void upsert(Agent agent);

    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------

    /// Master map: agent key (bare or namespaced) → Agent.
    std::unordered_map<std::string, Agent> agents_;
};

} // namespace batbox::plugins
