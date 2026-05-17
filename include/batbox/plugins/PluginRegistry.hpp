// include/batbox/plugins/PluginRegistry.hpp
// =============================================================================
// batbox::plugins::PluginRegistry — merged in-memory view of all loaded plugins.
//
// Design (per ned-cpp.md §2.C11):
//   - Holds a vector<Plugin> as the authoritative store.
//   - Thread-safe reload via atomic swap: callers read through a shared_ptr
//     snapshot so an in-flight reload never produces a torn read.
//   - disabled flag is honoured: active_plugins() filters disabled entries.
//   - Look-up helpers find skills, agents, and commands by name across all
//     active plugins plus standalone user-dir entries.
//
// Threading model:
//   - get_snapshot() returns a std::shared_ptr<const std::vector<Plugin>>;
//     the snapshot is stable for the lifetime of the shared_ptr regardless of
//     concurrent reloads.
//   - reload() constructs a fresh vector, then swaps the shared_ptr under a
//     std::mutex.  Readers that already hold a snapshot see the old data;
//     new readers see the new data.  No reader ever blocks a writer beyond
//     the duration of a single pointer swap.
//
// /plugin command surface (implemented in PluginRegistry.cpp):
//   load_dir(dir)       — scan one root directory, add discovered plugins
//   reload()            — rescan all previously-registered roots, atomic swap
//   all_plugins()       — const ref to full list (includes disabled)
//   active_plugins()    — copy filtered to enabled only
//   get(name)           — find by name; nullopt when not found
//   enable(name)        — clear disabled flag (does NOT persist to settings.json)
//   disable(name)       — set disabled flag  (does NOT persist to settings.json)
//   get_skill(name)     — first matching Skill across active plugins
//   get_agent(name)     — first matching Agent across active plugins
//   get_command(name)   — first matching Command across active plugins
//
// Persistence of enable/disable state to settings.json is the responsibility
// of the caller (PluginLoader or the /plugin command handler).
//
// Build (standalone — from repo root):
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

#pragma once

#include <batbox/plugins/Plugin.hpp>

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace batbox::plugins {

// ============================================================================
// PluginRegistry
// ============================================================================

/// Thread-safe in-memory registry of all loaded plugins.
///
/// Intended usage pattern:
///
///   PluginRegistry registry;
///   registry.load_dir(fs::path("~/.batbox/plugins"));
///   registry.load_dir(fs::path("./.batbox/plugins"));
///
///   // Fast lookup:
///   if (auto skill = registry.get_skill("debug")) {
///       // use *skill
///   }
///
///   // Atomic reload on /plugin reload command:
///   registry.reload();
class PluginRegistry {
public:
    PluginRegistry() = default;
    ~PluginRegistry() = default;

    // Non-copyable, non-movable (holds a std::mutex).
    PluginRegistry(const PluginRegistry&)            = delete;
    PluginRegistry& operator=(const PluginRegistry&) = delete;
    PluginRegistry(PluginRegistry&&)                 = delete;
    PluginRegistry& operator=(PluginRegistry&&)      = delete;

    // -------------------------------------------------------------------------
    // Loading
    // -------------------------------------------------------------------------

    /// Scan `dir` for plugin subdirectories and append any valid plugins found
    /// to the current store.  Each immediate child of `dir` that contains a
    /// .claude-plugin/marketplace.json or .batbox-plugin/marketplace.json is
    /// treated as a plugin root.
    ///
    /// Plugins with the same name as an already-loaded plugin replace it
    /// (last-root-wins overlay semantics, matching claude-code behaviour).
    ///
    /// `dir` is also registered as a "scan root" so that reload() can
    /// re-enumerate it.
    ///
    /// Does nothing (logs a warning) when `dir` does not exist or is not
    /// a directory.
    void load_dir(const fs::path& dir);

    /// Re-scan all roots registered via previous load_dir() calls and
    /// atomically replace the current plugin store with the new results.
    ///
    /// Thread-safe: callers holding a get_snapshot() shared_ptr continue to
    /// see the old data; new calls to get_snapshot() see the refreshed data.
    void reload();

    // -------------------------------------------------------------------------
    // Querying
    // -------------------------------------------------------------------------

    /// Return a stable snapshot of the full plugin list (including disabled).
    ///
    /// The returned shared_ptr keeps the vector alive for as long as the caller
    /// holds it, even if reload() runs concurrently.
    [[nodiscard]] std::shared_ptr<const std::vector<Plugin>> get_snapshot() const;

    /// Return a const reference to the current full plugin list.
    ///
    /// NOT reload-safe if called across a reload() boundary from a different
    /// thread.  Prefer get_snapshot() in multi-threaded contexts.
    [[nodiscard]] const std::vector<Plugin>& all_plugins() const;

    /// Return a copy of the plugin list filtered to enabled-only entries.
    [[nodiscard]] std::vector<Plugin> active_plugins() const;

    /// Find a plugin by name.  Searches the full list (including disabled).
    ///
    /// @return pointer into the internal vector; valid until the next
    ///         mutating call (load_dir, reload, enable, disable).  Returns
    ///         nullptr when not found.
    [[nodiscard]] const Plugin* get(std::string_view name) const;

    // -------------------------------------------------------------------------
    // Enable / Disable
    // -------------------------------------------------------------------------

    /// Clear the disabled flag for the plugin named `name`.
    ///
    /// @return true if the plugin was found and its state changed,
    ///         false when not found or already enabled.
    bool enable(std::string_view name);

    /// Set the disabled flag for the plugin named `name`.
    ///
    /// @return true if the plugin was found and its state changed,
    ///         false when not found or already disabled.
    bool disable(std::string_view name);

    // -------------------------------------------------------------------------
    // Asset look-ups (across active plugins only)
    // -------------------------------------------------------------------------

    /// Find the first Skill with the given name across all active plugins.
    /// Returns nullopt when not found.
    [[nodiscard]] std::optional<Skill> get_skill(std::string_view name) const;

    /// Find the first Agent with the given name across all active plugins.
    /// Returns nullopt when not found.
    [[nodiscard]] std::optional<Agent> get_agent(std::string_view name) const;

    /// Find the first Command with the given name across all active plugins.
    /// Returns nullopt when not found.
    [[nodiscard]] std::optional<Command> get_command(std::string_view name) const;

    // -------------------------------------------------------------------------
    // Capacity / diagnostics
    // -------------------------------------------------------------------------

    /// Total number of loaded plugins (enabled + disabled).
    [[nodiscard]] std::size_t size() const;

    /// True when no plugins are loaded.
    [[nodiscard]] bool empty() const;

private:
    // ---- Internal helpers ---------------------------------------------------

    /// Build a fresh plugin list by scanning all registered roots.
    /// Called by reload() and (internally) after each load_dir().
    std::vector<Plugin> scan_roots() const;

    // ---- State --------------------------------------------------------------

    mutable std::mutex                             mutex_;

    /// Authoritative snapshot.  Always non-null after construction.
    std::shared_ptr<std::vector<Plugin>>           store_{
        std::make_shared<std::vector<Plugin>>()
    };

    /// Directories registered via load_dir(), in insertion order.
    std::vector<fs::path>                          roots_;
};

} // namespace batbox::plugins
