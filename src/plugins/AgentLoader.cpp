// src/plugins/AgentLoader.cpp
// =============================================================================
// Implementation of batbox::plugins::AgentLoader.
//
// Scans plugin-bundled agents under dual-name paths:
//   <plugin_dir>/.claude-plugin/agents/*.md
//   <plugin_dir>/.batbox-plugin/agents/*.md
//
// Each .md file is parsed via FrontmatterParser → batbox::plugins::Agent.
// Collision between agents with the same name from different plugins is handled
// by saving the prior entry under a namespaced key before overwriting.
// =============================================================================

#include <batbox/plugins/AgentLoader.hpp>

#include <batbox/core/Logging.hpp>
#include <batbox/plugins/FrontmatterParser.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace batbox::plugins {

namespace {

// ---------------------------------------------------------------------------
// read_file_to_string — slurp a file into std::string.
// Returns empty string on I/O error (caller checks separately).
// ---------------------------------------------------------------------------
[[nodiscard]] std::string read_file_to_string(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ---------------------------------------------------------------------------
// extract_string_field — pull a string value from a Frontmatter map.
// Returns empty string when key absent or value is not a string.
// ---------------------------------------------------------------------------
[[nodiscard]] std::string extract_string_field(const Frontmatter& meta,
                                                const std::string& key) {
    auto it = meta.find(key);
    if (it == meta.end()) return {};
    const Json& v = it->second;
    if (v.is_string()) return v.get<std::string>();
    return {};
}

// ---------------------------------------------------------------------------
// extract_string_list — pull a JSON array of strings from Frontmatter.
// Accepts both JSON arrays and a bare string (single-item list).
// Returns empty vector when key absent or value is malformed.
// ---------------------------------------------------------------------------
[[nodiscard]] std::vector<std::string> extract_string_list(const Frontmatter& meta,
                                                            const std::string& key) {
    auto it = meta.find(key);
    if (it == meta.end()) return {};
    const Json& v = it->second;
    std::vector<std::string> result;
    if (v.is_array()) {
        result.reserve(v.size());
        for (const auto& item : v) {
            if (item.is_string()) result.push_back(item.get<std::string>());
        }
    } else if (v.is_string()) {
        result.push_back(v.get<std::string>());
    }
    return result;
}

// ---------------------------------------------------------------------------
// plugin_name_from_source — extract the plugin name from a source tag of the
// form "plugin:<plugin-name>".  Returns the full source tag on any other form.
// ---------------------------------------------------------------------------
[[nodiscard]] std::string plugin_name_from_source(std::string_view source) {
    constexpr std::string_view prefix = "plugin:";
    if (source.substr(0, prefix.size()) == prefix) {
        return std::string(source.substr(prefix.size()));
    }
    return std::string(source);
}

} // anonymous namespace

// =============================================================================
// AgentLoader — public API
// =============================================================================

void AgentLoader::load_plugin_dir(const std::filesystem::path& plugin_dir,
                                   std::string_view             plugin_name) {
    namespace fs = std::filesystem;

    // Dual-name compatibility: try both plugin directory name conventions.
    // .claude-plugin is the claude-code-compatible name; .batbox-plugin is the
    // batbox-native name.  Both are probed; files found in either are loaded.
    // Within a single plugin dir, .batbox-plugin is scanned after .claude-plugin
    // so .batbox-plugin agents win on name collision (batbox-native preferred).
    const std::array<fs::path, 2> agent_subdirs = {
        plugin_dir / ".claude-plugin" / "agents",
        plugin_dir / ".batbox-plugin" / "agents",
    };

    for (const fs::path& agents_dir : agent_subdirs) {
        if (!fs::exists(agents_dir) || !fs::is_directory(agents_dir)) continue;
        scan_dir(agents_dir, plugin_name);
    }
}

void AgentLoader::scan_dir(const std::filesystem::path& agents_dir,
                             std::string_view             plugin_name) {
    namespace fs = std::filesystem;

    if (!fs::exists(agents_dir) || !fs::is_directory(agents_dir)) return;

    std::error_code ec;
    for (const fs::directory_entry& entry : fs::directory_iterator(agents_dir, ec)) {
        if (ec) {
            BATBOX_LOG_WARN("AgentLoader: error iterating '{}': {}",
                            agents_dir.string(), ec.message());
            break;
        }
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".md") continue;

        auto agent_opt = parse_agent_file(entry.path(), plugin_name);
        if (!agent_opt) continue; // warning already logged inside parse_agent_file
        upsert(std::move(*agent_opt));
    }
}

void AgentLoader::clear() {
    agents_.clear();
}

std::vector<std::string> AgentLoader::names() const {
    std::vector<std::string> result;
    result.reserve(agents_.size());
    for (const auto& [name, _] : agents_) {
        result.push_back(name);
    }
    std::sort(result.begin(), result.end());
    return result;
}

const Agent* AgentLoader::find(std::string_view name) const {
    auto it = agents_.find(std::string(name));
    if (it == agents_.end()) return nullptr;
    return &it->second;
}

std::vector<Agent> AgentLoader::all() const {
    std::vector<Agent> result;
    result.reserve(agents_.size());
    for (const auto& [_, agent] : agents_) {
        result.push_back(agent);
    }
    return result;
}

// =============================================================================
// AgentLoader — private helpers
// =============================================================================

std::optional<Agent> AgentLoader::parse_agent_file(
        const std::filesystem::path& path,
        std::string_view             plugin_name) const {

    std::string content = read_file_to_string(path);
    if (content.empty() && !std::filesystem::exists(path)) {
        BATBOX_LOG_WARN("AgentLoader: cannot read file: {}", path.string());
        return std::nullopt;
    }

    auto result = parse_frontmatter(content);
    if (!result) {
        BATBOX_LOG_WARN("AgentLoader: malformed frontmatter in '{}': {}",
                        path.string(), result.error());
        return std::nullopt;
    }

    const Frontmatter& meta = result->first;
    const std::string& body = result->second;

    // "name" is mandatory — fall back to filename stem when absent.
    std::string name = extract_string_field(meta, "name");
    if (name.empty()) {
        name = path.stem().string();
        if (name.empty()) {
            BATBOX_LOG_WARN("AgentLoader: skipping '{}' — cannot determine agent name",
                            path.string());
            return std::nullopt;
        }
        BATBOX_LOG_WARN("AgentLoader: '{}' missing frontmatter 'name'; using stem '{}'",
                        path.string(), name);
    }

    Agent agent;
    agent.name        = std::move(name);
    agent.description = extract_string_field(meta, "description");
    agent.prompt_body = body;
    agent.source      = "plugin:" + std::string(plugin_name);

    // Optional model override.
    std::string model_str = extract_string_field(meta, "model");
    if (!model_str.empty()) agent.model = std::move(model_str);

    // allowed_tools — accept both flow-style [A, B] and block-style lists.
    agent.allowed_tools = extract_string_list(meta, "allowed_tools");

    // Optional companion script alongside the .md file.
    std::filesystem::path script = path.parent_path() / "script.sh";
    if (std::filesystem::exists(script)) {
        agent.script_path = std::move(script);
    }

    return agent;
}

void AgentLoader::upsert(Agent agent) {
    const std::string& name = agent.name;

    auto it = agents_.find(name);
    if (it != agents_.end()) {
        const Agent& existing = it->second;
        // If the existing entry comes from a DIFFERENT plugin, save it under
        // the namespaced key "<plugin-name>/<agent-name>" so callers can still
        // address it unambiguously.  Same-plugin re-scan just overwrites.
        const std::string existing_plugin = plugin_name_from_source(existing.source);
        const std::string incoming_plugin = plugin_name_from_source(agent.source);

        if (existing_plugin != incoming_plugin && !existing_plugin.empty()) {
            // Preserve the prior agent under its namespaced form.
            std::string namespaced_key = existing_plugin + "/" + name;
            // Only store if there isn't already an explicit namespaced entry.
            if (agents_.find(namespaced_key) == agents_.end()) {
                Agent prior_copy = existing;
                agents_[std::move(namespaced_key)] = std::move(prior_copy);
            }
        }
    }

    agents_[name] = std::move(agent);
}

} // namespace batbox::plugins
