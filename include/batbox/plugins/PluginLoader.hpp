// include/batbox/plugins/PluginLoader.hpp
// =============================================================================
// batbox::plugins::PluginLoader — top-level scanner that discovers plugins
// under the four standard roots, loads their assets via SkillLoader,
// AgentLoader, and CommandLoader, and populates a PluginRegistry.
//
// Scan roots (in ascending priority order — later overlays earlier):
//   1. ~/.claude/plugins/*/        (claude-code compat, read-only)
//   2. ./.claude/plugins/*/        (project-level compat, read-only)
//   3. ~/.batbox/plugins/*/        (user global)
//   4. ./.batbox/plugins/*/        (project-local, highest priority)
//
// Each root is walked for immediate subdirectories that contain either:
//   .claude-plugin/marketplace.json   (claude-code compat layout)
//   .batbox-plugin/marketplace.json   (batbox-native layout)
//
// Per-plugin assets enumerated from the same subdirectory layouts:
//   .claude-plugin/skills/*.md   OR  .batbox-plugin/skills/*.md
//   .claude-plugin/agents/*.md   OR  .batbox-plugin/agents/*.md
//   .claude-plugin/commands/*.md OR  .batbox-plugin/commands/*.md
//
// Disabled plugins:
//   Plugins whose name appears in settings.json plugins.disabled are loaded
//   into the registry with Plugin::disabled == true.  They are present in
//   all_plugins() but filtered from active_plugins() and all asset look-ups.
//
// Atomicity:
//   reload(registry) builds a fresh PluginRegistry and atomically swaps it
//   into the provided registry via PluginRegistry::reload() — no half-state
//   is ever visible to concurrent readers.
//
// Build (standalone — from repo root):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/integration/test_plugin_loader.cpp \
//       src/plugins/FrontmatterParser.cpp \
//       src/plugins/MarketplaceJson.cpp \
//       src/plugins/SkillLoader.cpp \
//       src/plugins/AgentLoader.cpp \
//       src/plugins/CommandLoader.cpp \
//       src/plugins/PluginRegistry.cpp \
//       src/plugins/PluginLoader.cpp \
//       src/core/Json.cpp src/core/Logging.cpp src/core/Paths.cpp \
//       src/commands/SlashCommandRegistry.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       build/vcpkg_installed/arm64-osx/lib/libspdlog.a \
//       build/vcpkg_installed/arm64-osx/lib/libfmt.a \
//       -o /tmp/test_plugin_loader && /tmp/test_plugin_loader
// =============================================================================

#pragma once

#include <batbox/core/Result.hpp>
#include <batbox/plugins/Plugin.hpp>
#include <batbox/plugins/PluginRegistry.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace batbox::plugins {

// =============================================================================
// PluginLoader
// =============================================================================

/// Scans the four standard plugin roots and populates a PluginRegistry.
///
/// Typical lifecycle:
///   1. Construct a PluginLoader (optionally with a custom settings path).
///   2. Call load_all() to scan all roots and return the loaded plugins.
///      OR supply a registry and call reload(registry) to perform an atomic swap.
///   3. For interactive /plugin operations call add_local() or remove().
///
/// Thread safety:
///   PluginLoader itself is not thread-safe.  All public methods must be called
///   from a single thread.  The PluginRegistry argument to reload() is
///   thread-safe (atomic swap) so concurrent readers are never blocked.
class PluginLoader {
public:
    /// Construct a PluginLoader that reads disabled-plugin state from
    /// `settings_path` (defaults to ~/.batbox/settings.json).
    explicit PluginLoader(
        fs::path settings_path = {});

    ~PluginLoader() = default;

    // Non-copyable, movable.
    PluginLoader(const PluginLoader&)            = delete;
    PluginLoader& operator=(const PluginLoader&) = delete;
    PluginLoader(PluginLoader&&)                 = default;
    PluginLoader& operator=(PluginLoader&&)      = default;

    // -------------------------------------------------------------------------
    // Core loading API
    // -------------------------------------------------------------------------

    /// Scan all 4 roots in priority order, build and return the complete
    /// merged list of Plugin objects.
    ///
    /// - Disabled plugins (per settings.json plugins.disabled) have
    ///   Plugin::disabled == true; their assets are still loaded.
    /// - Malformed plugin directories (missing marketplace.json, parse errors)
    ///   are logged as warnings and skipped.
    /// - Returns an empty vector (not an error) when no roots exist.
    ///
    /// @return Ok(vector<Plugin>) with all discovered plugins in merge order.
    [[nodiscard]] Result<std::vector<Plugin>> load_all();

    /// Rescan all 4 roots and atomically swap the result into `registry`.
    ///
    /// After this call completes, `registry` holds the freshly-scanned set.
    /// Concurrent readers that already hold a get_snapshot() from the old
    /// registry continue to see the old data; new readers see the new data.
    ///
    /// @param registry  The live PluginRegistry to update atomically.
    /// @return          Ok on success; Err with diagnostic on fatal I/O error.
    Result<void> reload(PluginRegistry& registry);

    // -------------------------------------------------------------------------
    // /plugin add / remove helpers
    // -------------------------------------------------------------------------

    /// Copy a local plugin directory tree into ~/.batbox/plugins/ and reload.
    ///
    /// Equivalent to `cp -r <source_path> ~/.batbox/plugins/<basename>`.
    /// After the copy completes, calls reload() on the registry so the new
    /// plugin is immediately visible.
    ///
    /// @param source_path  Absolute path to a plugin root directory on disk.
    ///                     Must contain a .claude-plugin/ or .batbox-plugin/
    ///                     subdirectory with a marketplace.json.
    /// @return Ok on success; Err when the source does not exist, is not a
    ///         directory, or the copy/reload fails.
    Result<void> add_local(const fs::path& source_path);

    /// Remove a plugin from ~/.batbox/plugins/ and reload.
    ///
    /// Equivalent to `rm -rf ~/.batbox/plugins/<name>`.
    /// The caller is responsible for obtaining user confirmation before calling
    /// this method — PluginLoader does NOT prompt.
    ///
    /// @param name  The canonical plugin name (from marketplace.json "name").
    /// @return Ok on success; Err when the plugin directory is not found in
    ///         the user plugin root or removal fails.
    Result<void> remove(std::string_view name);

private:
    // -------------------------------------------------------------------------
    // Internal helpers
    // -------------------------------------------------------------------------

    /// Return the path to ~/.batbox/settings.json, falling back to the
    /// value supplied at construction when set.
    [[nodiscard]] fs::path resolved_settings_path() const;

    /// Read the list of disabled plugin names from settings.json.
    /// Returns an empty vector when the file is missing or the key absent.
    [[nodiscard]] std::vector<std::string> load_disabled_names() const;

    /// Build the four standard scan roots in ascending priority order.
    [[nodiscard]] std::vector<fs::path> build_scan_roots() const;

    /// Scan one root directory for immediate plugin subdirectories and
    /// append discovered Plugin objects to `out`.  Plugins in `out` that
    /// share a name with a newly-discovered one are overwritten (last root wins).
    ///
    /// @param root          The root directory to iterate.
    /// @param disabled_set  Set of plugin names that should be marked disabled.
    /// @param by_name       Working map (name → Plugin) for overlay semantics.
    void scan_root(const fs::path& root,
                   const std::vector<std::string>& disabled_names,
                   std::unordered_map<std::string, Plugin>& by_name) const;

    /// Load all assets (skills, agents, commands) from the plugin directory
    /// `plugin_dir` into `plugin`.  The plugin's dir field must already be set.
    void load_assets(Plugin& plugin, const fs::path& plugin_dir) const;

    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------

    /// Path to settings.json; empty → use default (~/.batbox/settings.json).
    fs::path settings_path_;

    /// Cached reference to the registry supplied to reload(); used by
    /// add_local() and remove() after the initial reload call.
    PluginRegistry* registry_ = nullptr;
};

} // namespace batbox::plugins
