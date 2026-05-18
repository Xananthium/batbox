# src/plugins

Plugin system implementations: frontmatter parsing, marketplace.json loading, skill/agent/command loaders, registry, and the top-level four-root scanner.

## Files

### FrontmatterParser.cpp
`parse_frontmatter()` implementation: line-by-line YAML subset parser; handles key:value scalars, quoted strings, flow lists [a, b, c], block lists (- item), boolean coercion, integer coercion; returns Err with "line:col: message" on malformed input.

### MarketplaceJson.cpp
`parse_marketplace_json()` implementation: reads marketplace.json; extracts name, version, description, author; returns Err on missing required "name" field.

### SkillLoader.cpp
Scans skill directories; reads each .md file; calls parse_frontmatter(); maps frontmatter keys to Skill struct fields; stores by name for get() lookups.

### AgentLoader.cpp
Scans agent directories; reads each .md file; calls parse_frontmatter(); maps frontmatter to AgentSpec; detects dependency cycles via DFS on the "depends_on" key.

### CommandLoader.cpp
Scans command directories; reads each .md file; calls parse_frontmatter(); maps frontmatter to Command struct; registers user-defined slash commands.

### PluginLoader.cpp
`load_all()` implementation: builds four scan roots; calls scan_root() for each in ascending priority; overlays later roots over earlier via the by_name map; reads disabled names from settings.json. `add_local()`: copies source_path into ~/.batbox/plugins/ via std::filesystem::copy_recursive; calls reload(). `remove()`: removes the named directory via fs::remove_all; calls reload().

### PluginRegistry.cpp
`load_dir()` implementation: walks immediate subdirectories; looks for .batbox-plugin/marketplace.json and .claude-plugin/marketplace.json; populates by-name map. `reload()`: calls scan_roots(); wraps new vector in shared_ptr; swaps under mutex. `get_skill/agent/command()`: iterate active_plugins(); return first name match.

### CMakeLists.txt
Build rules for the plugins static library.
