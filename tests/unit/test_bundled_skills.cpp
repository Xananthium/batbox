// tests/unit/test_bundled_skills.cpp
// =============================================================================
// Unit tests for batbox::skills::BundledSkillsRegistry.
//
// Verifies that all 13 bundled skills are correctly parsed from embedded
// .md content and returned as populated Skill structs.
//
// Build standalone (from repo root):
//   cmake -S . -B build && cmake --build build --target test_bundled_skills
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/skills/BundledSkillsRegistry.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace batbox::skills;
using namespace batbox::plugins;

// ---------------------------------------------------------------------------
// Expected skill names — must match curated-surface.md exactly.
// ---------------------------------------------------------------------------
static const std::vector<std::string> kExpectedNames = {
    "verify",
    "debug",
    "simplify",
    "batch",
    "update-config",
    "remember",
    "getshitdone",
    "stuck",
    "verify-content",
    "dream",
    "hunter",
    "loop",
    "schedule-remote-agents",
};

TEST_SUITE("BundledSkillsRegistry") {

// ---------------------------------------------------------------------------
// Count: all() must return exactly 13 skills.
// ---------------------------------------------------------------------------
TEST_CASE("all() returns exactly 13 skills") {
    auto skills = BundledSkillsRegistry::all();
    CHECK(skills.size() == 13);
}

// ---------------------------------------------------------------------------
// All 13 expected names are present in the returned vector.
// ---------------------------------------------------------------------------
TEST_CASE("all 13 expected skill names are present") {
    auto skills = BundledSkillsRegistry::all();

    for (const auto& expected : kExpectedNames) {
        auto it = std::find_if(skills.begin(), skills.end(),
            [&](const Skill& s) { return s.name == expected; });
        CHECK_MESSAGE(it != skills.end(),
            "Missing skill: ", expected);
    }
}

// ---------------------------------------------------------------------------
// Every skill has non-empty name, description, and prompt_body.
// ---------------------------------------------------------------------------
TEST_CASE("every skill has non-empty name, description, and prompt_body") {
    auto skills = BundledSkillsRegistry::all();
    REQUIRE(!skills.empty());

    for (const auto& s : skills) {
        CHECK_MESSAGE(!s.name.empty(),
            "Empty name in skill");
        CHECK_MESSAGE(!s.description.empty(),
            "Empty description for skill: ", s.name);
        CHECK_MESSAGE(!s.prompt_body.empty(),
            "Empty prompt_body for skill: ", s.name);
    }
}

// ---------------------------------------------------------------------------
// Every skill's source is "bundled".
// ---------------------------------------------------------------------------
TEST_CASE("every skill has source == \"bundled\"") {
    auto skills = BundledSkillsRegistry::all();
    REQUIRE(!skills.empty());

    for (const auto& s : skills) {
        CHECK_MESSAGE(s.source == "bundled",
            "Wrong source for skill: ", s.name, " — got: ", s.source);
    }
}

// ---------------------------------------------------------------------------
// Every skill has at least one allowed_tool.
// ---------------------------------------------------------------------------
TEST_CASE("every skill has at least one allowed_tool") {
    auto skills = BundledSkillsRegistry::all();
    REQUIRE(!skills.empty());

    for (const auto& s : skills) {
        CHECK_MESSAGE(!s.allowed_tools.empty(),
            "No allowed_tools for skill: ", s.name);
    }
}

// ---------------------------------------------------------------------------
// No duplicate skill names.
// ---------------------------------------------------------------------------
TEST_CASE("no duplicate skill names") {
    auto skills = BundledSkillsRegistry::all();

    for (std::size_t i = 0; i < skills.size(); ++i) {
        for (std::size_t j = i + 1; j < skills.size(); ++j) {
            CHECK_MESSAGE(skills[i].name != skills[j].name,
                "Duplicate skill name: ", skills[i].name);
        }
    }
}

// ---------------------------------------------------------------------------
// Spot-check: verify skill has expected description substring.
// ---------------------------------------------------------------------------
TEST_CASE("verify skill has expected description") {
    auto skills = BundledSkillsRegistry::all();
    auto it = std::find_if(skills.begin(), skills.end(),
        [](const Skill& s) { return s.name == "verify"; });
    REQUIRE(it != skills.end());
    // Description should mention verifying a code change.
    CHECK(it->description.find("erif") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Spot-check: debug skill allows Bash tool.
// ---------------------------------------------------------------------------
TEST_CASE("debug skill allows Bash tool") {
    auto skills = BundledSkillsRegistry::all();
    auto it = std::find_if(skills.begin(), skills.end(),
        [](const Skill& s) { return s.name == "debug"; });
    REQUIRE(it != skills.end());
    auto& tools = it->allowed_tools;
    CHECK(std::find(tools.begin(), tools.end(), "Bash") != tools.end());
}

// ---------------------------------------------------------------------------
// Spot-check: remember skill allows Write tool.
// ---------------------------------------------------------------------------
TEST_CASE("remember skill allows Write tool") {
    auto skills = BundledSkillsRegistry::all();
    auto it = std::find_if(skills.begin(), skills.end(),
        [](const Skill& s) { return s.name == "remember"; });
    REQUIRE(it != skills.end());
    auto& tools = it->allowed_tools;
    CHECK(std::find(tools.begin(), tools.end(), "Write") != tools.end());
}

// ---------------------------------------------------------------------------
// Calling all() twice produces identical results (no mutable state).
// ---------------------------------------------------------------------------
TEST_CASE("all() is idempotent — calling twice gives same results") {
    auto first  = BundledSkillsRegistry::all();
    auto second = BundledSkillsRegistry::all();

    REQUIRE(first.size() == second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        CHECK(first[i].name        == second[i].name);
        CHECK(first[i].description == second[i].description);
        CHECK(first[i].source      == second[i].source);
        CHECK(first[i].prompt_body == second[i].prompt_body);
    }
}

} // TEST_SUITE("BundledSkillsRegistry")
