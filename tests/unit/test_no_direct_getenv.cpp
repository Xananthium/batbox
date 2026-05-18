// tests/unit/test_no_direct_getenv.cpp
//
// PEXT 3.1 — Regression: assert zero direct std::getenv( calls in src/commands/*.cpp
//
// Strategy
// --------
// Reads every .cpp file under src/commands/ and asserts that none contain the
// literal token "std::getenv(" (with opening parenthesis).  This is the same
// pattern that the PEXT 3.1 acceptance criterion grep uses:
//
//   grep -REn 'std::getenv\(' src/commands/
//
// All legitimate env reads in that directory now use either:
//   - batbox::paths::home_dir()           for $HOME
//   - batbox::util::binary_accessible()   for $PATH scanning
//   - ::getenv(...)                        for non-Config env vars (C-style)
//
// ModelCmd.cpp is excluded from comment but IS included in the scan — it was
// already fixed in PEXT 2.1 and should have zero std::getenv calls.
//
// The test uses <filesystem> + <fstream> to scan without shelling out, so it
// works on all platforms supported by batbox (macOS/Linux).
//
// IMPORTANT: this test relies on the source tree being present at build time.
// The CMakeLists.txt entry passes PROJECT_SOURCE_DIR as a compile definition
// so the test can locate src/commands/ at runtime.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// Injected by CMakeLists.txt: -DBATBOX_SOURCE_DIR="..."
#ifndef BATBOX_SOURCE_DIR
#  error "BATBOX_SOURCE_DIR must be set by CMakeLists.txt via target_compile_definitions"
#endif

static constexpr const char* kToken = "std::getenv(";

/// Read the entire content of a file into a string.
static std::string slurp(const fs::path& p) {
    std::ifstream f(p);
    if (!f.is_open()) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

TEST_CASE("PEXT 3.1 — no direct std::getenv( in src/commands/*.cpp") {
    const fs::path commands_dir =
        fs::path(BATBOX_SOURCE_DIR) / "src" / "commands";

    REQUIRE(fs::is_directory(commands_dir));

    std::vector<std::string> offenders;

    for (const auto& entry : fs::directory_iterator(commands_dir)) {
        if (!entry.is_regular_file()) continue;
        const fs::path& p = entry.path();
        if (p.extension() != ".cpp") continue;

        const std::string content = slurp(p);
        if (content.find(kToken) != std::string::npos) {
            // Count occurrences for diagnostic detail.
            std::size_t count = 0;
            std::size_t pos   = 0;
            while ((pos = content.find(kToken, pos)) != std::string::npos) {
                ++count;
                pos += std::string(kToken).size();
            }
            offenders.push_back(p.filename().string()
                                 + " (" + std::to_string(count) + " occurrence(s))");
        }
    }

    if (!offenders.empty()) {
        // Emit diagnostic before failing.
        MESSAGE("Files with direct std::getenv( calls (route through ctx.cfg or ::getenv):");
        for (const auto& o : offenders) {
            MESSAGE("  - " << o);
        }
    }

    CHECK(offenders.empty());
}
