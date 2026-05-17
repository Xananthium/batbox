// tests/integration/test_skill_loader.cpp
// =============================================================================
// Integration tests for batbox::plugins::SkillLoader.
//
// Tests exercise the full parse-and-scan pipeline against real fixture files
// (tests/fixtures/skills/real_world_remember.md and real_world_debug.md) plus
// a synthetic temp directory created at runtime.
//
// Build standalone (use absolute source paths so __FILE__ resolves correctly):
//   ROOT=/path/to/batbox
//   c++ -std=c++20 \
//       -I $ROOT/include \
//       -I $ROOT/build/vcpkg_installed/arm64-osx/include \
//       $ROOT/tests/integration/test_skill_loader.cpp \
//       $ROOT/src/plugins/FrontmatterParser.cpp \
//       $ROOT/src/plugins/SkillLoader.cpp \
//       $ROOT/src/core/Logging.cpp \
//       $ROOT/src/core/Paths.cpp \
//       $ROOT/src/core/Json.cpp \
//       $ROOT/build/vcpkg_installed/arm64-osx/lib/libfmt.a \
//       $ROOT/build/vcpkg_installed/arm64-osx/lib/libspdlog.a \
//       $ROOT/build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       -o /tmp/test_skill_loader && /tmp/test_skill_loader
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/plugins/SkillLoader.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace batbox;
using namespace batbox::plugins;

namespace {

// ---------------------------------------------------------------------------
// project_root() — derive the repo root from this file's compile-time path.
// __FILE__ is absolute when compiled via CMake; walk up 3 levels:
//   tests/integration/test_skill_loader.cpp → tests/integration → tests → root
// ---------------------------------------------------------------------------
[[nodiscard]] std::filesystem::path project_root() {
    namespace fs = std::filesystem;
    return fs::path(__FILE__)
               .parent_path()  // tests/integration
               .parent_path()  // tests
               .parent_path(); // project root
}

// ---------------------------------------------------------------------------
// load_fixture_content — read a fixture file relative to the project root.
// Returns empty string if the file does not exist.
// ---------------------------------------------------------------------------
[[nodiscard]] std::string load_fixture(const std::string& rel_path) {
    namespace fs = std::filesystem;
    fs::path p = project_root() / rel_path;
    if (!fs::exists(p)) return {};
    std::ifstream f(p);
    if (!f) return {};
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    return content;
}

// ---------------------------------------------------------------------------
// TempSkillDir — RAII helper: creates a temp dir, populates .md files,
// removes everything on destruction.
// ---------------------------------------------------------------------------
struct TempSkillDir {
    std::filesystem::path path;

    explicit TempSkillDir() {
        namespace fs = std::filesystem;
        path = fs::temp_directory_path() / ("batbox_test_skills_" +
               std::to_string(std::hash<std::string>{}(__FILE__)));
        fs::create_directories(path);
    }

    ~TempSkillDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    // Write a .md file into this temp dir.
    void write(const std::string& filename, const std::string& content) {
        std::ofstream f(path / filename);
        f << content;
    }

    TempSkillDir(const TempSkillDir&) = delete;
    TempSkillDir& operator=(const TempSkillDir&) = delete;
};

} // anonymous namespace

// =============================================================================
// TEST SUITE
// =============================================================================

TEST_SUITE("SkillLoader") {

// ---------------------------------------------------------------------------
// Basic scan_dir — parse two fixture skills
// ---------------------------------------------------------------------------
TEST_CASE("scan_dir parses real-world fixture skills") {
    namespace fs = std::filesystem;

    fs::path fixtures = project_root() / "tests" / "fixtures" / "skills";
    REQUIRE(fs::exists(fixtures)); // fixtures dir must exist

    SkillLoader loader;
    loader.scan_dir(fixtures, "user-dir");

    auto all_names = loader.names();
    REQUIRE(!all_names.empty());

    // real_world_remember.md should be present
    REQUIRE(loader.find("remember") != nullptr);
    // real_world_debug.md should be present
    REQUIRE(loader.find("debug") != nullptr);
}

// ---------------------------------------------------------------------------
// Verify skill fields for real_world_remember.md
// ---------------------------------------------------------------------------
TEST_CASE("real_world_remember.md parses correctly") {
    namespace fs = std::filesystem;

    // Verify the fixture exists so we get a meaningful error if it doesn't.
    fs::path remember_path = project_root() / "tests" / "fixtures" / "skills"
                           / "real_world_remember.md";
    REQUIRE(fs::exists(remember_path));

    SkillLoader loader;
    loader.scan_dir(remember_path.parent_path(), "user-dir");

    const Skill* s = loader.find("remember");
    REQUIRE(s != nullptr);

    CHECK(s->name == "remember");
    CHECK(s->description == "Review memory and propose updates");
    CHECK(s->source == "user-dir");

    // allowed_tools should contain at least Read and Write
    auto& tools = s->allowed_tools;
    CHECK(std::find(tools.begin(), tools.end(), "Read")  != tools.end());
    CHECK(std::find(tools.begin(), tools.end(), "Write") != tools.end());

    // prompt_body must be non-empty (the markdown body after ---)
    CHECK(!s->prompt_body.empty());

    // model should be set
    CHECK(s->model.has_value());
    CHECK(s->model.value() == "claude-opus-4-5");
}

// ---------------------------------------------------------------------------
// Verify skill fields for real_world_debug.md (flow-style allowed_tools)
// ---------------------------------------------------------------------------
TEST_CASE("real_world_debug.md parses correctly (flow-style list)") {
    namespace fs = std::filesystem;

    fs::path debug_path = project_root() / "tests" / "fixtures" / "skills"
                        / "real_world_debug.md";
    REQUIRE(fs::exists(debug_path));

    SkillLoader loader;
    loader.scan_dir(debug_path.parent_path(), "user-dir");

    const Skill* s = loader.find("debug");
    REQUIRE(s != nullptr);

    CHECK(s->name == "debug");
    CHECK(s->description == "Systematic debugging workflow");

    // allowed_tools was a flow-style list: [Read, Write, Bash, Edit, LS, Glob]
    auto& tools = s->allowed_tools;
    CHECK(tools.size() == 6);
    CHECK(std::find(tools.begin(), tools.end(), "Bash")  != tools.end());
    CHECK(std::find(tools.begin(), tools.end(), "Edit")  != tools.end());

    CHECK(!s->prompt_body.empty());
}

// ---------------------------------------------------------------------------
// names() returns sorted list
// ---------------------------------------------------------------------------
TEST_CASE("names() returns sorted list") {
    TempSkillDir tmp;
    tmp.write("zzz.md", "---\nname: zzz\ndescription: last\n---\nBody Z\n");
    tmp.write("aaa.md", "---\nname: aaa\ndescription: first\n---\nBody A\n");
    tmp.write("mmm.md", "---\nname: mmm\ndescription: middle\n---\nBody M\n");

    SkillLoader loader;
    loader.scan_dir(tmp.path, "user-dir");

    auto n = loader.names();
    REQUIRE(n.size() == 3);
    CHECK(n[0] == "aaa");
    CHECK(n[1] == "mmm");
    CHECK(n[2] == "zzz");
}

// ---------------------------------------------------------------------------
// Deduplication: later scan_dir call overrides earlier for same name
// ---------------------------------------------------------------------------
TEST_CASE("scan_dir deduplication: later overrides earlier") {
    TempSkillDir dir_a;
    TempSkillDir dir_b;

    dir_a.write("skill.md",
        "---\nname: my-skill\ndescription: from-a\n---\nBody A\n");
    dir_b.write("skill.md",
        "---\nname: my-skill\ndescription: from-b\n---\nBody B\n");

    SkillLoader loader;
    loader.scan_dir(dir_a.path, "user-dir");
    loader.scan_dir(dir_b.path, "user-dir");

    REQUIRE(loader.size() == 1);
    const Skill* s = loader.find("my-skill");
    REQUIRE(s != nullptr);
    // dir_b was scanned second, so its description wins.
    CHECK(s->description == "from-b");
    CHECK(s->prompt_body == "Body B\n");
}

// ---------------------------------------------------------------------------
// set_bundled_skills: bundled is lowest priority
// ---------------------------------------------------------------------------
TEST_CASE("set_bundled_skills: user-dir overrides bundled") {
    TempSkillDir tmp;
    tmp.write("remember.md",
        "---\nname: remember\ndescription: user-override\n---\nUser body\n");

    Skill bundled;
    bundled.name        = "remember";
    bundled.description = "bundled version";
    bundled.prompt_body = "Bundled body";
    bundled.source      = "bundled";

    SkillLoader loader;
    // Set bundled first (lowest priority).
    loader.set_bundled_skills({bundled});
    // Then scan user dir (higher priority).
    loader.scan_dir(tmp.path, "user-dir");

    const Skill* s = loader.find("remember");
    REQUIRE(s != nullptr);
    // User-dir version must win.
    CHECK(s->description == "user-override");
    CHECK(s->source == "user-dir");
}

// ---------------------------------------------------------------------------
// set_bundled_skills: bundled skill visible when no user override
// ---------------------------------------------------------------------------
TEST_CASE("set_bundled_skills: bundled skill visible when no user override") {
    Skill bundled;
    bundled.name        = "only-bundled";
    bundled.description = "only from bundled";
    bundled.prompt_body = "Bundled body";
    bundled.source      = "bundled";

    SkillLoader loader;
    loader.set_bundled_skills({bundled});

    const Skill* s = loader.find("only-bundled");
    REQUIRE(s != nullptr);
    CHECK(s->source == "bundled");
}

// ---------------------------------------------------------------------------
// run() returns prompt body
// ---------------------------------------------------------------------------
TEST_CASE("run() returns skill prompt body as ToolResult") {
    TempSkillDir tmp;
    tmp.write("greet.md",
        "---\nname: greet\ndescription: a greeting skill\n---\nHello, world!\n");

    SkillLoader loader;
    loader.scan_dir(tmp.path, "user-dir");

    tools::ToolContext ctx;
    auto result = loader.run("greet", ctx);
    REQUIRE(result.has_value());
    CHECK(!result->is_error);
    CHECK(result->body == "Hello, world!\n");
}

// ---------------------------------------------------------------------------
// run() for unknown skill returns Err
// ---------------------------------------------------------------------------
TEST_CASE("run() for unknown skill returns Err") {
    SkillLoader loader;
    tools::ToolContext ctx;
    auto result = loader.run("nonexistent-skill", ctx);
    CHECK(!result.has_value());
    CHECK(result.error().find("nonexistent-skill") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Malformed file: missing name field is silently skipped
// ---------------------------------------------------------------------------
TEST_CASE("malformed file missing 'name' field is skipped") {
    TempSkillDir tmp;
    tmp.write("no_name.md",
        "---\ndescription: no name here\n---\nBody\n");
    tmp.write("valid.md",
        "---\nname: valid-skill\ndescription: has name\n---\nBody\n");

    SkillLoader loader;
    loader.scan_dir(tmp.path, "user-dir");

    // Only the valid skill should be loaded.
    CHECK(loader.size() == 1);
    CHECK(loader.find("valid-skill") != nullptr);
}

// ---------------------------------------------------------------------------
// Malformed frontmatter (no closing ---) is silently skipped
// ---------------------------------------------------------------------------
TEST_CASE("malformed frontmatter (no closing ---) is skipped") {
    TempSkillDir tmp;
    tmp.write("broken.md",
        "---\nname: broken\ndescription: no closing delimiter\n");
    tmp.write("good.md",
        "---\nname: good\ndescription: fine\n---\nBody\n");

    SkillLoader loader;
    loader.scan_dir(tmp.path, "user-dir");

    CHECK(loader.find("broken") == nullptr);
    CHECK(loader.find("good") != nullptr);
}

// ---------------------------------------------------------------------------
// File without frontmatter (no leading ---) is treated as body-only skill
// with no name → skipped
// ---------------------------------------------------------------------------
TEST_CASE("file with no frontmatter is skipped (no name field)") {
    TempSkillDir tmp;
    tmp.write("no_fm.md", "# No Frontmatter\n\nJust raw markdown.\n");

    SkillLoader loader;
    loader.scan_dir(tmp.path, "user-dir");

    // no_fm.md has no name → must be skipped
    CHECK(loader.size() == 0);
}

// ---------------------------------------------------------------------------
// Non-.md files in the skills dir are ignored
// ---------------------------------------------------------------------------
TEST_CASE("non-.md files are ignored during scan") {
    TempSkillDir tmp;
    tmp.write("script.sh",    "#!/bin/bash\necho hi\n");
    tmp.write("notes.txt",    "some notes\n");
    tmp.write("skill.md",     "---\nname: one\ndescription: one\n---\nBody\n");

    SkillLoader loader;
    loader.scan_dir(tmp.path, "user-dir");

    CHECK(loader.size() == 1);
    CHECK(loader.find("one") != nullptr);
}

// ---------------------------------------------------------------------------
// scan_dir with a non-existent path is silently ignored
// ---------------------------------------------------------------------------
TEST_CASE("scan_dir with non-existent directory is silently ignored") {
    SkillLoader loader;
    // Should not throw or crash.
    loader.scan_dir("/tmp/batbox_definitely_does_not_exist_xyz789", "user-dir");
    CHECK(loader.size() == 0);
}

// ---------------------------------------------------------------------------
// source_tag is propagated correctly
// ---------------------------------------------------------------------------
TEST_CASE("source_tag is set on loaded skills") {
    TempSkillDir tmp;
    tmp.write("s.md", "---\nname: my-tagged\ndescription: tagged\n---\nBody\n");

    SkillLoader loader;
    loader.scan_dir(tmp.path, "plugin:my-plugin");

    const Skill* s = loader.find("my-tagged");
    REQUIRE(s != nullptr);
    CHECK(s->source == "plugin:my-plugin");
}

// ---------------------------------------------------------------------------
// reload() clears user-dir skills and re-applies bundled
// ---------------------------------------------------------------------------
TEST_CASE("reload() clears user-dir skills, preserves bundled base") {
    TempSkillDir tmp;
    tmp.write("transient.md",
        "---\nname: transient\ndescription: will disappear\n---\nBody\n");

    Skill bundled;
    bundled.name        = "persistent";
    bundled.description = "always here";
    bundled.prompt_body = "Persistent body";
    bundled.source      = "bundled";

    SkillLoader loader;
    loader.set_bundled_skills({bundled});
    loader.scan_dir(tmp.path, "user-dir");

    REQUIRE(loader.find("transient")  != nullptr);
    REQUIRE(loader.find("persistent") != nullptr);

    // After reload(), load_user_dirs() is called but the temp dir isn't in the
    // standard roots, so "transient" will disappear; "persistent" stays.
    loader.reload();

    CHECK(loader.find("persistent") != nullptr);
    // "transient" was only in the temp dir (not a standard root) — gone after reload.
    CHECK(loader.find("transient") == nullptr);
}

// ---------------------------------------------------------------------------
// size() reflects count
// ---------------------------------------------------------------------------
TEST_CASE("size() reflects total loaded skill count") {
    TempSkillDir tmp;
    for (int i = 0; i < 5; ++i) {
        std::string name = "skill" + std::to_string(i);
        tmp.write(name + ".md",
            "---\nname: " + name + "\ndescription: d\n---\nBody\n");
    }

    SkillLoader loader;
    loader.scan_dir(tmp.path, "user-dir");
    CHECK(loader.size() == 5);
}

} // TEST_SUITE("SkillLoader")
