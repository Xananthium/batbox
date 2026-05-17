// src/agents/AgentSpec.cpp
// =============================================================================
// Implementation of batbox::agents::AgentSpec.
//
// Parses agent configuration from YAML frontmatter in .md files located at
// ~/.batbox/agents/<name>.md.  Uses batbox::plugins::parse_frontmatter for
// all YAML parsing (no yaml-cpp dependency).
//
// [[ref]] tokens in prompt_body are preserved as-is; cycle detection is
// handled by the caller.  Callers may scan prompt_body with the regex
// \[\[.+?\]\] to enumerate all referenced agent names.
// =============================================================================

#include <batbox/agents/AgentSpec.hpp>
#include <batbox/plugins/FrontmatterParser.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace batbox::agents {

namespace {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/// Read the entire contents of a file into a string.
/// Returns Err with path + system message on failure.
[[nodiscard]] Result<std::string>
read_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return Err("cannot open agent file: " + path.string());
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    if (f.bad()) {
        return Err("read error on agent file: " + path.string());
    }
    return ss.str();
}

/// Extract a std::string from a Json value that must be a string type.
/// Returns Err if the Json value is not a string.
[[nodiscard]] Result<std::string>
require_string_field(const batbox::plugins::Frontmatter& meta,
                     std::string_view key,
                     const std::filesystem::path& source) {
    auto it = meta.find(std::string(key));
    if (it == meta.end()) {
        return Err("agent file '" + source.string() +
                   "': missing required frontmatter field '" + std::string(key) + "'");
    }
    if (!it->second.is_string()) {
        return Err("agent file '" + source.string() +
                   "': frontmatter field '" + std::string(key) + "' must be a string");
    }
    return it->second.get<std::string>();
}

/// Optionally extract a std::string from a Json value.
/// Returns nullopt when the key is absent.
/// Returns Err if the key is present but not a string.
[[nodiscard]] Result<std::optional<std::string>>
optional_string_field(const batbox::plugins::Frontmatter& meta,
                      std::string_view key,
                      const std::filesystem::path& source) {
    auto it = meta.find(std::string(key));
    if (it == meta.end()) {
        return std::optional<std::string>{};
    }
    if (!it->second.is_string()) {
        return Err("agent file '" + source.string() +
                   "': frontmatter field '" + std::string(key) + "' must be a string");
    }
    return std::optional<std::string>{ it->second.get<std::string>() };
}

/// Extract an optional list of strings from a Json value.
/// The value may be a JSON array (from flow- or block-style YAML lists) or
/// absent.  Returns empty vector when the key is absent.
/// Returns Err if the key is present but not an array of strings.
[[nodiscard]] Result<std::vector<std::string>>
optional_string_list(const batbox::plugins::Frontmatter& meta,
                     std::string_view key,
                     const std::filesystem::path& source) {
    auto it = meta.find(std::string(key));
    if (it == meta.end()) {
        return std::vector<std::string>{};
    }
    if (!it->second.is_array()) {
        return Err("agent file '" + source.string() +
                   "': frontmatter field '" + std::string(key) + "' must be a list");
    }
    std::vector<std::string> items;
    items.reserve(it->second.size());
    for (const auto& elem : it->second) {
        if (!elem.is_string()) {
            return Err("agent file '" + source.string() +
                       "': each entry in '" + std::string(key) + "' must be a string");
        }
        items.push_back(elem.get<std::string>());
    }
    return items;
}

/// Returns the platform-specific path to the batbox agents directory:
///   $BATBOX_AGENTS_DIR  (if set, for testing/override)
///   ~/.batbox/agents/   (default)
[[nodiscard]] std::filesystem::path agents_dir() {
    if (const char* env = std::getenv("BATBOX_AGENTS_DIR")) {
        return std::filesystem::path(env);
    }
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    return std::filesystem::path(home) / ".batbox" / "agents";
}

} // anonymous namespace

// =============================================================================
// AgentSpec::from_file
// =============================================================================

// static
Result<AgentSpec>
AgentSpec::from_file(const std::filesystem::path& path) {
    // 1. Read file contents.
    auto content_result = read_file(path);
    if (!content_result) {
        return Err(content_result.error());
    }
    const std::string& content = content_result.value();

    // 2. Parse YAML frontmatter.
    auto parse_result = batbox::plugins::parse_frontmatter(content);
    if (!parse_result) {
        return Err("agent file '" + path.string() +
                   "': frontmatter parse error: " + parse_result.error());
    }

    auto& [meta, body] = parse_result.value();

    // 3. Extract required field: name.
    auto name_result = require_string_field(meta, "name", path);
    if (!name_result) {
        return Err(name_result.error());
    }

    // 4. Extract optional field: description.
    auto desc_result = optional_string_field(meta, "description", path);
    if (!desc_result) {
        return Err(desc_result.error());
    }

    // 5. Extract optional field: model.
    auto model_result = optional_string_field(meta, "model", path);
    if (!model_result) {
        return Err(model_result.error());
    }

    // 6. Extract optional list: tools (allowed tool names).
    auto tools_result = optional_string_list(meta, "tools", path);
    if (!tools_result) {
        return Err(tools_result.error());
    }

    // 7. Assemble AgentSpec.
    AgentSpec spec;
    spec.name         = std::move(name_result.value());
    spec.description  = std::move(desc_result.value()).value_or(std::string{});
    spec.model        = std::move(model_result.value());
    spec.allowed_tools = std::move(tools_result.value());
    spec.prompt_body  = std::move(body);
    spec.source_path  = path;

    return spec;
}

// =============================================================================
// AgentSpec::from_type
// =============================================================================

// static
AgentSpec AgentSpec::from_type(std::string_view subagent_type) {
    // Attempt to load ~/.batbox/agents/<subagent_type>.md.
    const auto md_path = agents_dir() / (std::string(subagent_type) + ".md");

    if (std::filesystem::exists(md_path)) {
        auto result = from_file(md_path);
        if (result) {
            return std::move(result.value());
        }
        // File exists but failed to parse — fall through to generic spec.
        // Callers relying on clean configs should treat the file error as
        // informational; the fallback spec allows the agent to still launch.
    }

    // Generic fallback: use the subagent_type string as display name.
    AgentSpec spec;
    spec.name        = std::string(subagent_type);
    spec.description = "Agent of type: " + std::string(subagent_type);
    // model, allowed_tools, prompt_body, source_path remain default-initialised.
    return spec;
}

} // namespace batbox::agents
