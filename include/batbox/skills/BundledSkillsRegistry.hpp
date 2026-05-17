// include/batbox/skills/BundledSkillsRegistry.hpp
// =============================================================================
// batbox::skills::BundledSkillsRegistry — in-process registry of the 13
// bundled skills whose .md source files are embedded at CMake configure time
// via src/skills/embed.cmake.
//
// Usage (App::init):
//   skill_loader_.set_bundled_skills(batbox::skills::BundledSkillsRegistry::all());
//   skill_loader_.load_user_dirs();
//
// all() parses each embedded .md string with FrontmatterParser and returns
// a fully-populated vector<Skill>.  Skills with malformed frontmatter are
// logged as warnings and skipped (graceful degradation).
//
// The class is stateless — all() constructs the vector fresh on each call.
// In practice it is called once during App::init so there is no hot-path
// concern.
// =============================================================================

#pragma once

#include <batbox/plugins/SkillLoader.hpp>

#include <vector>

namespace batbox::skills {

/// Registry of the 13 bundled skills embedded at build time.
///
/// Typical use:
///   skill_loader_.set_bundled_skills(BundledSkillsRegistry::all());
class BundledSkillsRegistry {
public:
    /// Parse all 13 embedded skill .md strings and return them as Skill structs.
    ///
    /// Skills whose frontmatter fails to parse are logged as WARN and omitted.
    /// On a clean build all 13 should succeed; a truncated result indicates a
    /// malformed bundled .md file committed to the repo.
    ///
    /// The returned Skills have source == "bundled" so SkillLoader can
    /// distinguish them from user-dir and plugin-bundled skills.
    [[nodiscard]] static std::vector<batbox::plugins::Skill> all();
};

} // namespace batbox::skills
