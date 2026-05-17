// src/agents/AgentLoader.cpp
// =============================================================================
// Implementation of batbox::agents::AgentLoader.
//
// Scans ~/.batbox/agents/*.md, parses YAML frontmatter into AgentSpec structs,
// stores in unordered_map<string, AgentSpec>.  Builds [[name]] cross-ref graph;
// cycle detection emits BATBOX_LOG_ERROR.  Supports reload().
//
// Blueprint contract (task CPP 6.3):
//   blueprints row 16756: class batbox::agents::AgentLoader
//   blueprints row 16757: method names() → vector<string>
//   blueprints row 16755: file agents/AgentLoader.hpp
//   blueprints row 16758: file agents/AgentLoader.cpp
// =============================================================================

#include <batbox/agents/AgentLoader.hpp>

#include <batbox/core/Logging.hpp>
#include <batbox/core/Paths.hpp>
#include <batbox/plugins/FrontmatterParser.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace batbox::agents {

namespace {

// ---------------------------------------------------------------------------
// read_file_to_string — slurp a file into a std::string.
// Returns empty string on I/O error.
// ---------------------------------------------------------------------------
[[nodiscard]] std::string read_file_to_string(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ---------------------------------------------------------------------------
// extract_string_field — pull a string from a Frontmatter map by key.
// Returns empty string when key is absent or value is not a string.
// ---------------------------------------------------------------------------
[[nodiscard]] std::string extract_string_field(const plugins::Frontmatter& meta,
                                               const std::string& key) {
    auto it = meta.find(key);
    if (it == meta.end()) return {};
    const Json& v = it->second;
    if (v.is_string()) return v.get<std::string>();
    return {};
}

// ---------------------------------------------------------------------------
// extract_string_list — pull an array of strings from a Frontmatter map.
// Accepts both JSON arrays and a bare string (single-item list).
// Returns empty vector when key is absent or malformed.
// ---------------------------------------------------------------------------
[[nodiscard]] std::vector<std::string> extract_string_list(
        const plugins::Frontmatter& meta, const std::string& key) {
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
// extract_refs — collect [[agent-name]] tokens from prompt_body.
// Returns a sorted, deduplicated list of referenced agent names.
// ---------------------------------------------------------------------------
[[nodiscard]] std::vector<std::string> extract_refs(const std::string& prompt_body) {
    std::vector<std::string> refs;
    const std::string& text = prompt_body;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t open = text.find("[[", pos);
        if (open == std::string::npos) break;
        std::size_t close = text.find("]]", open + 2);
        if (close == std::string::npos) break;

        std::string ref = text.substr(open + 2, close - open - 2);
        // Trim whitespace
        while (!ref.empty() && (ref.front() == ' ' || ref.front() == '\t')) ref.erase(ref.begin());
        while (!ref.empty() && (ref.back()  == ' ' || ref.back()  == '\t')) ref.pop_back();

        if (!ref.empty()) refs.push_back(std::move(ref));
        pos = close + 2;
    }
    // Deduplicate
    std::sort(refs.begin(), refs.end());
    refs.erase(std::unique(refs.begin(), refs.end()), refs.end());
    return refs;
}

// ---------------------------------------------------------------------------
// dfs_has_cycle — depth-first search cycle detection on the ref graph.
//
// graph: agent name → list of referenced agent names
// node:  current node being visited
// visited: permanently visited set
// path_set: nodes on the current DFS path (for cycle detection)
// cycle_names: output — names of agents in detected cycles
//
// Returns true if a cycle is reachable from 'node'.
// ---------------------------------------------------------------------------
bool dfs_has_cycle(
        const std::unordered_map<std::string, std::vector<std::string>>& graph,
        const std::string& node,
        std::unordered_set<std::string>& visited,
        std::unordered_set<std::string>& path_set,
        std::vector<std::string>& cycle_names) {

    if (path_set.count(node)) {
        // Back edge found — cycle!
        cycle_names.push_back(node);
        return true;
    }
    if (visited.count(node)) return false;

    path_set.insert(node);
    auto it = graph.find(node);
    if (it != graph.end()) {
        for (const std::string& neighbour : it->second) {
            if (dfs_has_cycle(graph, neighbour, visited, path_set, cycle_names)) {
                cycle_names.push_back(node);
                path_set.erase(node);
                visited.insert(node);
                return true;
            }
        }
    }
    path_set.erase(node);
    visited.insert(node);
    return false;
}

} // anonymous namespace

// =============================================================================
// AgentLoader — public API
// =============================================================================

std::vector<AgentSpec> AgentLoader::load() {
    // Resolve the default agents directory: ~/.batbox/agents/
    std::filesystem::path agents_dir;
    try {
        agents_dir = paths::home_dir() / ".batbox" / "agents";
    } catch (const std::exception& e) {
        BATBOX_LOG_WARN("AgentLoader: cannot determine home dir: {}", e.what());
        return {};
    }
    return load_from(agents_dir);
}

std::vector<AgentSpec> AgentLoader::load_from(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;

    if (!fs::exists(dir) || !fs::is_directory(dir)) {
        // Missing directory is not an error — just return empty.
        return {};
    }

    std::error_code ec;
    for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
        if (ec) {
            BATBOX_LOG_WARN("AgentLoader: error iterating {}: {}",
                            dir.string(), ec.message());
            break;
        }
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".md") continue;

        auto spec_opt = parse_agent_file(entry.path());
        if (!spec_opt) continue; // warning already emitted inside parse_agent_file
        upsert_by_mtime(std::move(*spec_opt));
    }

    // Build cross-ref graph and run cycle detection.
    build_and_check_refs();

    // Return all loaded agents sorted by name.
    std::vector<AgentSpec> result;
    result.reserve(agents_.size());
    for (const auto& [name, spec] : agents_) {
        result.push_back(spec);
    }
    std::sort(result.begin(), result.end(),
              [](const AgentSpec& a, const AgentSpec& b) { return a.name < b.name; });
    return result;
}

void AgentLoader::reload() {
    agents_.clear();
    has_cycle_ = false;
    load();
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

std::optional<AgentSpec> AgentLoader::get(std::string_view name) const {
    auto it = agents_.find(std::string(name));
    if (it == agents_.end()) return std::nullopt;
    return it->second;
}

// =============================================================================
// AgentLoader — private helpers
// =============================================================================

// static
std::optional<AgentSpec> AgentLoader::parse_agent_file(
        const std::filesystem::path& path) {

    std::string content = read_file_to_string(path);
    if (content.empty() && !std::filesystem::exists(path)) {
        BATBOX_LOG_WARN("AgentLoader: cannot read file: {}", path.string());
        return std::nullopt;
    }

    auto result = plugins::parse_frontmatter(content);
    if (!result) {
        BATBOX_LOG_WARN("AgentLoader: malformed frontmatter in {}: {}",
                        path.string(), result.error());
        return std::nullopt;
    }

    const plugins::Frontmatter& meta = result->first;
    const std::string& body          = result->second;

    // "name" is mandatory.
    std::string name = extract_string_field(meta, "name");
    if (name.empty()) {
        BATBOX_LOG_WARN("AgentLoader: skipping {} — missing required 'name' field",
                        path.string());
        return std::nullopt;
    }

    AgentSpec spec;
    spec.name        = std::move(name);
    spec.description = extract_string_field(meta, "description");
    spec.prompt_body = body;
    spec.source_path = path;

    // Optional model override.
    std::string model_str = extract_string_field(meta, "model");
    if (!model_str.empty()) spec.model = std::move(model_str);

    // allowed_tools — accept both flow-style [A, B] and block-style lists.
    spec.allowed_tools = extract_string_list(meta, "allowed_tools");

    return spec;
}

void AgentLoader::upsert_by_mtime(AgentSpec spec) {
    namespace fs = std::filesystem;

    auto it = agents_.find(spec.name);
    if (it == agents_.end()) {
        // No existing entry — insert unconditionally.
        agents_[spec.name] = std::move(spec);
        return;
    }

    // Duplicate name: compare mtimes.  The newer file wins.
    std::error_code ec_new, ec_old;
    auto mtime_new = fs::last_write_time(spec.source_path,          ec_new);
    auto mtime_old = fs::last_write_time(it->second.source_path,    ec_old);

    if (ec_new || ec_old) {
        // If we can't stat either file, let the new entry win (safe default).
        BATBOX_LOG_WARN("AgentLoader: cannot compare mtimes for agent '{}' "
                        "(new={}, old={}); new file wins",
                        spec.name,
                        spec.source_path.string(),
                        it->second.source_path.string());
        it->second = std::move(spec);
        return;
    }

    if (mtime_new >= mtime_old) {
        it->second = std::move(spec);
    }
    // else: keep the existing (older) entry — it has a later mtime.
}

void AgentLoader::build_and_check_refs() {
    // Build adjacency list: agent name → list of [[referenced]] agent names.
    std::unordered_map<std::string, std::vector<std::string>> graph;
    for (const auto& [name, spec] : agents_) {
        graph[name] = extract_refs(spec.prompt_body);
    }

    // DFS cycle detection over all nodes.
    std::unordered_set<std::string> visited;
    std::vector<std::string> all_cycle_names;

    for (const auto& [name, _] : agents_) {
        if (!visited.count(name)) {
            std::unordered_set<std::string> path_set;
            std::vector<std::string> cycle_names;
            if (dfs_has_cycle(graph, name, visited, path_set, cycle_names)) {
                for (const auto& cn : cycle_names) {
                    all_cycle_names.push_back(cn);
                }
            }
        }
    }

    if (!all_cycle_names.empty()) {
        has_cycle_ = true;
        // Deduplicate for the error message.
        std::sort(all_cycle_names.begin(), all_cycle_names.end());
        all_cycle_names.erase(std::unique(all_cycle_names.begin(),
                                          all_cycle_names.end()),
                              all_cycle_names.end());
        std::string names_str;
        for (const auto& n : all_cycle_names) {
            if (!names_str.empty()) names_str += ", ";
            names_str += n;
        }
        BATBOX_LOG_ERROR("AgentLoader: [[name]] reference cycle detected "
                         "involving agent(s): {}", names_str);
    } else {
        has_cycle_ = false;
    }
}

} // namespace batbox::agents
