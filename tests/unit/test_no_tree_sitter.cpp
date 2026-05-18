// tests/unit/test_no_tree_sitter.cpp
// ---------------------------------------------------------------------------
// Meta-test: verify tree-sitter is fully removed from the codebase.
//
// Greps all .cpp, .hpp, .cmake, and CMakeLists.txt files under:
//   src/, include/, tests/ (excluding this file), and the repo root cmake/
// for any of the following forbidden strings:
//   - "tree_sitter"          (C API / symbol prefix — NOT the test target name)
//   - "BATBOX_SYNTAX"        (compile-time switch that no longer exists)
//   - "batbox_treesitter"    (CMake target name for vendored grammar library)
//   - "add_subdirectory(vendor)" (the vendor/ subdirectory hook)
//
// Exceptions:
//   - This file itself is excluded (it contains the strings as string literals).
//   - Lines matching "test_no_tree_sitter" are excluded: the test target name
//     contains "_tree_sitter" as part of its identifier, not as a TS API ref.
//
// Any other match is a test failure. This guards against accidental
// re-introduction of tree-sitter into the codebase.
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

/// Return absolute path to the repository root (the directory that contains
/// the root CMakeLists.txt). We walk up from this test file's location.
fs::path repo_root() {
    // __FILE__ gives .../tests/unit/test_no_tree_sitter.cpp
    // Up two levels → repo root.
    fs::path here = fs::path(__FILE__).parent_path().parent_path().parent_path();
    return here.lexically_normal();
}

struct Hit {
    fs::path   file;
    int        line_number;
    std::string line_text;
    std::string pattern;
};

/// Scan a single file for forbidden patterns. Appends any hits to `out`.
/// Lines that contain "test_no_tree_sitter" are exempt — that identifier is
/// the CMake target / test name for this meta-test, not a TS API reference.
void scan_file(const fs::path& p, const std::vector<std::string>& patterns,
               std::vector<Hit>& out) {
    std::ifstream f(p);
    if (!f.is_open()) return;

    // Exemption: lines that only mention the test target name itself
    static const std::string exempt_marker = "test_no_tree_sitter";

    std::string line;
    int lineno = 0;
    while (std::getline(f, line)) {
        ++lineno;

        // If the line references our own test target name, skip it —
        // "test_no_tree_sitter" is allowed as an identifier.
        if (line.find(exempt_marker) != std::string::npos) continue;

        for (const auto& pat : patterns) {
            if (line.find(pat) != std::string::npos) {
                out.push_back({p, lineno, line, pat});
            }
        }
    }
}

/// Decide whether a path should be scanned.
bool should_scan(const fs::path& p, const fs::path& this_file) {
    // Only scan regular files
    if (!fs::is_regular_file(p)) return false;

    // Skip this file itself (it contains the forbidden strings as string literals)
    if (p.lexically_normal() == this_file) return false;

    const std::string ext = p.extension().string();
    const std::string filename = p.filename().string();

    // Include .cpp, .hpp, .cmake, and CMakeLists.txt
    if (ext == ".cpp" || ext == ".hpp" || ext == ".cmake") return true;
    if (filename == "CMakeLists.txt") return true;

    return false;
}

} // namespace

TEST_CASE("no tree-sitter references remain in the codebase") {
    const fs::path root = repo_root();
    const fs::path this_file = fs::path(__FILE__).lexically_normal();

    // Directories to scan
    const std::vector<fs::path> scan_dirs = {
        root / "src",
        root / "include",
        root / "tests",
    };

    // Forbidden patterns (exact substrings)
    const std::vector<std::string> forbidden = {
        "tree_sitter",
        "BATBOX_SYNTAX",
        "batbox_treesitter",
        "add_subdirectory(vendor)",
    };

    std::vector<Hit> hits;

    // Scan src/, include/, tests/ recursively
    for (const auto& dir : scan_dirs) {
        if (!fs::exists(dir)) continue;
        for (const auto& entry : fs::recursive_directory_iterator(dir)) {
            if (should_scan(entry.path(), this_file)) {
                scan_file(entry.path(), forbidden, hits);
            }
        }
    }

    // Also scan root-level CMakeLists.txt and cmake/ directory
    const fs::path root_cmake = root / "CMakeLists.txt";
    if (fs::exists(root_cmake) && should_scan(root_cmake, this_file)) {
        scan_file(root_cmake, forbidden, hits);
    }
    const fs::path cmake_dir = root / "cmake";
    if (fs::exists(cmake_dir)) {
        for (const auto& entry : fs::recursive_directory_iterator(cmake_dir)) {
            if (should_scan(entry.path(), this_file)) {
                scan_file(entry.path(), forbidden, hits);
            }
        }
    }

    if (!hits.empty()) {
        std::ostringstream msg;
        msg << "Found " << hits.size()
            << " forbidden tree-sitter reference(s) — tree-sitter was removed in PEXT2 1.4a:\n\n";
        for (const auto& h : hits) {
            msg << "  [" << h.pattern << "] "
                << fs::relative(h.file, root).string()
                << ":" << h.line_number << "\n"
                << "    " << h.line_text << "\n";
        }
        FAIL(msg.str());
    }

    CHECK(hits.empty());
}
