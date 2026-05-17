// tests/unit/test_system_prompt.cpp
// =============================================================================
// Unit tests for batbox::conversation::compose_system_prompt (CPP 3.8).
//
// Build + run (standalone, from repo root):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_system_prompt.cpp \
//       src/conversation/SystemPrompt.cpp \
//       src/core/Paths.cpp \
//       -o /tmp/test_system_prompt && /tmp/test_system_prompt
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/conversation/SystemPrompt.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

// ---------------------------------------------------------------------------
// Test infrastructure
// ---------------------------------------------------------------------------
namespace {
namespace fs = std::filesystem;

// RAII guard: sets an environment variable for the test lifetime, then restores.
struct EnvGuard {
    std::string key_;
    bool        had_value_;
    std::string old_value_;

    EnvGuard(const char* k, const char* v) : key_{k} {
        const char* prev = std::getenv(k);
        had_value_ = prev != nullptr;
        if (had_value_) old_value_ = prev;
        ::setenv(k, v, /*overwrite=*/1);
    }
    ~EnvGuard() {
        if (had_value_) ::setenv(key_.c_str(), old_value_.c_str(), 1);
        else            ::unsetenv(key_.c_str());
    }
    EnvGuard(const EnvGuard&)            = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;
};

// RAII temporary directory: creates it on construction, removes it on destruction.
struct TempDir {
    fs::path path;

    explicit TempDir(const std::string& suffix = "") {
        path = fs::temp_directory_path()
               / ("batbox_sysprompt_test_" + suffix
                  + std::to_string(std::hash<std::string>{}(suffix
                    + std::to_string(reinterpret_cast<uintptr_t>(this)))));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;

    // Write a file inside this temp dir.
    fs::path write(const std::string& name, const std::string& content) const {
        const fs::path p = path / name;
        fs::create_directories(p.parent_path());
        std::ofstream f{p};
        f << content;
        return p;
    }
};

// Verify that `haystack` contains `needle`.
bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// Verify position: returns true if first appears before second in haystack.
bool before(const std::string& haystack,
            const std::string& first,
            const std::string& second) {
    const auto p1 = haystack.find(first);
    const auto p2 = haystack.find(second);
    return p1 != std::string::npos && p2 != std::string::npos && p1 < p2;
}

} // anonymous namespace

// =============================================================================
// Acceptance criterion: No BATBOX.md -> base prompt only
// =============================================================================
TEST_SUITE("no BATBOX.md") {
    TEST_CASE("base prompt is always present") {
        TempDir home{"no_batbox_home"};
        TempDir work{"no_batbox_work"};

        // Point config_dir to a dir with no BATBOX.md.
        EnvGuard cfg{"BATBOX_CONFIG_DIR", home.path.string().c_str()};
        // working_dir is also empty — no project BATBOX.md.

        const std::string sp =
            batbox::conversation::compose_system_prompt(false, work.path);

        // Base prompt must be present.
        CHECK(contains(sp, batbox::conversation::k_base_system_prompt));
        // Plan-mode prefix must NOT be present.
        CHECK_FALSE(contains(sp, "PLAN MODE"));
    }

    TEST_CASE("no extra separators when only base is present") {
        TempDir home{"no_batbox_sep_home"};
        TempDir work{"no_batbox_sep_work"};
        EnvGuard cfg{"BATBOX_CONFIG_DIR", home.path.string().c_str()};

        const std::string sp =
            batbox::conversation::compose_system_prompt(false, work.path);

        // Should not start or end with blank lines (no leading/trailing padding).
        CHECK_FALSE(sp.empty());
        CHECK(sp.front() != '\n');
    }
}

// =============================================================================
// Acceptance criterion: Project BATBOX.md -> appended after base
// =============================================================================
TEST_SUITE("project BATBOX.md") {
    TEST_CASE("project BATBOX.md content appears in output") {
        TempDir home{"proj_batbox_home"};
        TempDir work{"proj_batbox_work"};
        EnvGuard cfg{"BATBOX_CONFIG_DIR", home.path.string().c_str()};

        // Write a project BATBOX.md directly in working_dir.
        work.write("BATBOX.md", "# Project Instructions\nUse snake_case.\n");

        const std::string sp =
            batbox::conversation::compose_system_prompt(false, work.path);

        CHECK(contains(sp, "Project Instructions"));
        CHECK(contains(sp, "Use snake_case."));
    }

    TEST_CASE("project BATBOX.md appears after base prompt") {
        TempDir home{"proj_order_home"};
        TempDir work{"proj_order_work"};
        EnvGuard cfg{"BATBOX_CONFIG_DIR", home.path.string().c_str()};

        work.write("BATBOX.md", "PROJECT_MARKER_XYZ\n");

        const std::string sp =
            batbox::conversation::compose_system_prompt(false, work.path);

        CHECK(before(sp, "BatBox", "PROJECT_MARKER_XYZ"));
    }
}

// =============================================================================
// Acceptance criterion: Both -> user first, then project
// =============================================================================
TEST_SUITE("both user and project BATBOX.md") {
    TEST_CASE("user BATBOX.md appears before project BATBOX.md") {
        TempDir home{"both_home"};
        TempDir work{"both_work"};
        EnvGuard cfg{"BATBOX_CONFIG_DIR", home.path.string().c_str()};

        // User-wide BATBOX.md.
        home.write("BATBOX.md", "USER_MARKER_ALPHA\n");
        // Project BATBOX.md.
        work.write("BATBOX.md", "PROJECT_MARKER_BETA\n");

        const std::string sp =
            batbox::conversation::compose_system_prompt(false, work.path);

        CHECK(contains(sp, "USER_MARKER_ALPHA"));
        CHECK(contains(sp, "PROJECT_MARKER_BETA"));
        // User layer (layer 2) must precede project layer (layer 3).
        CHECK(before(sp, "USER_MARKER_ALPHA", "PROJECT_MARKER_BETA"));
    }

    TEST_CASE("base prompt appears before both user and project") {
        TempDir home{"both_order_home"};
        TempDir work{"both_order_work"};
        EnvGuard cfg{"BATBOX_CONFIG_DIR", home.path.string().c_str()};

        home.write("BATBOX.md", "USER_SECTION\n");
        work.write("BATBOX.md", "PROJECT_SECTION\n");

        const std::string sp =
            batbox::conversation::compose_system_prompt(false, work.path);

        CHECK(before(sp, "BatBox", "USER_SECTION"));
        CHECK(before(sp, "BatBox", "PROJECT_SECTION"));
    }
}

// =============================================================================
// Acceptance criterion: @import file.md single-level resolved
// =============================================================================
TEST_SUITE("@import single-level") {
    TEST_CASE("@import line is replaced by imported file content") {
        TempDir home{"import_home"};
        TempDir work{"import_work"};
        EnvGuard cfg{"BATBOX_CONFIG_DIR", home.path.string().c_str()};

        // Write the imported file.
        work.write("extra.md", "IMPORTED_CONTENT_HERE\n");
        // Write project BATBOX.md that imports it.
        work.write("BATBOX.md", "Before import.\n@import extra.md\nAfter import.\n");

        const std::string sp =
            batbox::conversation::compose_system_prompt(false, work.path);

        // @import line itself should NOT appear verbatim.
        CHECK_FALSE(contains(sp, "@import extra.md"));
        // Imported content must be spliced in.
        CHECK(contains(sp, "IMPORTED_CONTENT_HERE"));
        // Surrounding context must still be present.
        CHECK(contains(sp, "Before import."));
        CHECK(contains(sp, "After import."));
    }

    TEST_CASE("import order preserved: before-import content before imported content") {
        TempDir home{"import_order_home"};
        TempDir work{"import_order_work"};
        EnvGuard cfg{"BATBOX_CONFIG_DIR", home.path.string().c_str()};

        work.write("child.md", "CHILD_CONTENT\n");
        work.write("BATBOX.md", "BEFORE_IMPORT\n@import child.md\nAFTER_IMPORT\n");

        const std::string sp =
            batbox::conversation::compose_system_prompt(false, work.path);

        CHECK(before(sp, "BEFORE_IMPORT", "CHILD_CONTENT"));
        CHECK(before(sp, "CHILD_CONTENT", "AFTER_IMPORT"));
    }

    TEST_CASE("missing @import target is silently skipped") {
        TempDir home{"import_missing_home"};
        TempDir work{"import_missing_work"};
        EnvGuard cfg{"BATBOX_CONFIG_DIR", home.path.string().c_str()};

        work.write("BATBOX.md", "BEFORE\n@import does_not_exist.md\nAFTER\n");

        std::string sp;
        CHECK_NOTHROW(sp = batbox::conversation::compose_system_prompt(false, work.path));
        CHECK(contains(sp, "BEFORE"));
        CHECK(contains(sp, "AFTER"));
        // No crash, no partial output.
        CHECK_FALSE(sp.empty());
    }

    TEST_CASE("imported file content is NOT recursively re-scanned for @import") {
        TempDir home{"import_norecu_home"};
        TempDir work{"import_norecu_work"};
        EnvGuard cfg{"BATBOX_CONFIG_DIR", home.path.string().c_str()};

        // grandchild.md: would be imported if recursion happened.
        work.write("grandchild.md", "GRANDCHILD_CONTENT\n");
        // child.md: contains @import grandchild.md.
        work.write("child.md", "CHILD_BEFORE\n@import grandchild.md\nCHILD_AFTER\n");
        // BATBOX.md: imports child.md (single-level).
        work.write("BATBOX.md", "@import child.md\n");

        const std::string sp =
            batbox::conversation::compose_system_prompt(false, work.path);

        // child.md content must appear (direct import).
        CHECK(contains(sp, "CHILD_BEFORE"));
        CHECK(contains(sp, "CHILD_AFTER"));
        // grandchild.md must NOT appear — single-level only, no recursion.
        CHECK_FALSE(contains(sp, "GRANDCHILD_CONTENT"));
        // The @import line in child.md should appear verbatim since we don't
        // recurse into imported files.
        CHECK(contains(sp, "@import grandchild.md"));
    }
}

// =============================================================================
// Acceptance criterion: Cycle (a imports b imports a) detected and broken
// =============================================================================
TEST_SUITE("@import cycle detection") {
    TEST_CASE("direct self-import is skipped without hang") {
        TempDir home{"cycle_self_home"};
        TempDir work{"cycle_self_work"};
        EnvGuard cfg{"BATBOX_CONFIG_DIR", home.path.string().c_str()};

        // BATBOX.md tries to import itself.
        work.write("BATBOX.md", "SELF_BEFORE\n@import BATBOX.md\nSELF_AFTER\n");

        std::string sp;
        CHECK_NOTHROW(sp = batbox::conversation::compose_system_prompt(false, work.path));
        // Content outside the recursive @import must be present.
        CHECK(contains(sp, "SELF_BEFORE"));
        CHECK(contains(sp, "SELF_AFTER"));
    }

    TEST_CASE("two-level cycle (a imports b, b mentions a) is broken") {
        // Note: since imported files are NOT recursively re-scanned for @import
        // directives (single-level only), a two-file cycle where b imports a
        // cannot occur at runtime.  This test validates that even if somehow
        // a user creates two BATBOX.md files where the user one imports the
        // same file that the project one imports, the seen-paths dedup prevents
        // duplication.
        TempDir home{"cycle_two_home"};
        TempDir work{"cycle_two_work"};
        EnvGuard cfg{"BATBOX_CONFIG_DIR", home.path.string().c_str()};

        // shared.md is referenced from both user and project BATBOX.md.
        work.write("shared.md", "SHARED_CONTENT\n");
        // User BATBOX.md imports shared.md.
        home.write("BATBOX.md", "@import " + (work.path / "shared.md").string() + "\n");
        // Project BATBOX.md also imports shared.md (absolute path).
        work.write("BATBOX.md", "@import " + (work.path / "shared.md").string() + "\n");

        std::string sp;
        CHECK_NOTHROW(sp = batbox::conversation::compose_system_prompt(false, work.path));
        // SHARED_CONTENT should appear at most once (dedup via seen paths).
        const auto first = sp.find("SHARED_CONTENT");
        if (first != std::string::npos) {
            const auto second = sp.find("SHARED_CONTENT", first + 1);
            CHECK(second == std::string::npos);  // must not appear twice
        }
    }
}

// =============================================================================
// Acceptance criterion: Plan mode prefix added when active
// =============================================================================
TEST_SUITE("plan mode prefix") {
    TEST_CASE("plan_mode=true adds plan-mode prefix") {
        TempDir home{"plan_home"};
        TempDir work{"plan_work"};
        EnvGuard cfg{"BATBOX_CONFIG_DIR", home.path.string().c_str()};

        const std::string sp =
            batbox::conversation::compose_system_prompt(true, work.path);

        CHECK(contains(sp, "PLAN MODE"));
        CHECK(contains(sp, "read-only"));
        // Prefix must appear before the base system prompt.
        CHECK(before(sp, "PLAN MODE", "BatBox"));
    }

    TEST_CASE("plan_mode=false does NOT include plan-mode prefix") {
        TempDir home{"noplan_home"};
        TempDir work{"noplan_work"};
        EnvGuard cfg{"BATBOX_CONFIG_DIR", home.path.string().c_str()};

        const std::string sp =
            batbox::conversation::compose_system_prompt(false, work.path);

        CHECK_FALSE(contains(sp, "PLAN MODE"));
    }

    TEST_CASE("plan mode prefix appears before all other content including user BATBOX.md") {
        TempDir home{"plan_order_home"};
        TempDir work{"plan_order_work"};
        EnvGuard cfg{"BATBOX_CONFIG_DIR", home.path.string().c_str()};

        home.write("BATBOX.md", "USER_TEXT_ALPHA\n");
        work.write("BATBOX.md", "PROJ_TEXT_BETA\n");

        const std::string sp =
            batbox::conversation::compose_system_prompt(true, work.path);

        CHECK(before(sp, "PLAN MODE", "USER_TEXT_ALPHA"));
        CHECK(before(sp, "PLAN MODE", "PROJ_TEXT_BETA"));
    }
}

// =============================================================================
// Edge cases
// =============================================================================
TEST_SUITE("edge cases") {
    TEST_CASE("empty working_dir falls back to process cwd") {
        TempDir home{"empty_wd_home"};
        EnvGuard cfg{"BATBOX_CONFIG_DIR", home.path.string().c_str()};

        std::string sp;
        CHECK_NOTHROW(sp = batbox::conversation::compose_system_prompt(
            false, fs::path{}));
        CHECK(contains(sp, "BatBox"));
    }

    TEST_CASE("non-existent working_dir does not crash") {
        TempDir home{"nonexist_home"};
        EnvGuard cfg{"BATBOX_CONFIG_DIR", home.path.string().c_str()};

        std::string sp;
        CHECK_NOTHROW(sp = batbox::conversation::compose_system_prompt(
            false, fs::path{"/tmp/batbox_nonexistent_dir_xyz_12345"}));
        // Base prompt must still be present.
        CHECK(contains(sp, "BatBox"));
    }

    TEST_CASE("project BATBOX.md found via upward walk from subdirectory") {
        TempDir home{"upwalk_home"};
        TempDir root{"upwalk_root"};
        EnvGuard cfg{"BATBOX_CONFIG_DIR", home.path.string().c_str()};

        // Place BATBOX.md at the root.
        root.write("BATBOX.md", "ROOT_BATBOX_CONTENT\n");
        // Create a nested subdirectory.
        const fs::path subdir = root.path / "a" / "b" / "c";
        fs::create_directories(subdir);

        const std::string sp =
            batbox::conversation::compose_system_prompt(false, subdir);

        // The root-level BATBOX.md must be found.
        CHECK(contains(sp, "ROOT_BATBOX_CONTENT"));
    }

    TEST_CASE("result is non-empty in all cases") {
        TempDir home{"nonempty_home"};
        TempDir work{"nonempty_work"};
        EnvGuard cfg{"BATBOX_CONFIG_DIR", home.path.string().c_str()};

        const std::string sp =
            batbox::conversation::compose_system_prompt(false, work.path);
        CHECK_FALSE(sp.empty());
    }

    TEST_CASE("public constants are non-null and non-empty") {
        CHECK(batbox::conversation::k_base_system_prompt != nullptr);
        CHECK(std::string{batbox::conversation::k_base_system_prompt}.size() > 0);
        CHECK(batbox::conversation::k_plan_mode_prefix != nullptr);
        CHECK(std::string{batbox::conversation::k_plan_mode_prefix}.size() > 0);
    }
}

// =============================================================================
// TUI-PLAN-T1: EnterPlanMode unsolicited-call guard in base prompt
// =============================================================================
TEST_SUITE("EnterPlanMode guard") {
    TEST_CASE("k_base_system_prompt contains EnterPlanMode unsolicited-call guidance") {
        // The base prompt must explicitly instruct the model not to call
        // EnterPlanMode unless the user asked for it.  This guard prevents the
        // model from auto-entering plan mode on casual requests.
        const std::string base{batbox::conversation::k_base_system_prompt};
        CHECK(contains(base, "Do NOT call the EnterPlanMode tool"));
        CHECK(contains(base, "EnterPlanMode unsolicited"));
    }

    TEST_CASE("composed prompt (plan_mode=false) contains EnterPlanMode guard") {
        TempDir home{"guard_home"};
        TempDir work{"guard_work"};
        EnvGuard cfg{"BATBOX_CONFIG_DIR", home.path.string().c_str()};

        const std::string sp =
            batbox::conversation::compose_system_prompt(false, work.path);
        CHECK(contains(sp, "Do NOT call the EnterPlanMode tool"));
    }

    TEST_CASE("composed prompt (plan_mode=true) still contains EnterPlanMode guard") {
        // Even in plan-mode the base prompt (which contains the guard) is included.
        TempDir home{"guard_plan_home"};
        TempDir work{"guard_plan_work"};
        EnvGuard cfg{"BATBOX_CONFIG_DIR", home.path.string().c_str()};

        const std::string sp =
            batbox::conversation::compose_system_prompt(true, work.path);
        // Plan-mode prefix must be present.
        CHECK(contains(sp, "PLAN MODE"));
        // Base prompt guard must also be present.
        CHECK(contains(sp, "Do NOT call the EnterPlanMode tool"));
    }

    TEST_CASE("EnterPlanMode guard appears after plan-mode prefix") {
        TempDir home{"guard_order_home"};
        TempDir work{"guard_order_work"};
        EnvGuard cfg{"BATBOX_CONFIG_DIR", home.path.string().c_str()};

        const std::string sp =
            batbox::conversation::compose_system_prompt(true, work.path);
        // The plan-mode prefix (layer 0) appears before the base prompt (layer 1)
        // which contains the guard.
        CHECK(before(sp, "PLAN MODE", "Do NOT call the EnterPlanMode tool"));
    }
}

// =============================================================================
// TUI-PLAN-T4: cards-then-plan workflow instructions in k_plan_mode_prefix
// =============================================================================
TEST_SUITE("TUI-PLAN-T4 cards-then-plan prefix") {
    TEST_CASE("k_plan_mode_prefix instructs model to use AskUserQuestion") {
        // The plan-mode prefix must explicitly name AskUserQuestion so the model
        // knows which tool to call for clarifying questions.
        const std::string prefix{batbox::conversation::k_plan_mode_prefix};
        CHECK(contains(prefix, "AskUserQuestion"));
    }

    TEST_CASE("k_plan_mode_prefix specifies multi-choice questions") {
        // The prefix must require multi-choice options (not yes/no) to prevent
        // binary-only questions that don't give the model enough decision context.
        const std::string prefix{batbox::conversation::k_plan_mode_prefix};
        CHECK(contains(prefix, "multi-choice"));
    }

    TEST_CASE("k_plan_mode_prefix requires ExitPlanMode call for plan submission") {
        // The model must be told to call ExitPlanMode with the plan markdown.
        const std::string prefix{batbox::conversation::k_plan_mode_prefix};
        CHECK(contains(prefix, "ExitPlanMode"));
    }

    TEST_CASE("k_plan_mode_prefix blocks mutating tools until ExitPlanMode approved") {
        // Constraint: Write, Edit, and Bash (mutating) must not be called in plan mode.
        const std::string prefix{batbox::conversation::k_plan_mode_prefix};
        CHECK(contains(prefix, "MUST NOT call Write, Edit, or Bash"));
    }

    TEST_CASE("k_plan_mode_prefix allows read-only tools in plan mode") {
        // The model must know it can read files and grep during planning.
        const std::string prefix{batbox::conversation::k_plan_mode_prefix};
        CHECK(contains(prefix, "read-only tools allowed"));
    }

    TEST_CASE("composed plan-mode prompt contains AskUserQuestion instruction") {
        // The composed system prompt when plan_mode=true must also contain the
        // AskUserQuestion instruction (inherited from k_plan_mode_prefix, layer 0).
        TempDir home{"t4_aq_home"};
        TempDir work{"t4_aq_work"};
        EnvGuard cfg{"BATBOX_CONFIG_DIR", home.path.string().c_str()};

        const std::string sp =
            batbox::conversation::compose_system_prompt(true, work.path);
        CHECK(contains(sp, "AskUserQuestion"));
        CHECK(contains(sp, "multi-choice"));
        CHECK(contains(sp, "ExitPlanMode"));
    }

    TEST_CASE("composed plan-mode prompt: AskUserQuestion instruction precedes ExitPlanMode step") {
        // Step 1 (ask questions) must appear before Step 3 (call ExitPlanMode)
        // in the prefix to match the intended workflow order.
        TempDir home{"t4_order_home"};
        TempDir work{"t4_order_work"};
        EnvGuard cfg{"BATBOX_CONFIG_DIR", home.path.string().c_str()};

        const std::string sp =
            batbox::conversation::compose_system_prompt(true, work.path);
        CHECK(before(sp, "AskUserQuestion", "ExitPlanMode"));
    }

    TEST_CASE("plan-mode prefix step limit: plans capped at 10 steps") {
        // Ensure the prefix communicates a step limit to keep plans manageable.
        const std::string prefix{batbox::conversation::k_plan_mode_prefix};
        CHECK(contains(prefix, "10 steps"));
    }

    TEST_CASE("AskUserQuestion NOT present when plan_mode=false") {
        // Regression: AskUserQuestion instruction is only in the plan-mode prefix.
        // It must not appear in the base prompt (non-plan-mode composed output).
        // NOTE: The base prompt itself does not mention AskUserQuestion — only
        // the plan-mode prefix does.  This test verifies the two layers are distinct.
        TempDir home{"t4_noplan_home"};
        TempDir work{"t4_noplan_work"};
        EnvGuard cfg{"BATBOX_CONFIG_DIR", home.path.string().c_str()};

        const std::string sp =
            batbox::conversation::compose_system_prompt(false, work.path);
        // The base prompt must not contain "AskUserQuestion" (that instruction
        // belongs only in plan-mode context).
        CHECK_FALSE(contains(sp, "AskUserQuestion"));
    }
}
