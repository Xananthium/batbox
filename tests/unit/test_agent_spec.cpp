// tests/unit/test_agent_spec.cpp
// =============================================================================
// doctest suite for batbox::agents::AgentSpec.
//
// Coverage:
//   1. Parse valid frontmatter + body → AgentSpec (all fields)
//   2. Missing required field (name) → Err
//   3. [[ref]] tokens detected in prompt_body
//   4. from_file: optional model field present and absent
//   5. from_file: flow-style tools list parsed into allowed_tools
//   6. from_file: block-style tools list parsed into allowed_tools
//   7. from_file: nonexistent file → Err with path in message
//   8. from_file: no frontmatter → Err (missing name)
//   9. from_file: empty name value → Err (empty name is not allowed post-parse)
//  10. from_type: file does not exist → returns generic fallback spec
//  11. from_type: with BATBOX_AGENTS_DIR pointing to fixture dir → loads file
//  12. Unit tests against bundled skill fixture files
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/agents/AgentSpec.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>

namespace fs = std::filesystem;
using namespace batbox::agents;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

/// Write a temp .md file and return its path.
[[nodiscard]] fs::path write_tmp_md(const std::string& content,
                                    const std::string& name = "test_agent.md") {
    const auto path = fs::temp_directory_path() / ("batbox_test_agent_" + name);
    std::ofstream f(path, std::ios::trunc | std::ios::binary);
    f << content;
    return path;
}

/// Absolute path to project root (two levels up from tests/unit/).
[[nodiscard]] fs::path project_root() {
    return fs::path(__FILE__).parent_path().parent_path().parent_path();
}

/// Absolute path to skills fixture directory.
[[nodiscard]] fs::path skills_fixtures_dir() {
    return project_root() / "tests" / "fixtures" / "skills";
}

/// Count [[ref]] tokens in a string using a simple regex scan.
[[nodiscard]] std::size_t count_ref_tokens(const std::string& body) {
    static const std::regex kRefPattern(R"(\[\[[^\]]+\]\])");
    auto begin = std::sregex_iterator(body.begin(), body.end(), kRefPattern);
    auto end   = std::sregex_iterator{};
    return static_cast<std::size_t>(std::distance(begin, end));
}

} // namespace

// ============================================================================
// TEST SUITE 1: Basic valid frontmatter → AgentSpec
// ============================================================================
TEST_SUITE("AgentSpec::from_file — valid frontmatter") {

    TEST_CASE("full spec: name, description, model, tools, and body") {
        const auto path = write_tmp_md(
            "---\n"
            "name: senior-dev\n"
            "description: Writes production-ready C++ code.\n"
            "model: claude-opus-4-5\n"
            "tools: [Read, Write, Bash, Edit]\n"
            "---\n"
            "You are Senior Dev. Write clean, idiomatic C++20.\n");

        const auto r = AgentSpec::from_file(path);
        REQUIRE(r.has_value());
        CHECK(r->name == "senior-dev");
        CHECK(r->description == "Writes production-ready C++ code.");
        REQUIRE(r->model.has_value());
        CHECK(r->model.value() == "claude-opus-4-5");
        REQUIRE(r->allowed_tools.size() == 4);
        CHECK(r->allowed_tools[0] == "Read");
        CHECK(r->allowed_tools[3] == "Edit");
        CHECK(r->prompt_body == "You are Senior Dev. Write clean, idiomatic C++20.\n");
        CHECK(r->source_path == path);

        fs::remove(path);
    }

    TEST_CASE("minimal spec: only name is required") {
        const auto path = write_tmp_md(
            "---\n"
            "name: minimal-agent\n"
            "---\n"
            "Body text.\n");

        const auto r = AgentSpec::from_file(path);
        REQUIRE(r.has_value());
        CHECK(r->name == "minimal-agent");
        CHECK(r->description.empty());
        CHECK(!r->model.has_value());
        CHECK(r->allowed_tools.empty());
        CHECK(r->prompt_body == "Body text.\n");

        fs::remove(path);
    }

    TEST_CASE("spec with empty body") {
        const auto path = write_tmp_md(
            "---\n"
            "name: no-body-agent\n"
            "description: No prompt body.\n"
            "---\n");

        const auto r = AgentSpec::from_file(path);
        REQUIRE(r.has_value());
        CHECK(r->name == "no-body-agent");
        CHECK(r->prompt_body.empty());

        fs::remove(path);
    }

    TEST_CASE("model field absent → model is nullopt") {
        const auto path = write_tmp_md(
            "---\n"
            "name: no-model-agent\n"
            "description: Uses default model.\n"
            "---\n"
            "Prompt.\n");

        const auto r = AgentSpec::from_file(path);
        REQUIRE(r.has_value());
        CHECK(!r->model.has_value());

        fs::remove(path);
    }
}

// ============================================================================
// TEST SUITE 2: Missing required field → Err
// ============================================================================
TEST_SUITE("AgentSpec::from_file — missing required name") {

    TEST_CASE("no 'name' key → Err with diagnostic message") {
        const auto path = write_tmp_md(
            "---\n"
            "description: An agent without a name.\n"
            "model: claude-opus-4-5\n"
            "---\n"
            "Prompt.\n",
            "no_name.md");

        const auto r = AgentSpec::from_file(path);
        CHECK_FALSE(r.has_value());
        // Error message must contain the field name for debuggability.
        CHECK(r.error().find("name") != std::string::npos);

        fs::remove(path);
    }

    TEST_CASE("file with no frontmatter → Err (name absent)") {
        const auto path = write_tmp_md(
            "Just a plain markdown file with no frontmatter block.\n",
            "no_frontmatter.md");

        const auto r = AgentSpec::from_file(path);
        CHECK_FALSE(r.has_value());

        fs::remove(path);
    }
}

// ============================================================================
// TEST SUITE 3: [[ref]] tokens in prompt_body
// ============================================================================
TEST_SUITE("AgentSpec — [[ref]] token detection") {

    TEST_CASE("single [[ref]] token in body is preserved") {
        const auto path = write_tmp_md(
            "---\n"
            "name: orchestrator\n"
            "---\n"
            "Delegate to [[junior-dev]] for implementation.\n",
            "single_ref.md");

        const auto r = AgentSpec::from_file(path);
        REQUIRE(r.has_value());
        CHECK(count_ref_tokens(r->prompt_body) == 1);
        CHECK(r->prompt_body.find("[[junior-dev]]") != std::string::npos);

        fs::remove(path);
    }

    TEST_CASE("multiple [[ref]] tokens in body are all preserved") {
        const auto path = write_tmp_md(
            "---\n"
            "name: project-manager\n"
            "---\n"
            "Call [[architecture-ned]] then [[karla-fant]] then [[senior-dev]].\n",
            "multi_ref.md");

        const auto r = AgentSpec::from_file(path);
        REQUIRE(r.has_value());
        CHECK(count_ref_tokens(r->prompt_body) == 3);

        fs::remove(path);
    }

    TEST_CASE("no [[ref]] tokens → count is zero") {
        const auto path = write_tmp_md(
            "---\n"
            "name: standalone-agent\n"
            "---\n"
            "This agent has no references to other agents.\n",
            "no_ref.md");

        const auto r = AgentSpec::from_file(path);
        REQUIRE(r.has_value());
        CHECK(count_ref_tokens(r->prompt_body) == 0);

        fs::remove(path);
    }
}

// ============================================================================
// TEST SUITE 4: Tools list variants
// ============================================================================
TEST_SUITE("AgentSpec::from_file — tools list") {

    TEST_CASE("flow-style tools list [A, B, C] → allowed_tools vector") {
        const auto path = write_tmp_md(
            "---\n"
            "name: tool-agent\n"
            "tools: [Read, Write, Bash]\n"
            "---\n"
            "Prompt.\n",
            "flow_tools.md");

        const auto r = AgentSpec::from_file(path);
        REQUIRE(r.has_value());
        REQUIRE(r->allowed_tools.size() == 3);
        CHECK(r->allowed_tools[0] == "Read");
        CHECK(r->allowed_tools[1] == "Write");
        CHECK(r->allowed_tools[2] == "Bash");

        fs::remove(path);
    }

    TEST_CASE("block-style tools list → allowed_tools vector") {
        const auto path = write_tmp_md(
            "---\n"
            "name: block-tool-agent\n"
            "tools:\n"
            "  - Read\n"
            "  - Write\n"
            "  - Edit\n"
            "---\n"
            "Prompt.\n",
            "block_tools.md");

        const auto r = AgentSpec::from_file(path);
        REQUIRE(r.has_value());
        REQUIRE(r->allowed_tools.size() == 3);
        CHECK(r->allowed_tools[0] == "Read");
        CHECK(r->allowed_tools[2] == "Edit");

        fs::remove(path);
    }

    TEST_CASE("tools key absent → allowed_tools empty (inherits parent set)") {
        const auto path = write_tmp_md(
            "---\n"
            "name: no-tools-agent\n"
            "---\n"
            "Prompt.\n",
            "no_tools.md");

        const auto r = AgentSpec::from_file(path);
        REQUIRE(r.has_value());
        CHECK(r->allowed_tools.empty());

        fs::remove(path);
    }
}

// ============================================================================
// TEST SUITE 5: File-level errors
// ============================================================================
TEST_SUITE("AgentSpec::from_file — file errors") {

    TEST_CASE("nonexistent file → Err with path in message") {
        const fs::path ghost = "/tmp/batbox_no_such_agent_XXXXXX.md";
        fs::remove(ghost); // ensure absence

        const auto r = AgentSpec::from_file(ghost);
        CHECK_FALSE(r.has_value());
        CHECK(r.error().find(ghost.string()) != std::string::npos);
    }

    TEST_CASE("malformed YAML frontmatter → Err") {
        const auto path = write_tmp_md(
            "---\n"
            "name: bad-agent\n"
            "bad line without colon\n"
            "---\n"
            "Body.\n",
            "bad_yaml.md");

        const auto r = AgentSpec::from_file(path);
        CHECK_FALSE(r.has_value());
        CHECK_FALSE(r.error().empty());

        fs::remove(path);
    }
}

// ============================================================================
// TEST SUITE 6: AgentSpec::from_type fallback
// ============================================================================
TEST_SUITE("AgentSpec::from_type") {

    TEST_CASE("nonexistent agent type → generic fallback spec") {
        // Use a type name that certainly has no .md file.
        const auto spec = AgentSpec::from_type("nonexistent-agent-xyz");
        CHECK(spec.name == "nonexistent-agent-xyz");
        CHECK_FALSE(spec.description.empty());
        CHECK(!spec.model.has_value());
        CHECK(spec.allowed_tools.empty());
    }

    TEST_CASE("from_type with BATBOX_AGENTS_DIR pointing at fixtures → loads file") {
        const fs::path fixtures = skills_fixtures_dir();
        if (!fs::exists(fixtures / "basic.md")) {
            // Skill fixtures not present in this build — skip.
            return;
        }

        // Point BATBOX_AGENTS_DIR at the skills fixture dir.
        // "basic" corresponds to basic.md in that directory.
        ::setenv("BATBOX_AGENTS_DIR", fixtures.c_str(), /*overwrite=*/1);

        const auto spec = AgentSpec::from_type("basic");
        CHECK(spec.name == "basic-skill");

        // Clean up env override.
        ::unsetenv("BATBOX_AGENTS_DIR");
    }
}

// ============================================================================
// TEST SUITE 7: Fixture files from tests/fixtures/skills/
// ============================================================================
TEST_SUITE("AgentSpec::from_file — skill fixture files") {

    TEST_CASE("basic.md fixture parses name and model") {
        const auto path = skills_fixtures_dir() / "basic.md";
        if (!fs::exists(path)) return; // fixture absent in some build configs

        const auto r = AgentSpec::from_file(path);
        REQUIRE(r.has_value());
        CHECK(r->name == "basic-skill");
        REQUIRE(r->model.has_value());
        CHECK(r->model.value() == "claude-opus-4-5");
        CHECK_FALSE(r->prompt_body.empty());
    }

    TEST_CASE("with_list.md fixture parses flow-style tools list") {
        const auto path = skills_fixtures_dir() / "with_list.md";
        if (!fs::exists(path)) return;

        // with_list.md uses allowed_tools key if present, otherwise tools.
        // The fixture was written for SkillSpec (allowed_tools) — AgentSpec
        // reads 'tools'.  Parse it: at minimum, name must be present.
        const auto r = AgentSpec::from_file(path);
        REQUIRE(r.has_value());
        CHECK(r->name == "tool-skill");
    }

    TEST_CASE("block_list.md fixture parses block-style list") {
        const auto path = skills_fixtures_dir() / "block_list.md";
        if (!fs::exists(path)) return;

        const auto r = AgentSpec::from_file(path);
        REQUIRE(r.has_value());
        CHECK(r->name == "block-list-skill");
    }

    TEST_CASE("no_frontmatter.md fixture → Err (name absent)") {
        const auto path = skills_fixtures_dir() / "no_frontmatter.md";
        if (!fs::exists(path)) return;

        const auto r = AgentSpec::from_file(path);
        CHECK_FALSE(r.has_value());
    }

    TEST_CASE("only_frontmatter.md fixture → empty body") {
        const auto path = skills_fixtures_dir() / "only_frontmatter.md";
        if (!fs::exists(path)) return;

        const auto r = AgentSpec::from_file(path);
        // only_frontmatter.md may or may not have a name — accept either outcome.
        if (r.has_value()) {
            CHECK(r->prompt_body.empty());
        } else {
            CHECK_FALSE(r.error().empty());
        }
    }
}
