// include/batbox/plugins/SkillLoader.hpp
// =============================================================================
// batbox::plugins::SkillLoader — scan skill directories, parse .md files into
// Skill structs, deduplicate by name, and invoke skill prompts on request.
//
// Design (per ned-cpp.md §2.C11):
//   Skills are markdown files with YAML frontmatter (name, description,
//   allowed_tools, model, enabled) followed by a freeform prompt body.
//   SkillLoader scans 4 roots in order; later roots override earlier ones
//   when the same skill name appears in multiple places:
//
//     1. ~/.claude/skills/       (read-only claude-code compat)
//     2. ./.claude/skills/       (project-level compat)
//     3. ~/.batbox/skills/       (user global)
//     4. ./.batbox/skills/       (project-local, highest priority among user dirs)
//
//   Plugin-bundled skills are added via scan_dir() calls from PluginLoader.
//   Bundled (embedded) skills are the lowest-priority base; user-dir skills
//   always win on name collision.  Bundled skills are injected via
//   set_bundled_skills() called by App::init once BundledSkillsRegistry lands
//   (CPP K.0).
//
// Skill struct:
//   Declared here (not in a separate file) so SkillLoader.hpp is self-contained
//   and BundledSkillsRegistry.hpp can include it without circular deps.
//
// Graceful failure:
//   Malformed frontmatter, missing required "name" key, or unreadable files
//   log a BATBOX_LOG_WARN and are silently skipped. The scan always continues.
//
// Build standalone (from repo root):
//   c++ -std=c++20 -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/integration/test_skill_loader.cpp \
//       src/plugins/FrontmatterParser.cpp \
//       src/plugins/SkillLoader.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libfmt.a \
//       -o /tmp/test_skill_loader && /tmp/test_skill_loader
// =============================================================================

#pragma once

#include <batbox/core/Json.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace batbox::plugins {

// =============================================================================
// Skill — one parsed skill file.
// =============================================================================

/// A skill loaded from a .md file in a skills/ directory.
/// Matches the shape in ned-cpp.md §2.C11 exactly.
struct Skill {
    /// The skill's canonical name (from frontmatter "name" key).
    /// Used as the deduplication key and as the autocomplete entry.
    std::string name;

    /// Human-readable description (from frontmatter "description" key).
    std::string description;

    /// Optional model override (e.g. "claude-opus-4-5").
    std::optional<std::string> model;

    /// Tool allow-list for this skill (from frontmatter "allowed_tools").
    /// Empty means "no tool restriction specified".
    std::vector<std::string> allowed_tools;

    /// The markdown body text after the closing --- delimiter.
    /// This is the prompt injected into the conversation when the skill runs.
    std::string prompt_body;

    /// If a script.sh exists alongside the .md file, this holds its path.
    /// Actual script execution is deferred to SkillTool (CPP 5.19).
    std::optional<std::filesystem::path> script_path;

    /// Provenance string: "user-dir" for scanned directories,
    /// "plugin:<plugin-name>" for plugin-bundled skills,
    /// "bundled" for embedded skills from BundledSkillsRegistry.
    std::string source;

    bool operator==(const Skill&) const = default;
};

// =============================================================================
// SkillLoader
// =============================================================================

/// Loads, deduplicates, and invokes skills from user directories and plugins.
///
/// Typical lifecycle:
///   1. Construct a SkillLoader.
///   2. Optionally call set_bundled_skills() to seed the base set.
///   3. Call load_user_dirs() to scan the 4 standard roots.
///   4. Call scan_dir() once per plugin to add plugin-bundled skills.
///   5. Call names() for autocomplete or run() to execute a skill.
///   6. Call reload() to redo steps 2–4 in place (atomic swap of skills_).
///
/// Thread safety: not thread-safe. Callers that need concurrent access should
/// hold an external mutex around load/reload calls. names() and run() are safe
/// to call concurrently after loading is complete (no mutation).
class SkillLoader {
public:
    SkillLoader() = default;

    // -------------------------------------------------------------------------
    // Loading API
    // -------------------------------------------------------------------------

    /// Seed the bundled (embedded) skills that form the lowest-priority base.
    /// Called by App::init after BundledSkillsRegistry (CPP K.0) is available.
    /// Any subsequent load_user_dirs() or scan_dir() call will overlay these.
    /// Calling this again replaces the current bundled set.
    void set_bundled_skills(std::vector<Skill> bundled);

    /// Scan the 4 standard user-directory roots in order:
    ///   1. ~/.claude/skills/
    ///   2. ./.claude/skills/   (relative to batbox::paths::project_root())
    ///   3. ~/.batbox/skills/
    ///   4. ./.batbox/skills/   (relative to batbox::paths::project_root())
    ///
    /// Skills found in later roots override those in earlier roots (same name).
    /// All user-dir skills override bundled skills.
    /// Non-existent directories are silently skipped.
    void load_user_dirs();

    /// Scan a single directory for .md skill files.
    /// Each file is parsed; malformed files are warned and skipped.
    /// Skills found here override any previously loaded skill with the same name.
    /// @param dir         Directory to scan (must contain .md files directly).
    /// @param source_tag  Value to assign to Skill::source for skills found here.
    void scan_dir(const std::filesystem::path& dir, std::string_view source_tag = "user-dir");

    /// Reload: clear all user-dir + plugin skills and re-run load_user_dirs().
    /// Bundled skills (set via set_bundled_skills) are preserved.
    void reload();

    // -------------------------------------------------------------------------
    // Query API
    // -------------------------------------------------------------------------

    /// Return a sorted list of all loaded skill names for autocomplete.
    [[nodiscard]] std::vector<std::string> names() const;

    /// Look up a skill by name.
    /// Returns nullptr if no skill with that name is loaded.
    [[nodiscard]] const Skill* find(std::string_view name) const;

    /// Invoke a named skill: returns its prompt_body as a ToolResult body.
    /// If the skill has a script_path, appends a note indicating the script
    /// path (actual execution is deferred to SkillTool in CPP 5.19).
    /// Returns Err if no skill with the given name is found.
    [[nodiscard]] Result<tools::ToolResult> run(std::string_view name,
                                                 tools::ToolContext& ctx);

    /// Return the total number of loaded skills.
    [[nodiscard]] std::size_t size() const noexcept { return skills_.size(); }

private:
    // -------------------------------------------------------------------------
    // Internal helpers
    // -------------------------------------------------------------------------

    /// Parse one .md file into a Skill struct.
    /// Returns the Skill on success, or std::nullopt on failure (caller logs).
    [[nodiscard]] std::optional<Skill> parse_skill_file(
        const std::filesystem::path& path,
        std::string_view             source_tag) const;

    /// Insert or replace a skill in skills_ by name (new entry wins).
    void upsert(Skill skill);

    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------

    /// Master map: skill name → Skill.
    /// Includes bundled, user-dir, and plugin skills merged together.
    std::unordered_map<std::string, Skill> skills_;

    /// The bundled base set preserved across reload() calls.
    std::vector<Skill> bundled_skills_;
};

} // namespace batbox::plugins
