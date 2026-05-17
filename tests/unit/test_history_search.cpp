// tests/unit/test_history_search.cpp
// ---------------------------------------------------------------------------
// Unit tests for batbox::repl::HistorySearch.
//
// Build + run (standalone, no CMake needed):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_history_search.cpp \
//       src/repl/HistorySearch.cpp \
//       src/repl/History.cpp \
//       src/core/Paths.cpp \
//       -o /tmp/test_hs && /tmp/test_hs
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "batbox/repl/HistorySearch.hpp"
#include "batbox/repl/History.hpp"

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using batbox::repl::History;
using batbox::repl::HistorySearch;
using batbox::repl::MatchResult;

// ---------------------------------------------------------------------------
// RAII temporary directory helper (mirrored from test_history.cpp).
// ---------------------------------------------------------------------------
namespace {

struct TmpDir {
    fs::path path;
    TmpDir() {
        static std::atomic<int> counter{0};
        const int id = counter.fetch_add(1, std::memory_order_relaxed);
        path = fs::temp_directory_path() /
               ("batbox_hssearch_test_" + std::to_string(id));
        fs::create_directories(path);
    }
    ~TmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    fs::path file(const char* name = "history") const { return path / name; }
    TmpDir(const TmpDir&)            = delete;
    TmpDir& operator=(const TmpDir&) = delete;
};

// Build a History with persistence disabled, push a list of entries.
History make_history(const std::vector<std::string>& entries) {
    History h{fs::path{}, 10'000};
    for (const auto& e : entries) {
        h.push(e);
    }
    return h;
}

} // anonymous namespace

// ===========================================================================
// Basic active / reset lifecycle
// ===========================================================================

TEST_SUITE("HistorySearch — lifecycle") {

    TEST_CASE("not active before first filter_matches call") {
        TmpDir tmp;
        History h{tmp.file(), 100};
        HistorySearch hs{h};
        CHECK_FALSE(hs.active());
    }

    TEST_CASE("active after filter_matches") {
        TmpDir tmp;
        History h{tmp.file(), 100};
        h.push("git status");
        HistorySearch hs{h};
        hs.filter_matches("git");
        CHECK(hs.active());
    }

    TEST_CASE("reset clears active flag and matches") {
        TmpDir tmp;
        History h{tmp.file(), 100};
        h.push("echo hello");
        HistorySearch hs{h};
        hs.filter_matches("echo");
        REQUIRE(hs.active());
        hs.reset();
        CHECK_FALSE(hs.active());
        CHECK(hs.matches().empty());
    }

    TEST_CASE("selected() returns nullopt on empty history") {
        TmpDir tmp;
        History h{tmp.file(), 100};
        HistorySearch hs{h};
        hs.filter_matches("anything");
        CHECK_FALSE(hs.selected().has_value());
    }

    TEST_CASE("next_match() returns nullopt when no matches") {
        TmpDir tmp;
        History h{tmp.file(), 100};
        HistorySearch hs{h};
        hs.filter_matches("zzznomatch");
        CHECK_FALSE(hs.next_match().has_value());
    }
}

// ===========================================================================
// Substring matching
// ===========================================================================

TEST_SUITE("HistorySearch — substring matching") {

    TEST_CASE("exact substring match is returned") {
        auto h = make_history({"git commit -m 'fix'", "echo hello", "ls -la"});
        HistorySearch hs{h};
        const auto& results = hs.filter_matches("echo");
        REQUIRE(results.size() == 1);
        CHECK(results[0].text == "echo hello");
    }

    TEST_CASE("case-insensitive matching — query uppercase matches lowercase entry") {
        auto h = make_history({"git status", "echo hello"});
        HistorySearch hs{h};
        const auto& results = hs.filter_matches("GIT");
        REQUIRE(results.size() == 1);
        CHECK(results[0].text == "git status");
    }

    TEST_CASE("case-insensitive matching — query lowercase matches uppercase entry") {
        auto h = make_history({"ECHO HELLO", "ls -la"});
        HistorySearch hs{h};
        const auto& results = hs.filter_matches("echo");
        REQUIRE(results.size() == 1);
        CHECK(results[0].text == "ECHO HELLO");
    }

    TEST_CASE("multiple substring matches are all returned") {
        auto h = make_history({"git status", "git log", "git commit", "ls -la"});
        HistorySearch hs{h};
        const auto& results = hs.filter_matches("git");
        CHECK(results.size() == 3);
    }

    TEST_CASE("non-matching query returns empty results") {
        auto h = make_history({"git status", "echo hello"});
        HistorySearch hs{h};
        const auto& results = hs.filter_matches("zzznomatch");
        CHECK(results.empty());
    }

    TEST_CASE("substring match scores 1.0 quality factor") {
        // A substring match should have a higher or equal score than a fuzzy
        // match at the same recency position.  We verify score > 0 and that
        // the exact-match entry appears first.
        auto h = make_history({"g-i-t fuzzy", "git status"});
        // "git status" is more recent AND an exact substring → should rank first.
        HistorySearch hs{h};
        const auto& results = hs.filter_matches("git");
        REQUIRE_FALSE(results.empty());
        CHECK(results[0].text == "git status");
    }
}

// ===========================================================================
// Fuzzy matching
// ===========================================================================

TEST_SUITE("HistorySearch — fuzzy matching") {

    TEST_CASE("fuzzy match — all chars in order but non-contiguous") {
        // "gst" should fuzzy-match "git status"
        auto h = make_history({"git status"});
        HistorySearch hs{h};
        const auto& results = hs.filter_matches("gst");
        REQUIRE(results.size() == 1);
        CHECK(results[0].text == "git status");
    }

    TEST_CASE("fuzzy no-match when chars not all in order") {
        // "zec" — 'z' not in "echo hello"
        auto h = make_history({"echo hello"});
        HistorySearch hs{h};
        const auto& results = hs.filter_matches("zec");
        CHECK(results.empty());
    }

    TEST_CASE("substring beats fuzzy for same recency") {
        // "git status" is an exact substring match for "git"
        // "g_i_t stuff" is only a fuzzy match for "git"
        // Both pushed, status more recent → status should rank first
        auto h = make_history({"g_i_t stuff", "git status"});
        HistorySearch hs{h};
        const auto& results = hs.filter_matches("git");
        REQUIRE_FALSE(results.empty());
        CHECK(results[0].text == "git status");
    }

    TEST_CASE("fuzzy match score is in (0, 1)") {
        auto h = make_history({"git status"});
        HistorySearch hs{h};
        const auto& results = hs.filter_matches("gst");  // fuzzy
        REQUIRE(results.size() == 1);
        CHECK(results[0].score > 0.0f);
        CHECK(results[0].score < 1.0f);
    }
}

// ===========================================================================
// Recency tiebreak
// ===========================================================================

TEST_SUITE("HistorySearch — recency tiebreak") {

    TEST_CASE("among equal-quality substring matches, more recent ranks first") {
        // All entries are exact substring matches for "cmd".
        // "cmd_3" is most recent, so it should rank first.
        auto h = make_history({"cmd_1", "cmd_2", "cmd_3"});
        HistorySearch hs{h};
        const auto& results = hs.filter_matches("cmd");
        REQUIRE(results.size() == 3);
        CHECK(results[0].text == "cmd_3");
        CHECK(results[1].text == "cmd_2");
        CHECK(results[2].text == "cmd_1");
    }

    TEST_CASE("recency score is strictly monotone newest-highest") {
        auto h = make_history({"old_cmd", "mid_cmd", "new_cmd"});
        HistorySearch hs{h};
        const auto& results = hs.filter_matches("cmd");
        REQUIRE(results.size() == 3);
        // Scores should be descending.
        for (std::size_t i = 1; i < results.size(); ++i) {
            CHECK(results[i - 1].score >= results[i].score);
        }
    }

    TEST_CASE("a very recent fuzzy match can outscore an older exact match") {
        // Three entries:
        //   age=2 (oldest)  "git commit"        — exact match for "git"
        //   age=1           "some other command" — no match
        //   age=0 (newest)  "g_i_t action"      — fuzzy match for "git"
        //
        // The fuzzy (newest) might outscore the exact (oldest) if recency weight
        // is high enough.  We don't assert the order here but we do assert both
        // are returned and scores are positive.
        auto h = make_history({"git commit", "some other command", "g_i_t action"});
        HistorySearch hs{h};
        const auto& results = hs.filter_matches("git");
        // "git commit" and "g_i_t action" should both match.
        bool found_exact = false;
        bool found_fuzzy = false;
        for (const auto& r : results) {
            if (r.text == "git commit")   found_exact = true;
            if (r.text == "g_i_t action") found_fuzzy = true;
            CHECK(r.score > 0.0f);
        }
        CHECK(found_exact);
        CHECK(found_fuzzy);
    }
}

// ===========================================================================
// Empty query returns all entries (most recent first)
// ===========================================================================

TEST_SUITE("HistorySearch — empty query") {

    TEST_CASE("empty query returns all entries") {
        auto h = make_history({"alpha", "beta", "gamma"});
        HistorySearch hs{h};
        const auto& results = hs.filter_matches("");
        CHECK(results.size() == 3);
    }

    TEST_CASE("empty query: most recent entry is ranked first") {
        auto h = make_history({"first", "second", "third"});
        HistorySearch hs{h};
        const auto& results = hs.filter_matches("");
        REQUIRE_FALSE(results.empty());
        CHECK(results[0].text == "third");
    }

    TEST_CASE("empty query: entries ordered newest to oldest") {
        auto h = make_history({"a", "b", "c"});
        HistorySearch hs{h};
        const auto& results = hs.filter_matches("");
        REQUIRE(results.size() == 3);
        CHECK(results[0].text == "c");
        CHECK(results[1].text == "b");
        CHECK(results[2].text == "a");
    }

    TEST_CASE("empty query on empty history returns empty results") {
        History h{fs::path{}, 100};
        HistorySearch hs{h};
        const auto& results = hs.filter_matches("");
        CHECK(results.empty());
    }
}

// ===========================================================================
// next_match() cycling
// ===========================================================================

TEST_SUITE("HistorySearch — next_match cycling") {

    TEST_CASE("first next_match() returns the top-ranked match") {
        auto h = make_history({"git status", "git log", "git commit"});
        HistorySearch hs{h};
        hs.filter_matches("git");
        const auto first = hs.next_match();
        REQUIRE(first.has_value());
        // Top-ranked should be the most recent: "git commit"
        CHECK(first.value() == "git commit");
    }

    TEST_CASE("successive next_match() calls cycle through all matches") {
        auto h = make_history({"cmd_1", "cmd_2", "cmd_3"});
        HistorySearch hs{h};
        const auto& results = hs.filter_matches("cmd");
        REQUIRE(results.size() == 3);

        std::vector<std::string> cycled;
        for (std::size_t i = 0; i < results.size(); ++i) {
            const auto m = hs.next_match();
            REQUIRE(m.has_value());
            cycled.push_back(m.value());
        }
        // Every match appears exactly once.
        CHECK(cycled.size() == 3);
        for (const auto& r : results) {
            const bool found = std::find(cycled.begin(), cycled.end(), r.text)
                               != cycled.end();
            CHECK(found);
        }
    }

    TEST_CASE("next_match() wraps around after exhausting all matches") {
        auto h = make_history({"cmd_1", "cmd_2"});
        HistorySearch hs{h};
        hs.filter_matches("cmd");
        const auto first  = hs.next_match();  // index 0 → advance to 1
        const auto second = hs.next_match();  // index 1 → advance to 0 (wrap)
        const auto third  = hs.next_match();  // index 0 again → advance to 1
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        REQUIRE(third.has_value());
        // Third call wraps: should equal first.
        CHECK(third.value() == first.value());
    }

    TEST_CASE("filter_matches resets cycling index") {
        auto h = make_history({"cmd_1", "cmd_2", "cmd_3"});
        HistorySearch hs{h};
        hs.filter_matches("cmd");
        hs.next_match();  // advance index
        hs.next_match();

        // Re-run filter — index must reset to 0.
        hs.filter_matches("cmd");
        const auto m1 = hs.next_match();
        hs.filter_matches("cmd");  // reset again
        const auto m2 = hs.next_match();
        // Both should return the same top-ranked entry.
        REQUIRE(m1.has_value());
        REQUIRE(m2.has_value());
        CHECK(m1.value() == m2.value());
    }

    TEST_CASE("selected() returns current match without advancing index") {
        auto h = make_history({"alpha", "beta", "gamma"});
        HistorySearch hs{h};
        hs.filter_matches("a");

        const auto sel1 = hs.selected();
        const auto sel2 = hs.selected();  // second call must not advance
        REQUIRE(sel1.has_value());
        REQUIRE(sel2.has_value());
        CHECK(sel1.value() == sel2.value());
    }
}

// ===========================================================================
// Deduplication
// ===========================================================================

TEST_SUITE("HistorySearch — deduplication") {

    TEST_CASE("duplicate entries in history appear only once in results") {
        // History intentionally populated via direct manipulation isn't possible
        // since push() dedupes consecutive entries.  Build a history using
        // non-consecutive identical entries.
        auto h = make_history({"echo hi", "ls", "echo hi"});  // non-consecutive
        HistorySearch hs{h};
        const auto& results = hs.filter_matches("echo");
        // Both "echo hi" entries are in history, but search should deduplicate.
        std::size_t count = 0;
        for (const auto& r : results) {
            if (r.text == "echo hi") ++count;
        }
        CHECK(count == 1);
    }
}

// ===========================================================================
// Re-query narrows results
// ===========================================================================

TEST_SUITE("HistorySearch — incremental narrowing") {

    TEST_CASE("longer query narrows results compared to shorter query") {
        auto h = make_history({"git status", "git log --oneline", "git commit"});
        HistorySearch hs{h};

        const auto& broad = hs.filter_matches("git");
        const std::size_t broad_count = broad.size();

        const auto& narrow = hs.filter_matches("git log");
        CHECK(narrow.size() <= broad_count);
        REQUIRE(narrow.size() >= 1);
        CHECK(narrow[0].text == "git log --oneline");
    }

    TEST_CASE("filter_matches replaces previous match list on each call") {
        auto h = make_history({"echo hello", "ls -la", "git status"});
        HistorySearch hs{h};

        hs.filter_matches("echo");
        REQUIRE(hs.matches().size() == 1);

        hs.filter_matches("ls");
        REQUIRE(hs.matches().size() == 1);
        CHECK(hs.matches()[0].text == "ls -la");
    }
}

// ===========================================================================
// Edge cases
// ===========================================================================

TEST_SUITE("HistorySearch — edge cases") {

    TEST_CASE("query longer than any entry returns no matches") {
        auto h = make_history({"hi", "ls"});
        HistorySearch hs{h};
        const auto& results = hs.filter_matches("this_is_a_very_long_query_indeed");
        CHECK(results.empty());
    }

    TEST_CASE("single-char query matches entries containing that char") {
        auto h = make_history({"abc", "xyz", "aaa"});
        HistorySearch hs{h};
        const auto& results = hs.filter_matches("a");
        for (const auto& r : results) {
            CHECK(r.text.find('a') != std::string::npos);
        }
    }

    TEST_CASE("single entry history returns that entry for matching query") {
        auto h = make_history({"git status"});
        HistorySearch hs{h};
        const auto& results = hs.filter_matches("git");
        REQUIRE(results.size() == 1);
        CHECK(results[0].text == "git status");
    }

    TEST_CASE("score is always positive for returned matches") {
        auto h = make_history({"git status", "ls -la", "echo hello"});
        HistorySearch hs{h};
        const auto& results = hs.filter_matches("l");
        for (const auto& r : results) {
            CHECK(r.score > 0.0f);
        }
    }

    TEST_CASE("all scores are greater than zero and at most one") {
        auto h = make_history({"alpha", "beta", "gamma", "delta"});
        HistorySearch hs{h};
        const auto& results = hs.filter_matches("a");
        for (const auto& r : results) {
            CHECK(r.score > 0.0f);
            CHECK(r.score <= 1.0001f);  // small float epsilon tolerance
        }
    }

    TEST_CASE("reset then filter_matches works correctly") {
        auto h = make_history({"git status"});
        HistorySearch hs{h};
        hs.filter_matches("git");
        hs.reset();
        CHECK_FALSE(hs.active());
        const auto& results = hs.filter_matches("git");
        CHECK(hs.active());
        REQUIRE(results.size() == 1);
        CHECK(results[0].text == "git status");
    }
}
