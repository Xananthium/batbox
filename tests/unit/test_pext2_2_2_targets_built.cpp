// tests/unit/test_pext2_2_2_targets_built.cpp
// ---------------------------------------------------------------------------
// PEXT2 2.2 meta-test: verify every predicted-lag target binary exists.
//
// Karla's Section B (karla-postmortem-postext.md) predicted 16 targets that
// could exhibit fixture lag after the PEXT changes.  This test asserts that
// each target's binary is present on disk, confirming:
//   1. The target compiled successfully (no build-time regression).
//   2. The binary path matches the CMake-generated layout.
//
// Binary layout:
//   integration tests → build/tests/integration/<target>
//   unit tests        → build/tests/<target>
//
// The build directory is expected to be a sibling of the repo root named
// "build" (the standard batbox development convention). If the build
// directory cannot be found, the test emits a MESSAGE and passes (CI
// environments without a local build should not block the suite).
//
// Targets covered (all 16 from Karla Section B):
//   test_conversation_basic          (PEXT2 1.1 already fixed — verify)
//   test_conversation_tool_loop
//   test_subagent
//   test_agent_planning_commands
//   test_agent_supervision_bounded
//   test_agent_supervision_integration
//   test_workflow_dag
//   test_demon_command
//   test_status_stats_commands       (PEXT2 1.2 already fixed — verify)
//   test_team_family
//   test_chat_view
//   test_tui_layout                  (fixture-fixed in PEXT2 2.2)
//   test_repl
//   test_compactor
//   test_compactor_model_param
//   test_compactor_no_copy
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

/// Derive the repository root from this source file's location.
/// __FILE__ gives .../tests/unit/test_pext2_2_2_targets_built.cpp
/// Parent × 3 = repo root.
fs::path repo_root() {
    fs::path here = fs::path(__FILE__)   // ...tests/unit/test_pext2_2_2_targets_built.cpp
                        .parent_path()   // ...tests/unit
                        .parent_path()   // ...tests
                        .parent_path();  // ...repo root
    return here.lexically_normal();
}

/// Find the build directory.  Returns std::nullopt if not found.
/// Checks <repo_root>/build first (standard convention), then
/// <repo_root>/../build (out-of-tree builds one level up).
std::optional<fs::path> find_build_dir() {
    const fs::path root = repo_root();

    const std::vector<fs::path> candidates = {
        root / "build",
        root.parent_path() / "build",
    };

    for (const auto& candidate : candidates) {
        // A valid build dir has a CMakeCache.txt.
        if (fs::exists(candidate / "CMakeCache.txt")) {
            return candidate.lexically_normal();
        }
    }
    return std::nullopt;
}

struct Target {
    std::string name;
    bool        is_unit;  ///< true → build/tests/<name>, false → build/tests/integration/<name>
};

/// All 16 Karla Section B predicted-lag targets.
const std::vector<Target> kTargets = {
    // Conversation / high-severity targets
    { "test_conversation_basic",            false },  // PEXT2 1.1 (already fixed)
    { "test_conversation_tool_loop",        false },
    { "test_subagent",                      false },
    { "test_agent_planning_commands",       false },
    { "test_agent_supervision_bounded",     false },
    { "test_agent_supervision_integration", false },
    { "test_workflow_dag",                  false },
    { "test_demon_command",                 false },
    { "test_status_stats_commands",         false },  // PEXT2 1.2 (already fixed)
    { "test_team_family",                   false },
    // TUI targets
    { "test_chat_view",                     true  },  // lives in build/tests/ (unit layout)
    { "test_tui_layout",                    false },  // PEXT2 2.2 fixture-fixed
    { "test_repl",                          false },
    // Compactor targets
    { "test_compactor",                     false },
    { "test_compactor_model_param",         true  },  // lives in build/tests/ (unit layout)
    { "test_compactor_no_copy",             true  },  // lives in build/tests/ (unit layout)
};

}  // namespace

// =============================================================================
// Meta-test: each Karla Section B binary must exist on disk.
// =============================================================================

TEST_CASE("PEXT2 2.2: all Karla Section B targets have compiled binaries") {
    const auto build = find_build_dir();

    if (!build.has_value()) {
        // No build directory found — skip gracefully (CI without local build).
        MESSAGE("build/ directory not found from repo root "
                << repo_root().string()
                << " — skipping binary-existence check");
        return;
    }

    const fs::path integration_dir = *build / "tests" / "integration";
    const fs::path unit_dir        = *build / "tests";

    int missing = 0;

    for (const auto& tgt : kTargets) {
        const fs::path dir    = tgt.is_unit ? unit_dir : integration_dir;
        const fs::path binary = dir / tgt.name;

        INFO("Checking for binary: " << binary.string());

        bool exists = fs::exists(binary) && !fs::is_directory(binary);

        if (!exists) {
            MESSAGE("MISSING binary: " << binary.string());
            ++missing;
        }

        CHECK(exists);
    }

    if (missing == 0) {
        MESSAGE("All " << kTargets.size()
                << " Karla Section B binaries present in "
                << build->string());
    }
}

// =============================================================================
// Spot-check: binaries must be non-empty (not a zero-byte stub).
// =============================================================================

TEST_CASE("PEXT2 2.2: all Karla Section B binaries are non-empty") {
    const auto build = find_build_dir();

    if (!build.has_value()) {
        MESSAGE("build/ directory not found — skipping size check");
        return;
    }

    const fs::path integration_dir = *build / "tests" / "integration";
    const fs::path unit_dir        = *build / "tests";

    for (const auto& tgt : kTargets) {
        const fs::path dir    = tgt.is_unit ? unit_dir : integration_dir;
        const fs::path binary = dir / tgt.name;

        if (!fs::exists(binary) || fs::is_directory(binary)) {
            // Existence failure already reported in the first test case.
            continue;
        }

        const auto size = fs::file_size(binary);
        INFO("Binary " << tgt.name << " size = " << size << " bytes");
        CHECK(size > 0);
    }
}
