// src/plugins/PluginRegistry.cpp
// =============================================================================
// Implementation of batbox::plugins::PluginRegistry.
//
// Thread-safety contract:
//   - mutex_ guards both store_ and roots_.
//   - get_snapshot() returns a copy of the shared_ptr (under lock), giving the
//     caller an immutable, lock-free view after the call returns.
//   - load_dir() appends to roots_, builds a fresh store via scan_roots(), and
//     atomically replaces store_ under the mutex.
//   - reload() calls scan_roots() and atomically replaces store_ — same pattern.
// =============================================================================

#include <batbox/plugins/PluginRegistry.hpp>
#include <batbox/plugins/MarketplaceJson.hpp>
#include <batbox/core/Logging.hpp>

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace batbox::plugins {

// ============================================================================
// Internal helpers (file-scope anonymous namespace)
// ============================================================================

namespace {

/// Build a Plugin from a parsed Marketplace + the plugin's root directory.
///
/// This is a lightweight struct-fill: PluginLoader (CPP 11.4) will later walk
/// the skills/, agents/, commands/ subdirectories and populate those vectors.
/// Here we only carry over what marketplace.json gives us:
///   name, version, description, mcp_servers.
/// author is left empty (not in current marketplace.json schema).
/// disabled defaults to false.
Plugin plugin_from_marketplace(const Marketplace& m, const fs::path& plugin_dir) {
    Plugin p;
    p.name        = m.name;
    p.version     = m.version;
    p.description = m.description;
    p.dir         = plugin_dir;
    p.disabled    = false;

    // Flatten unordered_map<string, McpServerSpec> → vector<McpServerConfig>.
    // We preserve each entry but discard the map key (server name); callers
    // that need the server name should use the Marketplace struct directly.
    // McpServerConfig is a type alias for McpServerSpec so no conversion needed.
    p.mcp_servers.reserve(m.mcp_servers.size());
    for (const auto& [/*name*/ _, spec] : m.mcp_servers) {
        p.mcp_servers.push_back(spec);
    }

    return p;
}

/// Try to load one plugin from `candidate_dir`.
///
/// Looks for .claude-plugin/marketplace.json or .batbox-plugin/marketplace.json
/// (via find_marketplace_in_dir).  Returns nullopt when no manifest is found or
/// parsing fails (errors are logged as warnings so the caller can continue).
std::optional<Plugin> try_load_plugin(const fs::path& candidate_dir) {
    auto manifest_path = find_marketplace_in_dir(candidate_dir);
    if (!manifest_path) {
        // Not a plugin directory — silently skip.
        return std::nullopt;
    }

    auto result = parse_marketplace_json(*manifest_path);
    if (!result) {
        BATBOX_LOG_WARN("plugins: skipping {}: {}", candidate_dir.string(), result.error());
        return std::nullopt;
    }

    return plugin_from_marketplace(result.value(), candidate_dir);
}

} // namespace (anonymous)

// ============================================================================
// PluginRegistry — public implementation
// ============================================================================

// ----------------------------------------------------------------------------
// load_dir
// ----------------------------------------------------------------------------

void PluginRegistry::load_dir(const fs::path& dir) {
    std::error_code ec;

    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) {
        BATBOX_LOG_WARN("plugins: load_dir: {} does not exist or is not a directory — skipping",
                        dir.string());
        return;
    }

    {
        std::unique_lock lock(mutex_);

        // Register root if not already present (deduplicate).
        if (std::find(roots_.begin(), roots_.end(), dir) == roots_.end()) {
            roots_.push_back(dir);
        }

        // Rebuild the full store from all roots (keeps last-root-wins overlay
        // semantics consistent with a full reload).
        auto new_store = scan_roots();  // calls back into roots_ — mutex already held
        store_ = std::make_shared<std::vector<Plugin>>(std::move(new_store));
    }
}

// ----------------------------------------------------------------------------
// reload
// ----------------------------------------------------------------------------

void PluginRegistry::reload() {
    std::unique_lock lock(mutex_);
    auto new_store = scan_roots();
    store_ = std::make_shared<std::vector<Plugin>>(std::move(new_store));
    BATBOX_LOG_INFO("plugins: reloaded; {} plugin(s) loaded", store_->size());
}

// ----------------------------------------------------------------------------
// get_snapshot
// ----------------------------------------------------------------------------

std::shared_ptr<const std::vector<Plugin>> PluginRegistry::get_snapshot() const {
    std::unique_lock lock(mutex_);
    // Return a copy of the shared_ptr (bump ref count under lock).
    return store_;
}

// ----------------------------------------------------------------------------
// all_plugins
// ----------------------------------------------------------------------------

const std::vector<Plugin>& PluginRegistry::all_plugins() const {
    // NOTE: Caller is responsible for ensuring no concurrent reload() when
    // using the returned reference.  For multi-threaded access prefer
    // get_snapshot().
    std::unique_lock lock(mutex_);
    return *store_;
}

// ----------------------------------------------------------------------------
// active_plugins
// ----------------------------------------------------------------------------

std::vector<Plugin> PluginRegistry::active_plugins() const {
    auto snap = get_snapshot();  // stable snapshot
    std::vector<Plugin> active;
    active.reserve(snap->size());
    for (const auto& p : *snap) {
        if (!p.disabled) {
            active.push_back(p);
        }
    }
    return active;
}

// ----------------------------------------------------------------------------
// get
// ----------------------------------------------------------------------------

const Plugin* PluginRegistry::get(std::string_view name) const {
    std::unique_lock lock(mutex_);
    for (const auto& p : *store_) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

// ----------------------------------------------------------------------------
// enable / disable
// ----------------------------------------------------------------------------

bool PluginRegistry::enable(std::string_view name) {
    std::unique_lock lock(mutex_);
    // We need a mutable copy of the vector to modify it, then swap.
    auto new_store = *store_;  // copy
    for (auto& p : new_store) {
        if (p.name == name) {
            if (!p.disabled) return false;  // already enabled
            p.disabled = false;
            store_ = std::make_shared<std::vector<Plugin>>(std::move(new_store));
            BATBOX_LOG_INFO("plugins: enabled '{}'", std::string(name));
            return true;
        }
    }
    BATBOX_LOG_WARN("plugins: enable: plugin '{}' not found", std::string(name));
    return false;
}

bool PluginRegistry::disable(std::string_view name) {
    std::unique_lock lock(mutex_);
    auto new_store = *store_;  // copy
    for (auto& p : new_store) {
        if (p.name == name) {
            if (p.disabled) return false;  // already disabled
            p.disabled = true;
            store_ = std::make_shared<std::vector<Plugin>>(std::move(new_store));
            BATBOX_LOG_INFO("plugins: disabled '{}'", std::string(name));
            return true;
        }
    }
    BATBOX_LOG_WARN("plugins: disable: plugin '{}' not found", std::string(name));
    return false;
}

// ----------------------------------------------------------------------------
// get_skill / get_agent / get_command
// ----------------------------------------------------------------------------

std::optional<Skill> PluginRegistry::get_skill(std::string_view name) const {
    auto snap = get_snapshot();
    for (const auto& p : *snap) {
        if (p.disabled) continue;
        for (const auto& s : p.skills) {
            if (s.name == name) return s;
        }
    }
    return std::nullopt;
}

std::optional<Agent> PluginRegistry::get_agent(std::string_view name) const {
    auto snap = get_snapshot();
    for (const auto& p : *snap) {
        if (p.disabled) continue;
        for (const auto& a : p.agents) {
            if (a.name == name) return a;
        }
    }
    return std::nullopt;
}

std::optional<Command> PluginRegistry::get_command(std::string_view name) const {
    auto snap = get_snapshot();
    for (const auto& p : *snap) {
        if (p.disabled) continue;
        for (const auto& c : p.commands) {
            if (c.name == name) return c;
        }
    }
    return std::nullopt;
}

// ----------------------------------------------------------------------------
// size / empty
// ----------------------------------------------------------------------------

std::size_t PluginRegistry::size() const {
    std::unique_lock lock(mutex_);
    return store_->size();
}

bool PluginRegistry::empty() const {
    std::unique_lock lock(mutex_);
    return store_->empty();
}

// ============================================================================
// PluginRegistry — private helpers
// ============================================================================

// ----------------------------------------------------------------------------
// scan_roots — must be called with mutex_ already held
// ----------------------------------------------------------------------------

std::vector<Plugin> PluginRegistry::scan_roots() const {
    // unordered_map<name, Plugin>: last root wins (overlay semantics).
    // Walking roots in order means later roots overwrite earlier ones for
    // the same plugin name — matching claude-code's overlay precedence rules.
    std::unordered_map<std::string, Plugin> by_name;

    for (const auto& root : roots_) {
        std::error_code ec;
        if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) continue;

        for (const auto& entry : fs::directory_iterator(root, ec)) {
            if (ec) { ec.clear(); continue; }
            if (!entry.is_directory()) continue;

            if (auto plugin = try_load_plugin(entry.path())) {
                by_name.insert_or_assign(plugin->name, std::move(*plugin));
            }
        }
    }

    // Collect all values into a result vector.
    std::vector<Plugin> result;
    result.reserve(by_name.size());
    for (auto& [name, plugin] : by_name) {
        (void)name;
        result.push_back(std::move(plugin));
    }

    return result;
}

} // namespace batbox::plugins
