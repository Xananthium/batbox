// tests/unit/test_bundled_skills_registry.cpp
// =============================================================================
// PEXT 3.3 — AUDIT-3: BundledSkillsRegistry sanity test
//
// Verifies that the 3 agentic bundled skills (hunter, loop,
// schedule-remote-agents) are correctly embedded and parseable.
//
// Design:
//   Uses BundledSkillsRegistry::all() (the only public API — no get() method)
//   and searches for each named skill.  Checks are length + frontmatter-sentinel
//   (non-empty description proves frontmatter was parsed) — no hardcoded content
//   strings.
//
// Regression intent (per ned-post-extraction.md §5):
//   If embed.cmake ever stops embedding one of these 3 skills, or if their .md
//   files lose their frontmatter, this test fires.
//
// Build:
//   cmake --build build --target test_bundled_skills_registry
// Run:
//   ctest -R bundled_skills_registry
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/skills/BundledSkillsRegistry.hpp>

#include <algorithm>
#include <array>
#include <string>

using namespace batbox::skills;
using namespace batbox::plugins;

// ---------------------------------------------------------------------------
// Helper: find a skill by name in the all() result.
// Returns a pointer into the vector or nullptr if not found.
// The vector is passed by reference so the pointer remains valid.
// ---------------------------------------------------------------------------
static const Skill* find_skill(const std::vector<Skill>& skills,
                                const std::string& name)
{
    auto it = std::find_if(skills.begin(), skills.end(),
                           [&](const Skill& s) { return s.name == name; });
    return (it != skills.end()) ? &(*it) : nullptr;
}

// ---------------------------------------------------------------------------
// Primary sanity test for the 3 agentic bundled skills.
// ---------------------------------------------------------------------------
TEST_CASE("BundledSkillsRegistry exposes 3 bundled skills") {

    auto skills = BundledSkillsRegistry::all();

    // The registry embeds 13 bundled skills total; the 3 agentic skills
    // (hunter, loop, schedule-remote-agents) must be among them.
    REQUIRE(skills.size() >= 3);

    // Names of the 3 agentic skills to verify.
    constexpr std::array<const char*, 3> kAgenticSkills = {
        "hunter",
        "loop",
        "schedule-remote-agents",
    };

    for (const char* name : kAgenticSkills) {
        INFO("Checking skill: " << name);

        const Skill* sk = find_skill(skills, name);

        // Skill must be present in the registry.
        REQUIRE_MESSAGE(sk != nullptr,
            "Skill not found in BundledSkillsRegistry::all(): ", name);

        // prompt_body must be substantial (> 100 bytes).
        // This fires if embed.cmake truncates or empties the .md file.
        CHECK_MESSAGE(sk->prompt_body.size() > 100,
            "prompt_body too short for skill: ", name,
            " (size=", sk->prompt_body.size(), ")");

        // Frontmatter was parsed: description must be non-empty.
        // A missing or blank description means the YAML header was lost or
        // malformed, which would also prevent the skill from working at runtime.
        CHECK_MESSAGE(!sk->description.empty(),
            "Empty description for skill: ", name,
            " — frontmatter may be malformed");

        // source must be "bundled" — distinguishes embedded skills from
        // user-dir or plugin-bundled skills.
        CHECK_MESSAGE(sk->source == "bundled",
            "Wrong source for skill: ", name,
            " — got: '", sk->source, "'");
    }
}
