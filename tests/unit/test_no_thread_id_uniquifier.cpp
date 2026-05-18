// tests/unit/test_no_thread_id_uniquifier.cpp
//
// PEXT2 1.3 — Regression: assert zero occurrences of the thread-id hash
// anti-pattern used as a temp-dir uniquifier in tests/ and src/.
//
// The pattern being banned is:  hash<thread::id>{}(this_thread::get_id())
// used as a filesystem path uniquifier — it collides across forked ctest
// worker processes on macOS because the main thread pthread_t is allocated
// identically per fork.
//
// The correct replacement is:  ::getpid() + steady_clock nanoseconds
//
// This file is excluded from the scan by filename so that legitimate
// references to the token string inside this file do not self-report.
//
// BATBOX_SOURCE_DIR is injected by CMakeLists.txt via target_compile_definitions.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

#ifndef BATBOX_SOURCE_DIR
#  error "BATBOX_SOURCE_DIR must be set by CMakeLists.txt via target_compile_definitions"
#endif

// Token is the literal text we forbid.  Stored as a const so the compiler
// can see it, but built from two concatenated string literals so that the
// token does not appear as a substring of this source file's own content
// (which would cause the test to spuriously flag itself).
static const std::string kBannedToken =
    "std::hash<std::thread" "::id>";

// This file's own basename — excluded from scanning to prevent self-report.
static constexpr const char* kSelfName = "test_no_thread_id_uniquifier.cpp";
// Sibling audit meta-test — also excluded (it documents the banned token in comments).
static constexpr const char* kSiblingName = "test_no_thread_id_across_process.cpp";

/// Read the entire content of a file into a string.
static std::string slurp(const fs::path& p) {
    std::ifstream f(p);
    if (!f.is_open()) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// Scan `dir` recursively for .cpp/.hpp files that contain kBannedToken,
/// skipping kSelfName.  Appends offender descriptions to `offenders`.
static void scan_dir(const fs::path& dir, std::vector<std::string>& offenders) {
    if (!fs::is_directory(dir)) return;

    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        const fs::path& p = entry.path();

        // Skip this meta-check file itself and its sibling audit test.
        if (p.filename() == kSelfName)    continue;
        if (p.filename() == kSiblingName) continue;

        const auto ext = p.extension();
        if (ext != ".cpp" && ext != ".hpp") continue;

        const std::string content = slurp(p);
        if (content.find(kBannedToken) == std::string::npos) continue;

        // Count occurrences for diagnostic detail.
        std::size_t count = 0;
        std::size_t pos   = 0;
        while ((pos = content.find(kBannedToken, pos)) != std::string::npos) {
            ++count;
            pos += kBannedToken.size();
        }
        offenders.push_back(p.lexically_relative(fs::path(BATBOX_SOURCE_DIR)).string()
                            + " (" + std::to_string(count) + " occurrence(s))");
    }
}

TEST_CASE("PEXT2 1.3 — no thread-id hash used as temp-dir uniquifier in tests/ or src/") {
    const fs::path source_root(BATBOX_SOURCE_DIR);

    const fs::path tests_dir = source_root / "tests";
    const fs::path src_dir   = source_root / "src";

    REQUIRE(fs::is_directory(tests_dir));
    REQUIRE(fs::is_directory(src_dir));

    std::vector<std::string> offenders;
    scan_dir(tests_dir, offenders);
    scan_dir(src_dir,   offenders);

    if (!offenders.empty()) {
        MESSAGE("Files still using thread-id hash as uniquifier (replace with ::getpid() + steady_clock nanos):");
        for (const auto& o : offenders) {
            MESSAGE("  - " << o);
        }
    }

    CHECK(offenders.empty());
}
