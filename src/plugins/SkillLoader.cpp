// src/plugins/SkillLoader.cpp
// =============================================================================
// Implementation of batbox::plugins::SkillLoader.
//
// Scan roots (in order, later overlays earlier):
//   1. ~/.claude/skills/
//   2. ./.claude/skills/
//   3. ~/.batbox/skills/
//   4. ./.batbox/skills/
//
// Plus any plugin-bundled skills added via scan_dir().
// Bundled (embedded) skills injected via set_bundled_skills() are lowest priority.
// =============================================================================

#include <batbox/plugins/SkillLoader.hpp>

#include <batbox/core/Logging.hpp>
#include <batbox/core/Paths.hpp>
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
// read_file_to_string — slurp a file into a std::string.
// Returns empty string on error; caller should already have checked exists().
// ---------------------------------------------------------------------------
[[nodiscard]] std::string read_file_to_string(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ---------------------------------------------------------------------------
// extract_string_field — safely pull a string value from a Frontmatter map.
// Returns empty string when the key is absent or not a string type.
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
// extract_string_list — pull a JSON array of strings from a Frontmatter map.
// Accepts both JSON arrays and a bare string (treated as a single-item list).
// Returns empty vector when the key is absent or malformed.
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

} // anonymous namespace

// =============================================================================
// SkillLoader — public API
// =============================================================================

void SkillLoader::set_bundled_skills(std::vector<Skill> bundled) {
    bundled_skills_ = std::move(bundled);
    // Re-seed skills_ with bundled as the base, then re-apply any user-dir/plugin
    // skills currently loaded (by rebuilding from bundled and re-running dirs).
    // Since this is typically called once at startup before load_user_dirs(),
    // we simply replace the bundled entries in the map.
    for (const Skill& s : bundled_skills_) {
        // Only insert if not already overridden by a higher-priority source.
        if (skills_.find(s.name) == skills_.end()) {
            skills_[s.name] = s;
        }
    }
}

void SkillLoader::load_user_dirs() {
    namespace fs = std::filesystem;

    // Resolve scan roots.
    // home_dir() returns the user's home directory (throws on failure).
    // project_root() walks up from cwd to find .git / BATBOX.md.
    fs::path home;
    fs::path project;
    try {
        home = paths::home_dir();
    } catch (const std::exception& e) {
        BATBOX_LOG_WARN("SkillLoader: cannot determine home dir: {}", e.what());
        return;
    }
    project = paths::project_root();

    // The 4 roots in ascending priority order (later overrides earlier).
    const std::array<fs::path, 4> roots = {
        home    / ".claude"  / "skills",
        project / ".claude"  / "skills",
        home    / ".batbox"  / "skills",
        project / ".batbox"  / "skills",
    };

    for (const fs::path& root : roots) {
        if (!fs::exists(root) || !fs::is_directory(root)) continue;
        scan_dir(root, "user-dir");
    }
}

void SkillLoader::scan_dir(const std::filesystem::path& dir,
                            std::string_view source_tag) {
    namespace fs = std::filesystem;

    if (!fs::exists(dir) || !fs::is_directory(dir)) return;

    std::error_code ec;
    for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
        if (ec) {
            BATBOX_LOG_WARN("SkillLoader: error iterating {}: {}", dir.string(), ec.message());
            break;
        }
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".md") continue;

        auto skill_opt = parse_skill_file(entry.path(), source_tag);
        if (!skill_opt) continue; // warning already logged inside parse_skill_file
        upsert(std::move(*skill_opt));
    }
}

void SkillLoader::reload() {
    // Remove all non-bundled skills from the map.
    // Bundled skill names are in bundled_skills_.
    std::unordered_map<std::string, Skill> fresh;
    // Seed with bundled base.
    for (const Skill& s : bundled_skills_) {
        fresh[s.name] = s;
    }
    skills_ = std::move(fresh);
    // Re-scan user dirs (plugins must be re-added by their owner after reload).
    load_user_dirs();
}

std::vector<std::string> SkillLoader::names() const {
    std::vector<std::string> result;
    result.reserve(skills_.size());
    for (const auto& [name, _] : skills_) {
        result.push_back(name);
    }
    std::sort(result.begin(), result.end());
    return result;
}

const Skill* SkillLoader::find(std::string_view name) const {
    auto it = skills_.find(std::string(name));
    if (it == skills_.end()) return nullptr;
    return &it->second;
}

Result<tools::ToolResult> SkillLoader::run(std::string_view name,
                                             tools::ToolContext& /*ctx*/) {
    const Skill* skill = find(name);
    if (!skill) {
        return Err(std::string("skill not found: ") + std::string(name));
    }

    std::string body = skill->prompt_body;

    // If there is a companion script, append a note.  Actual execution is
    // handled by SkillTool (CPP 5.19) which calls BashTool after injecting
    // the prompt.
    if (skill->script_path.has_value()) {
        body += "\n\n[Script available: " + skill->script_path->string() + "]";
    }

    return tools::ToolResult::ok(std::move(body));
}

// =============================================================================
// SkillLoader — private helpers
// =============================================================================

std::optional<Skill> SkillLoader::parse_skill_file(
        const std::filesystem::path& path,
        std::string_view source_tag) const {

    std::string content = read_file_to_string(path);
    if (content.empty() && !std::filesystem::exists(path)) {
        BATBOX_LOG_WARN("SkillLoader: cannot read file: {}", path.string());
        return std::nullopt;
    }

    auto result = parse_frontmatter(content);
    if (!result) {
        BATBOX_LOG_WARN("SkillLoader: malformed frontmatter in {}: {}",
                        path.string(), result.error());
        return std::nullopt;
    }

    const Frontmatter& meta = result->first;
    const std::string& body = result->second;

    // "name" is mandatory.
    std::string name = extract_string_field(meta, "name");
    if (name.empty()) {
        BATBOX_LOG_WARN("SkillLoader: skipping {} — missing required 'name' field",
                        path.string());
        return std::nullopt;
    }

    Skill skill;
    skill.name        = std::move(name);
    skill.description = extract_string_field(meta, "description");
    skill.prompt_body = body;
    skill.source      = std::string(source_tag);

    // Optional model override.
    std::string model_str = extract_string_field(meta, "model");
    if (!model_str.empty()) skill.model = std::move(model_str);

    // allowed_tools — accept both flow-style [A, B] and block-style lists.
    skill.allowed_tools = extract_string_list(meta, "allowed_tools");

    // script.sh companion check.
    std::filesystem::path script = path.parent_path() / "script.sh";
    if (std::filesystem::exists(script)) {
        skill.script_path = std::move(script);
    }

    return skill;
}

void SkillLoader::upsert(Skill skill) {
    skills_[skill.name] = std::move(skill);
}

} // namespace batbox::plugins
