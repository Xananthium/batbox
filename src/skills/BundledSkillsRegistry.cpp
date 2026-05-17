// src/skills/BundledSkillsRegistry.cpp
// =============================================================================
// Implementation of batbox::skills::BundledSkillsRegistry.
//
// The 13 bundled skill .md files are embedded as C++ raw string literals by
// the CMake helper src/skills/embed.cmake, which generates:
//
//   ${CMAKE_BINARY_DIR}/generated/include/batbox/skills/skills_embed.inc
//
// That file defines an anonymous-namespace constexpr array k_bundled_skills[]
// of BundledSkillEntry { const char* name; const char* md_content; }.
// all() iterates that array, parses each entry with FrontmatterParser, and
// returns a populated vector<Skill>.
// =============================================================================

#include <batbox/skills/BundledSkillsRegistry.hpp>
#include <batbox/plugins/FrontmatterParser.hpp>
#include <batbox/core/Logging.hpp>

// Auto-generated embed — produced by batbox_generate_skills_embed() in
// src/skills/embed.cmake.  The include path is set up by the target_include_
// directories() call in src/skills/CMakeLists.txt.
#include <batbox/skills/skills_embed.inc>

namespace batbox::skills {

std::vector<batbox::plugins::Skill> BundledSkillsRegistry::all() {
    std::vector<batbox::plugins::Skill> result;
    result.reserve(13);

    for (const auto& entry : k_bundled_skills) {
        auto parsed = batbox::plugins::parse_frontmatter(entry.md_content);
        if (!parsed.has_value()) {
            BATBOX_LOG_WARN("BundledSkillsRegistry: frontmatter parse error "
                            "for skill '{}': {}",
                            entry.name, parsed.error());
            continue;
        }

        auto& [fm, body] = parsed.value();

        batbox::plugins::Skill skill;
        skill.name        = entry.name;
        skill.source      = "bundled";
        skill.prompt_body = body;

        // description — required by convention; warn but don't skip if absent.
        if (auto it = fm.find("description");
            it != fm.end() && it->second.is_string()) {
            skill.description = it->second.get<std::string>();
        } else {
            BATBOX_LOG_WARN("BundledSkillsRegistry: skill '{}' has no "
                            "'description' frontmatter key",
                            entry.name);
        }

        // model — optional per-skill model override.
        if (auto it = fm.find("model");
            it != fm.end() && it->second.is_string()) {
            skill.model = it->second.get<std::string>();
        }

        // allowed_tools — optional list; empty means "no restriction".
        if (auto it = fm.find("allowed_tools");
            it != fm.end() && it->second.is_array()) {
            for (const auto& t : it->second) {
                if (t.is_string()) {
                    skill.allowed_tools.push_back(t.get<std::string>());
                }
            }
        }

        result.push_back(std::move(skill));
    }

    return result;
}

} // namespace batbox::skills
