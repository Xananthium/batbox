// tests/unit/test_no_thread_id_across_process.cpp
//
// PEXT2 4.2 — Repo-wide audit: assert zero occurrences of thread-id-as-uniquifier
// patterns that cross process boundaries.
//
// Patterns guarded (each stored split across two adjacent string literals so
// this file cannot self-report):
//
//   P1  std::hash<std::thread::id>{}(std::this_thread::get_id())
//       — already covered by test_no_thread_id_uniquifier (PEXT2 1.3) for the
//         hash-based form; this test adds the broader raw-get-id patterns.
//
//   P2  pthread_self() used in a path / name construction context
//       — platform TID that forks get a clone of; inter-process uniqueness fails.
//
//   P3  GetCurrentThreadId() — Win32 equivalent of pthread_self().
//
//   P4  gettid() — Linux-specific syscall returning a per-process TID.
//       On fork the child gets tid == 1 in its new namespace, so the
//       parent's tid is meaningless across the fork boundary.
//
// Note: std::this_thread::get_id() ALONE (without hashing) could be legitimate
// for in-process bookkeeping (e.g., logging which thread fired an event).
// This test therefore scans for the COMBINED pattern of get_id() used in a
// filesystem path construction: the literal token that triggered PEXT2 1.3 is
// already banned by test_no_thread_id_uniquifier.  For the broader OS-level
// forms (P2–P4) we flag ALL occurrences in tests/ and src/ and require zero,
// since no batbox code legitimately needs raw OS TIDs that survive fork.
//
// If a future use-case is genuinely in-process and safe, the author must add
// a // PEXT2-4.2-safe comment on the same line to document the intent, and the
// scan logic here excludes such lines.
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

// Self-exclusion — this file must not report itself.
static constexpr const char* kSelfName = "test_no_thread_id_across_process.cpp";
// Also exclude the PEXT2 1.3 file (handles its own banned-token).
static constexpr const char* kSiblingName = "test_no_thread_id_uniquifier.cpp";

/// Patterns banned in production + test code.
/// Each is split across adjacent string literals to prevent self-report.
struct Pattern {
    std::string token;
    const char* description;
};

static const Pattern kBannedPatterns[] = {
    // Raw get_id() — already caught by sibling test when combined with hash,
    // but catch the standalone form too in case it migrates to a path.
    { "std::this_thread::" "get_id()",
      "std::this_thread::get_id() (raw thread-id — use getpid()+nanos instead when building filesystem paths)" },
    // POSIX / OS-level TIDs.
    { "pthread_" "self()",
      "pthread_self() (POSIX thread-id; not unique across forked processes)" },
    { "GetCurrentThread" "Id()",
      "GetCurrentThreadId() (Win32 thread-id; not unique across forked processes)" },
    { "gettid" "()",
      "gettid() (Linux thread-id; TID namespace resets on fork)" },
};

/// Escape sequence inserted on the same line to suppress the ban.
static const std::string kSafeMarker = "PEXT2-4.2-safe";

/// Read the entire content of a file into a string.
static std::string slurp(const fs::path& p) {
    std::ifstream f(p);
    if (!f.is_open()) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// Scan one line at a time: if it contains the banned token AND does NOT
/// contain the safe marker, record it.
static void scan_file(const fs::path& p,
                      const std::string& root_str,
                      std::vector<std::string>& offenders) {
    std::ifstream f(p);
    if (!f.is_open()) return;

    std::string line;
    int lineno = 0;
    while (std::getline(f, line)) {
        ++lineno;
        if (line.find(kSafeMarker) != std::string::npos) continue;

        for (const auto& pat : kBannedPatterns) {
            if (line.find(pat.token) != std::string::npos) {
                const std::string rel =
                    fs::path(p).lexically_relative(fs::path(root_str)).string();
                offenders.push_back(
                    rel + ":" + std::to_string(lineno) + " — " + pat.description);
            }
        }
    }
}

static void scan_dir(const fs::path& dir,
                     const std::string& root_str,
                     std::vector<std::string>& offenders) {
    if (!fs::is_directory(dir)) return;

    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        const fs::path& p = entry.path();

        if (p.filename() == kSelfName)    continue;
        if (p.filename() == kSiblingName) continue;

        const auto ext = p.extension();
        if (ext != ".cpp" && ext != ".hpp") continue;

        scan_file(p, root_str, offenders);
    }
}

TEST_CASE("PEXT2 4.2 — no thread-id-as-uniquifier pattern that crosses process boundaries") {
    const fs::path source_root(BATBOX_SOURCE_DIR);
    const std::string root_str = source_root.string();

    const fs::path tests_dir   = source_root / "tests";
    const fs::path src_dir     = source_root / "src";
    const fs::path include_dir = source_root / "include";

    REQUIRE(fs::is_directory(tests_dir));
    REQUIRE(fs::is_directory(src_dir));
    // include/ may not exist; only assert if present.

    std::vector<std::string> offenders;
    scan_dir(tests_dir,   root_str, offenders);
    scan_dir(src_dir,     root_str, offenders);
    if (fs::is_directory(include_dir))
        scan_dir(include_dir, root_str, offenders);

    if (!offenders.empty()) {
        MESSAGE("Files using thread-id patterns that are unsafe across process boundaries.");
        MESSAGE("Fix: replace with ::getpid() * 1000000UL + (steady_clock nanos % 1000000).");
        MESSAGE("If the usage is genuinely in-process (e.g., a debug label), add");
        MESSAGE("  // PEXT2-4.2-safe");
        MESSAGE("on the same source line to suppress this check.");
        MESSAGE("");
        for (const auto& o : offenders) {
            MESSAGE("  - " << o);
        }
    }

    CHECK(offenders.empty());
}
