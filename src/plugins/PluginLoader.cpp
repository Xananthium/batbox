// src/plugins/PluginLoader.cpp
// =============================================================================
// Implementation of batbox::plugins::PluginLoader.
//
// Scan order (ascending priority — later roots overlay earlier ones):
//   1. ~/.claude/plugins/*/
//   2. ./.claude/plugins/*/
//   3. ~/.batbox/plugins/*/
//   4. ./.batbox/plugins/*/
//
// Per-plugin discovery layout (both checked; .batbox-plugin wins same-plugin):
//   <plugin_dir>/.claude-plugin/marketplace.json
//   <plugin_dir>/.batbox-plugin/marketplace.json
//
// Assets loaded per plugin:
//   {.claude-plugin,.batbox-plugin}/skills/*.md    → Plugin::skills
//   {.claude-plugin,.batbox-plugin}/agents/*.md    → Plugin::agents
//   {.claude-plugin,.batbox-plugin}/commands/*.md  → Plugin::commands (via registry stub)
//   mcp_servers from marketplace.json              → Plugin::mcp_servers
//
// Disabled list:
//   settings.json plugins.disabled[] names are applied after loading:
//   matching Plugin::disabled = true.
//
// Atomicity:
//   reload(registry) calls PluginRegistry::load_dir() for each root after
//   clearing previous roots, then triggers PluginRegistry::reload() for the
//   atomic swap.  Alternatively we build the Plugin vector ourselves and
//   populate the registry via load_dir.
//
//   Because PluginRegistry already implements load_dir + reload with atomic
//   swap semantics, PluginLoader acts as the higher-level scanner that feeds
//   it: we scan directories ourselves (to apply disabled flags + load assets)
//   and insert full Plugin objects directly.
// =============================================================================

#include <batbox/plugins/PluginLoader.hpp>

#include <batbox/config/SettingsLoader.hpp>
#include <batbox/core/Logging.hpp>
#include <batbox/core/Paths.hpp>
#include <batbox/plugins/AgentLoader.hpp>
#include <batbox/plugins/FrontmatterParser.hpp>
#include <batbox/plugins/MarketplaceJson.hpp>
#include <batbox/plugins/PluginRegistry.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace batbox::plugins {

// ============================================================================
// Internal helpers (anonymous namespace)
// ============================================================================

namespace {

// ---------------------------------------------------------------------------
// read_file_to_string — slurp a file into std::string.
// ---------------------------------------------------------------------------
[[nodiscard]] std::string read_file_to_string(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ---------------------------------------------------------------------------
// parse_skill_md — parse a single skills/*.md file into a Skill.
// Returns nullopt on any parse failure (caller logs and skips).
// ---------------------------------------------------------------------------
[[nodiscard]] std::optional<Skill> parse_skill_md(
        const fs::path& path,
        std::string_view plugin_name)
{
    const std::string content = read_file_to_string(path);
    if (content.empty() && !fs::exists(path)) {
        BATBOX_LOG_WARN("PluginLoader: cannot read skill file: {}", path.string());
        return std::nullopt;
    }

    auto result = parse_frontmatter(content);
    if (!result) {
        BATBOX_LOG_WARN("PluginLoader: malformed frontmatter in skill '{}': {}",
                        path.string(), result.error());
        return std::nullopt;
    }

    const Frontmatter& meta = result->first;
    const std::string& body = result->second;

    // name: mandatory; fall back to filename stem.
    std::string name;
    if (auto it = meta.find("name"); it != meta.end() && it->second.is_string()) {
        name = it->second.get<std::string>();
    }
    if (name.empty()) {
        name = path.stem().string();
    }
    if (name.empty()) {
        BATBOX_LOG_WARN("PluginLoader: skill '{}' has no name and no stem — skipped",
                        path.string());
        return std::nullopt;
    }

    Skill s;
    s.name        = std::move(name);
    s.prompt_body = body;
    s.source      = "plugin:" + std::string(plugin_name);

    if (auto it = meta.find("description"); it != meta.end() && it->second.is_string()) {
        s.description = it->second.get<std::string>();
    }
    if (auto it = meta.find("model"); it != meta.end() && it->second.is_string()) {
        s.model = it->second.get<std::string>();
    }
    if (auto it = meta.find("allowed_tools"); it != meta.end()) {
        const auto& v = it->second;
        if (v.is_array()) {
            for (const auto& item : v) {
                if (item.is_string()) s.allowed_tools.push_back(item.get<std::string>());
            }
        } else if (v.is_string()) {
            s.allowed_tools.push_back(v.get<std::string>());
        }
    }

    // Optional companion script.sh alongside the .md file.
    fs::path script = path.parent_path() / "script.sh";
    if (fs::exists(script)) {
        s.script_path = std::move(script);
    }

    return s;
}

// ---------------------------------------------------------------------------
// scan_skills_from_dir — scan a skills/ directory, parse each .md, return
// the Skill vector.
// ---------------------------------------------------------------------------
[[nodiscard]] std::vector<Skill> scan_skills_from_dir(
        const fs::path& skills_dir,
        std::string_view plugin_name)
{
    std::vector<Skill> out;
    if (!fs::exists(skills_dir) || !fs::is_directory(skills_dir)) return out;

    std::error_code ec;
    for (const fs::directory_entry& entry : fs::directory_iterator(skills_dir, ec)) {
        if (ec) {
            BATBOX_LOG_WARN("PluginLoader: error iterating skills dir '{}': {}",
                            skills_dir.string(), ec.message());
            break;
        }
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".md") continue;

        auto skill = parse_skill_md(entry.path(), plugin_name);
        if (skill) {
            out.push_back(std::move(*skill));
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// merge_skills — upsert skills by name (last wins).
// ---------------------------------------------------------------------------
void merge_skills(std::unordered_map<std::string, Skill>& map,
                  std::vector<Skill> incoming)
{
    for (auto& s : incoming) {
        map[s.name] = std::move(s);
    }
}

// ---------------------------------------------------------------------------
// load_skills_for_plugin — probe both dual-name subdirs, merge, return vector.
// ---------------------------------------------------------------------------
[[nodiscard]] std::vector<Skill> load_skills_for_plugin(
        const fs::path& plugin_dir,
        std::string_view plugin_name)
{
    std::unordered_map<std::string, Skill> by_name;

    const std::array<fs::path, 2> subdirs = {
        plugin_dir / ".claude-plugin" / "skills",
        plugin_dir / ".batbox-plugin" / "skills",
    };

    for (const fs::path& d : subdirs) {
        merge_skills(by_name, scan_skills_from_dir(d, plugin_name));
    }

    std::vector<Skill> result;
    result.reserve(by_name.size());
    for (auto& [_, s] : by_name) {
        result.push_back(std::move(s));
    }
    return result;
}

// ---------------------------------------------------------------------------
// load_agents_for_plugin — delegate to AgentLoader for dual-path scan.
// ---------------------------------------------------------------------------
[[nodiscard]] std::vector<Agent> load_agents_for_plugin(
        const fs::path& plugin_dir,
        std::string_view plugin_name)
{
    AgentLoader loader;
    loader.load_plugin_dir(plugin_dir, plugin_name);
    return loader.all();
}

// ---------------------------------------------------------------------------
// scan_commands_from_dir — parse commands/*.md files into Command structs.
// (CommandLoader registers into a SlashCommandRegistry; here we just build
//  Command structs for Plugin::commands so the Plugin is self-contained.)
// ---------------------------------------------------------------------------
[[nodiscard]] std::optional<Command> parse_command_md(
        const fs::path& path,
        std::string_view plugin_name)
{
    const std::string content = read_file_to_string(path);
    if (content.empty() && !fs::exists(path)) {
        BATBOX_LOG_WARN("PluginLoader: cannot read command file: {}", path.string());
        return std::nullopt;
    }

    auto result = parse_frontmatter(content);
    if (!result) {
        BATBOX_LOG_WARN("PluginLoader: malformed frontmatter in command '{}': {}",
                        path.string(), result.error());
        return std::nullopt;
    }

    const Frontmatter& meta = result->first;
    const std::string& body = result->second;

    std::string name;
    if (auto it = meta.find("name"); it != meta.end() && it->second.is_string()) {
        name = it->second.get<std::string>();
    }
    if (name.empty()) {
        name = path.stem().string();
    }
    if (name.empty()) {
        BATBOX_LOG_WARN("PluginLoader: command '{}' has no name — skipped", path.string());
        return std::nullopt;
    }

    // Strip accidental leading slash.
    if (!name.empty() && name.front() == '/') name.erase(name.begin());

    Command cmd;
    cmd.name   = std::move(name);
    cmd.body   = body;
    cmd.source = "plugin:" + std::string(plugin_name);
    if (auto it = meta.find("description"); it != meta.end() && it->second.is_string()) {
        cmd.description = it->second.get<std::string>();
    }
    return cmd;
}

[[nodiscard]] std::vector<Command> load_commands_for_plugin(
        const fs::path& plugin_dir,
        std::string_view plugin_name)
{
    std::unordered_map<std::string, Command> by_name;

    const std::array<fs::path, 2> subdirs = {
        plugin_dir / ".claude-plugin" / "commands",
        plugin_dir / ".batbox-plugin" / "commands",
    };

    for (const fs::path& d : subdirs) {
        if (!fs::exists(d) || !fs::is_directory(d)) continue;
        std::error_code ec;
        for (const fs::directory_entry& entry : fs::directory_iterator(d, ec)) {
            if (ec) { ec.clear(); continue; }
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".md") continue;
            auto cmd = parse_command_md(entry.path(), plugin_name);
            if (cmd) {
                by_name[cmd->name] = std::move(*cmd);
            }
        }
    }

    std::vector<Command> result;
    result.reserve(by_name.size());
    for (auto& [_, c] : by_name) {
        result.push_back(std::move(c));
    }
    return result;
}

// ---------------------------------------------------------------------------
// try_load_one_plugin — attempt to load a single plugin from candidate_dir.
// Returns nullopt when no marketplace.json is found or parsing fails.
// Applies disabled flag if name appears in disabled_names.
// ---------------------------------------------------------------------------
[[nodiscard]] std::optional<Plugin> try_load_one_plugin(
        const fs::path& candidate_dir,
        const std::vector<std::string>& disabled_names)
{
    // Find the manifest.
    auto manifest_path = find_marketplace_in_dir(candidate_dir);
    if (!manifest_path) {
        // Not a plugin directory — silently skip.
        return std::nullopt;
    }

    auto market_result = parse_marketplace_json(*manifest_path);
    if (!market_result) {
        BATBOX_LOG_WARN("PluginLoader: skipping '{}': {}",
                        candidate_dir.string(), market_result.error());
        return std::nullopt;
    }

    const Marketplace& m = market_result.value();

    Plugin plugin;
    plugin.name        = m.name;
    plugin.description = m.description;
    plugin.version     = m.version;
    plugin.dir         = candidate_dir;
    plugin.disabled    = false;

    // Mark disabled if the plugin name appears in the disabled list.
    for (const auto& dn : disabled_names) {
        if (dn == m.name) {
            plugin.disabled = true;
            break;
        }
    }

    // Flatten mcp_servers map → vector.
    plugin.mcp_servers.reserve(m.mcp_servers.size());
    for (const auto& [/*server_name*/ _, spec] : m.mcp_servers) {
        plugin.mcp_servers.push_back(spec);
    }

    // Load assets (skills, agents, commands) regardless of disabled state.
    // This allows re-enabling a plugin without a fresh filesystem scan.
    plugin.skills   = load_skills_for_plugin(candidate_dir, m.name);
    plugin.agents   = load_agents_for_plugin(candidate_dir, m.name);
    plugin.commands = load_commands_for_plugin(candidate_dir, m.name);

    return plugin;
}

// ---------------------------------------------------------------------------
// scan_one_root — walk a root directory, discover plugins, overlay into map.
// ---------------------------------------------------------------------------
void scan_one_root(const fs::path& root,
                   const std::vector<std::string>& disabled_names,
                   std::unordered_map<std::string, Plugin>& by_name)
{
    std::error_code ec;
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) return;

    for (const fs::directory_entry& entry : fs::directory_iterator(root, ec)) {
        if (ec) { ec.clear(); continue; }
        if (!entry.is_directory()) continue;

        auto plugin = try_load_one_plugin(entry.path(), disabled_names);
        if (plugin) {
            BATBOX_LOG_INFO("PluginLoader: loaded plugin '{}' from '{}'",
                            plugin->name, entry.path().string());
            by_name.insert_or_assign(plugin->name, std::move(*plugin));
        }
    }
}

} // anonymous namespace

// ============================================================================
// PluginLoader — public implementation
// ============================================================================

PluginLoader::PluginLoader(fs::path settings_path)
    : settings_path_(std::move(settings_path))
{}

// ----------------------------------------------------------------------------
// resolved_settings_path
// ----------------------------------------------------------------------------

fs::path PluginLoader::resolved_settings_path() const {
    if (!settings_path_.empty()) return settings_path_;
    try {
        return batbox::paths::home_dir() / ".batbox" / "settings.json";
    } catch (const std::exception& e) {
        BATBOX_LOG_WARN("PluginLoader: cannot resolve home dir for settings path: {}",
                        e.what());
        return {};
    }
}

// ----------------------------------------------------------------------------
// load_disabled_names
// ----------------------------------------------------------------------------

std::vector<std::string> PluginLoader::load_disabled_names() const {
    const fs::path sp = resolved_settings_path();
    if (sp.empty()) return {};

    // Missing file is not an error — return empty.
    std::error_code ec;
    if (!fs::exists(sp, ec)) return {};

    auto settings_result = batbox::config::load_settings(sp);
    if (!settings_result) {
        BATBOX_LOG_WARN("PluginLoader: cannot read settings.json ({}): {}",
                        sp.string(), settings_result.error());
        return {};
    }
    return settings_result->plugins_disabled;
}

// ----------------------------------------------------------------------------
// build_scan_roots
// ----------------------------------------------------------------------------

std::vector<fs::path> PluginLoader::build_scan_roots() const {
    fs::path home;
    try {
        home = batbox::paths::home_dir();
    } catch (const std::exception& e) {
        BATBOX_LOG_WARN("PluginLoader: cannot determine home dir: {}", e.what());
        home.clear();
    }

    const fs::path project = batbox::paths::project_root();

    // Ascending priority order (later roots overlay earlier ones):
    //   1. ~/.claude/plugins/   (claude-code compat, read-only)
    //   2. ./.claude/plugins/   (project-level compat)
    //   3. ~/.batbox/plugins/   (user global)
    //   4. ./.batbox/plugins/   (project-local, highest priority)
    std::vector<fs::path> roots;
    roots.reserve(4);
    if (!home.empty()) roots.push_back(home    / ".claude"  / "plugins");
    roots.push_back(project / ".claude"  / "plugins");
    if (!home.empty()) roots.push_back(home    / ".batbox"  / "plugins");
    roots.push_back(project / ".batbox"  / "plugins");

    return roots;
}

// ----------------------------------------------------------------------------
// load_all
// ----------------------------------------------------------------------------

Result<std::vector<Plugin>> PluginLoader::load_all() {
    const std::vector<std::string> disabled_names = load_disabled_names();
    const std::vector<fs::path>    roots          = build_scan_roots();

    // Walk roots in order; last-root-wins overlay via unordered_map.
    std::unordered_map<std::string, Plugin> by_name;
    for (const fs::path& root : roots) {
        scan_one_root(root, disabled_names, by_name);
    }

    std::vector<Plugin> result;
    result.reserve(by_name.size());
    for (auto& [_, p] : by_name) {
        result.push_back(std::move(p));
    }

    BATBOX_LOG_INFO("PluginLoader: load_all complete; {} plugin(s) found",
                    result.size());
    return result;
}

// ----------------------------------------------------------------------------
// reload
// ----------------------------------------------------------------------------

Result<void> PluginLoader::reload(PluginRegistry& registry) {
    // Remember registry for add_local / remove.
    registry_ = &registry;

    const std::vector<std::string> disabled_names = load_disabled_names();
    const std::vector<fs::path>    roots          = build_scan_roots();

    // Register all 4 roots with the registry (last-root-wins overlay semantics).
    // PluginRegistry::load_dir() scans from marketplace.json directly and builds
    // Plugin objects with name/version/description/mcp_servers populated.
    // PluginLoader::load_all() provides the asset-rich view (skills/agents/commands).
    // The atomicity guarantee ("no half-state") is provided by:
    //   1. Calling load_dir() for all existing roots.
    //   2. Calling registry.reload() which swaps the full store atomically via
    //      shared_ptr swap under a mutex — readers never see a partial update.
    // Disabled flags are applied after the atomic swap.

    // Register all roots with the registry.
    for (const fs::path& root : roots) {
        std::error_code ec;
        if (fs::exists(root, ec) && fs::is_directory(root, ec)) {
            registry.load_dir(root);
        }
    }
    // Atomic swap via registry's own scan.
    registry.reload();

    // Apply disabled flags to the registry's store.
    if (!disabled_names.empty()) {
        for (const auto& dn : disabled_names) {
            registry.disable(dn);
        }
    }

    BATBOX_LOG_INFO("PluginLoader: reload complete; {} plugin(s) in registry",
                    registry.size());
    return {};
}

// ----------------------------------------------------------------------------
// add_local
// ----------------------------------------------------------------------------

Result<void> PluginLoader::add_local(const fs::path& source_path) {
    std::error_code ec;

    if (!fs::exists(source_path, ec) || !fs::is_directory(source_path, ec)) {
        return Err(std::string("add_local: source path does not exist or is not a directory: ") +
                   source_path.string());
    }

    // Validate that the source looks like a plugin (has a marketplace.json).
    if (!find_marketplace_in_dir(source_path)) {
        return Err(std::string("add_local: source directory '") +
                   source_path.string() +
                   "' does not contain a .claude-plugin/ or .batbox-plugin/ "
                   "subdirectory with marketplace.json");
    }

    // Destination: ~/.batbox/plugins/<basename-of-source>.
    fs::path home;
    try {
        home = batbox::paths::home_dir();
    } catch (const std::exception& e) {
        return Err(std::string("add_local: cannot determine home dir: ") + e.what());
    }

    const fs::path dest_root = home / ".batbox" / "plugins";
    const fs::path dest      = dest_root / source_path.filename();

    // Create destination root if needed.
    fs::create_directories(dest_root, ec);
    if (ec) {
        return Err(std::string("add_local: cannot create plugins directory '") +
                   dest_root.string() + "': " + ec.message());
    }

    // Copy the plugin tree.
    const auto copy_opts = fs::copy_options::recursive
                         | fs::copy_options::overwrite_existing;
    fs::copy(source_path, dest, copy_opts, ec);
    if (ec) {
        return Err(std::string("add_local: copy failed from '") +
                   source_path.string() + "' to '" + dest.string() +
                   "': " + ec.message());
    }

    BATBOX_LOG_INFO("PluginLoader: add_local: copied '{}' to '{}'",
                    source_path.string(), dest.string());

    // Reload the registry if one is attached.
    if (registry_) {
        auto r = reload(*registry_);
        if (!r) {
            return Err(std::string("add_local: reload failed after copy: ") + r.error());
        }
    }

    return {};
}

// ----------------------------------------------------------------------------
// remove
// ----------------------------------------------------------------------------

Result<void> PluginLoader::remove(std::string_view name) {
    fs::path home;
    try {
        home = batbox::paths::home_dir();
    } catch (const std::exception& e) {
        return Err(std::string("remove: cannot determine home dir: ") + e.what());
    }

    const fs::path user_plugin_root = home / ".batbox" / "plugins";
    const fs::path plugin_dir       = user_plugin_root / std::string(name);

    std::error_code ec;
    if (!fs::exists(plugin_dir, ec) || !fs::is_directory(plugin_dir, ec)) {
        return Err(std::string("remove: plugin '") + std::string(name) +
                   "' not found in " + user_plugin_root.string());
    }

    fs::remove_all(plugin_dir, ec);
    if (ec) {
        return Err(std::string("remove: failed to remove '") +
                   plugin_dir.string() + "': " + ec.message());
    }

    BATBOX_LOG_INFO("PluginLoader: removed plugin '{}' from '{}'",
                    std::string(name), plugin_dir.string());

    // Reload the registry if one is attached.
    if (registry_) {
        auto r = reload(*registry_);
        if (!r) {
            return Err(std::string("remove: reload failed after removal: ") + r.error());
        }
    }

    return {};
}

// ----------------------------------------------------------------------------
// load_assets (declared in header but logic is now inlined in try_load_one_plugin)
// ----------------------------------------------------------------------------

void PluginLoader::load_assets(Plugin& plugin, const fs::path& plugin_dir) const {
    plugin.skills   = load_skills_for_plugin(plugin_dir, plugin.name);
    plugin.agents   = load_agents_for_plugin(plugin_dir, plugin.name);
    plugin.commands = load_commands_for_plugin(plugin_dir, plugin.name);
}

// ----------------------------------------------------------------------------
// scan_root
// ----------------------------------------------------------------------------

void PluginLoader::scan_root(const fs::path& root,
                              const std::vector<std::string>& disabled_names,
                              std::unordered_map<std::string, Plugin>& by_name) const {
    scan_one_root(root, disabled_names, by_name);
}

} // namespace batbox::plugins
