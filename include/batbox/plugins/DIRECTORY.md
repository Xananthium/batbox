# include/batbox/plugins

Plugin system headers: the four-root scanner, in-memory registry, asset loaders for skills/agents/commands, YAML frontmatter parser, and data types.

## Files

### FrontmatterParser.hpp
Minimal hand-rolled YAML frontmatter parser for .md files.

- `parse_frontmatter(md_content) -> Result<FrontmatterResult>` — splits md_content at opening/closing "---" delimiters; parses key:value pairs, flow-style lists, block lists, booleans, and integers; returns (Frontmatter map, body string) or Err with "line:col: message"

### PluginLoader.hpp
Top-level scanner that discovers plugins under four standard roots and populates a PluginRegistry.

- `PluginLoader::PluginLoader(settings_path={})` — constructs; reads disabled plugin names from settings_path (defaults to ~/.batbox/settings.json)
- `PluginLoader::load_all() -> Result<vector<Plugin>>` — scans all four roots in ascending priority order; returns merged Plugin list with disabled flags set
- `PluginLoader::reload(registry) -> Result<void>` — rescans all roots and atomically swaps the result into registry via PluginRegistry::reload()
- `PluginLoader::add_local(source_path) -> Result<void>` — copies source_path into ~/.batbox/plugins/; calls reload() on the cached registry
- `PluginLoader::remove(name) -> Result<void>` — removes ~/.batbox/plugins/<name>/; calls reload() on the cached registry

### PluginRegistry.hpp
Thread-safe in-memory registry of loaded plugins with atomic-swap reload.

- `PluginRegistry::load_dir(dir)` — scans dir for immediate plugin subdirectories; appends found plugins; registers dir as a scan root
- `PluginRegistry::reload()` — rescans all registered roots; atomically swaps the new plugin store via shared_ptr swap under mutex
- `PluginRegistry::get_snapshot() -> shared_ptr<const vector<Plugin>>` — returns a snapshot shared_ptr; stable across concurrent reloads
- `PluginRegistry::all_plugins() -> vector<Plugin>&` — returns const ref to current full plugin list (not reload-safe across threads; prefer get_snapshot)
- `PluginRegistry::active_plugins() -> vector<Plugin>` — returns copy filtered to enabled-only entries
- `PluginRegistry::get(name) -> const Plugin*` — finds plugin by name; returns nullptr when not found
- `PluginRegistry::enable(name) -> bool` — clears disabled flag; returns true when state changed
- `PluginRegistry::disable(name) -> bool` — sets disabled flag; returns true when state changed
- `PluginRegistry::get_skill(name) -> optional<Skill>` — finds first Skill matching name across active plugins
- `PluginRegistry::get_agent(name) -> optional<Agent>` — finds first Agent matching name across active plugins
- `PluginRegistry::get_command(name) -> optional<Command>` — finds first Command matching name across active plugins
- `PluginRegistry::size() -> size_t` — total plugin count including disabled
- `PluginRegistry::empty() -> bool` — true when no plugins are loaded

### SkillLoader.hpp
Loads Skill objects from .md files in user and plugin directories.

- `SkillLoader::load() -> Result<void>` — scans registered skill directories; parses frontmatter; populates the skill map
- `SkillLoader::set_bundled_skills(skills)` — installs pre-parsed bundled skills (from BundledSkillsRegistry::all())
- `SkillLoader::reload()` — rescans all directories; replaces existing skills
- `SkillLoader::names() -> vector<string>` — returns all loaded skill names
- `SkillLoader::get(name) -> optional<Skill>` — looks up skill by name

### AgentLoader.hpp
Loads AgentSpec objects from .md files in user and plugin directories.

- `AgentLoader::load() -> Result<void>` — scans agent dirs; parses frontmatter; populates agent map
- `AgentLoader::load_from(dir) -> Result<void>` — scans a single directory
- `AgentLoader::reload() -> Result<void>` — rescans all dirs
- `AgentLoader::names() -> vector<string>` — returns all loaded agent names
- `AgentLoader::get(name) -> optional<AgentSpec>` — looks up agent by name
- `AgentLoader::size() -> size_t` — count of loaded agents
- `AgentLoader::has_cycle_error() -> bool` — true when a dependency cycle was found

### CommandLoader.hpp
Loads user-defined slash Command objects from .md files.

- `CommandLoader::load() -> Result<void>` — scans command dirs; parses frontmatter; populates command map
- `CommandLoader::reload() -> Result<void>` — rescans all dirs
- `CommandLoader::names() -> vector<string>` — returns all loaded command names
- `CommandLoader::get(name) -> optional<Command>` — looks up command by name

### MarketplaceJson.hpp
Parser for marketplace.json plugin manifests.

- `parse_marketplace_json(path) -> Result<PluginManifest>` — reads and parses a marketplace.json file; extracts name, version, description, and author fields; returns Err on missing required fields

### Plugin.hpp
Plugin data type: bundles manifest metadata with loaded skills, agents, and commands.

- `Plugin` — struct: name, version, description, author, dir, disabled flag, skills vector, agents vector, commands vector
