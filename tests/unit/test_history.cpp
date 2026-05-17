// tests/unit/test_history.cpp
// ---------------------------------------------------------------------------
// Unit tests for batbox::repl::History.
//
// Build + run (standalone, no CMake needed):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_history.cpp src/repl/History.cpp src/core/Paths.cpp \
//       -o /tmp/test_hist && /tmp/test_hist
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "batbox/repl/History.hpp"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using batbox::repl::History;

// ---------------------------------------------------------------------------
// RAII helper: creates a unique temporary directory and removes it on scope exit.
// ---------------------------------------------------------------------------
namespace {

struct TmpDir {
    fs::path path;

    TmpDir() {
        // Use a process-global counter for uniqueness — no thread/chrono needed.
        static std::atomic<int> counter{0};
        const int id = counter.fetch_add(1, std::memory_order_relaxed);
        path = fs::temp_directory_path() /
               ("batbox_hist_test_" + std::to_string(id));
        fs::create_directories(path);
    }

    ~TmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    fs::path file(const char* name = "history") const {
        return path / name;
    }

    // Non-copyable.
    TmpDir(const TmpDir&)            = delete;
    TmpDir& operator=(const TmpDir&) = delete;
};

// RAII environment variable guard.
struct EnvGuard {
    std::string key;
    bool had_previous{false};
    std::string previous_value;

    explicit EnvGuard(const char* k, const char* v) : key{k} {
        const char* existing = std::getenv(k);
        if (existing != nullptr) {
            had_previous = true;
            previous_value = existing;
        }
        ::setenv(k, v, /*overwrite=*/1);
    }
    ~EnvGuard() {
        if (had_previous) {
            ::setenv(key.c_str(), previous_value.c_str(), 1);
        } else {
            ::unsetenv(key.c_str());
        }
    }
    EnvGuard(const EnvGuard&)            = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// Basic push / at / size semantics
// ---------------------------------------------------------------------------

TEST_SUITE("History — push and at") {
    TEST_CASE("starts empty") {
        TmpDir tmp;
        History h{tmp.file(), 100};
        CHECK(h.size() == 0);
    }

    TEST_CASE("push increases size by one") {
        TmpDir tmp;
        History h{tmp.file(), 100};
        h.push("echo hello");
        CHECK(h.size() == 1);
    }

    TEST_CASE("at(0) returns most recently pushed entry") {
        TmpDir tmp;
        History h{tmp.file(), 100};
        h.push("first");
        h.push("second");
        h.push("third");
        CHECK(h.at(0).value() == "third");
    }

    TEST_CASE("at(1) returns second-most-recent entry") {
        TmpDir tmp;
        History h{tmp.file(), 100};
        h.push("alpha");
        h.push("beta");
        CHECK(h.at(1).value() == "alpha");
    }

    TEST_CASE("at(age >= size) returns nullopt") {
        TmpDir tmp;
        History h{tmp.file(), 100};
        h.push("only entry");
        CHECK_FALSE(h.at(1).has_value());
        CHECK_FALSE(h.at(100).has_value());
    }

    TEST_CASE("multiple entries stored in correct order") {
        TmpDir tmp;
        History h{tmp.file(), 100};
        h.push("a");
        h.push("b");
        h.push("c");
        CHECK(h.at(0).value() == "c");
        CHECK(h.at(1).value() == "b");
        CHECK(h.at(2).value() == "a");
    }
}

// ---------------------------------------------------------------------------
// Deduplication
// ---------------------------------------------------------------------------

TEST_SUITE("History — consecutive dedup") {
    TEST_CASE("consecutive identical pushes are collapsed to one") {
        TmpDir tmp;
        History h{tmp.file(), 100};
        h.push("ls");
        h.push("ls");
        h.push("ls");
        CHECK(h.size() == 1);
        CHECK(h.at(0).value() == "ls");
    }

    TEST_CASE("non-consecutive identical entries are both stored") {
        TmpDir tmp;
        History h{tmp.file(), 100};
        h.push("ls");
        h.push("pwd");
        h.push("ls");
        CHECK(h.size() == 3);
        CHECK(h.at(0).value() == "ls");
        CHECK(h.at(1).value() == "pwd");
        CHECK(h.at(2).value() == "ls");
    }

    TEST_CASE("empty string is never stored") {
        TmpDir tmp;
        History h{tmp.file(), 100};
        h.push("");
        h.push("  ");
        h.push("\t");
        CHECK(h.size() == 0);
    }

    TEST_CASE("whitespace-only string is never stored") {
        TmpDir tmp;
        History h{tmp.file(), 100};
        h.push("   ");
        h.push("\t\t");
        CHECK(h.size() == 0);
    }
}

// ---------------------------------------------------------------------------
// Cap / ring-buffer rollover
// ---------------------------------------------------------------------------

TEST_SUITE("History — cap rollover") {
    TEST_CASE("size never exceeds cap") {
        TmpDir tmp;
        constexpr std::size_t kCap = 5;
        History h{tmp.file(), kCap};
        for (int i = 0; i < 20; ++i) {
            h.push("cmd_" + std::to_string(i));
        }
        CHECK(h.size() == kCap);
    }

    TEST_CASE("oldest entries are evicted on rollover") {
        TmpDir tmp;
        constexpr std::size_t kCap = 3;
        History h{tmp.file(), kCap};
        h.push("old1");
        h.push("old2");
        h.push("new1");
        h.push("new2");  // evicts "old1"
        h.push("new3");  // evicts "old2"

        // Should contain new1, new2, new3.
        CHECK(h.size() == kCap);
        CHECK(h.at(0).value() == "new3");
        CHECK(h.at(1).value() == "new2");
        CHECK(h.at(2).value() == "new1");
    }

    TEST_CASE("cap() accessor returns configured value") {
        TmpDir tmp;
        History h{tmp.file(), 42};
        CHECK(h.cap() == 42);
    }
}

// ---------------------------------------------------------------------------
// clear()
// ---------------------------------------------------------------------------

TEST_SUITE("History — clear") {
    TEST_CASE("clear removes all entries") {
        TmpDir tmp;
        History h{tmp.file(), 100};
        h.push("a");
        h.push("b");
        h.clear();
        CHECK(h.size() == 0);
    }

    TEST_CASE("after clear, at(0) returns nullopt") {
        TmpDir tmp;
        History h{tmp.file(), 100};
        h.push("x");
        h.clear();
        CHECK_FALSE(h.at(0).has_value());
    }

    TEST_CASE("push works normally after clear") {
        TmpDir tmp;
        History h{tmp.file(), 100};
        h.push("before");
        h.clear();
        h.push("after");
        CHECK(h.size() == 1);
        CHECK(h.at(0).value() == "after");
    }
}

// ---------------------------------------------------------------------------
// Navigation: previous() / next() / reset_cursor()
// ---------------------------------------------------------------------------

TEST_SUITE("History — navigation") {
    TEST_CASE("previous() on empty history returns nullopt") {
        TmpDir tmp;
        History h{tmp.file(), 100};
        CHECK_FALSE(h.previous().has_value());
    }

    TEST_CASE("previous() returns most recent entry on first call") {
        TmpDir tmp;
        History h{tmp.file(), 100};
        h.push("cmd1");
        h.push("cmd2");
        h.push("cmd3");
        CHECK(h.previous().value() == "cmd3");
    }

    TEST_CASE("successive previous() calls walk backward") {
        TmpDir tmp;
        History h{tmp.file(), 100};
        h.push("a");
        h.push("b");
        h.push("c");
        CHECK(h.previous().value() == "c");
        CHECK(h.previous().value() == "b");
        CHECK(h.previous().value() == "a");
    }

    TEST_CASE("previous() does not go past the oldest entry") {
        TmpDir tmp;
        History h{tmp.file(), 100};
        h.push("only");
        CHECK(h.previous().value() == "only");
        // Calling previous() again when already at oldest should return oldest.
        CHECK(h.previous().value() == "only");
    }

    TEST_CASE("next() after walking back goes forward") {
        TmpDir tmp;
        History h{tmp.file(), 100};
        h.push("a");
        h.push("b");
        h.push("c");
        h.previous();  // → c
        h.previous();  // → b
        h.previous();  // → a
        CHECK(h.next().value() == "b");
        CHECK(h.next().value() == "c");
    }

    TEST_CASE("next() at the end returns nullopt (live-input position)") {
        TmpDir tmp;
        History h{tmp.file(), 100};
        h.push("x");
        h.previous();  // → x (cursor at 0)
        CHECK(h.next() == std::nullopt);  // back past the end
    }

    TEST_CASE("reset_cursor() brings cursor back to past-the-end") {
        TmpDir tmp;
        History h{tmp.file(), 100};
        h.push("first");
        h.push("second");
        h.previous();        // cursor moves back
        h.reset_cursor();
        // After reset, previous() should return the most recent entry again.
        CHECK(h.previous().value() == "second");
    }

    TEST_CASE("push() resets cursor to past-the-end") {
        TmpDir tmp;
        History h{tmp.file(), 100};
        h.push("a");
        h.push("b");
        h.previous();   // cursor at b
        h.previous();   // cursor at a
        h.push("c");    // should reset cursor
        // Next previous() should return "c" (the newest).
        CHECK(h.previous().value() == "c");
    }
}

// ---------------------------------------------------------------------------
// save() + load() round-trip
// ---------------------------------------------------------------------------

TEST_SUITE("History — save and load round-trip") {
    TEST_CASE("saved entries can be loaded back") {
        TmpDir tmp;
        const fs::path hfile = tmp.file("hist1");

        {
            History h{hfile, 100};
            h.push("echo foo");
            h.push("ls -la");
            h.push("git status");
            h.save();
        }

        History h2{hfile, 100};
        CHECK(h2.size() == 3);
        CHECK(h2.at(0).value() == "git status");
        CHECK(h2.at(1).value() == "ls -la");
        CHECK(h2.at(2).value() == "echo foo");
    }

    TEST_CASE("save creates parent directories if needed") {
        TmpDir tmp;
        const fs::path nested_dir  = tmp.path / "a" / "b" / "c";
        const fs::path nested_file = nested_dir / "history";

        History h{nested_file, 100};
        h.push("hello");
        CHECK_NOTHROW(h.save());
        CHECK(fs::exists(nested_file));
    }

    TEST_CASE("load on non-existent file is a no-op (no throw)") {
        TmpDir tmp;
        const fs::path nonexistent = tmp.path / "no_such_file";
        CHECK_NOTHROW(History{nonexistent, 100});
    }

    TEST_CASE("empty history file saves and reloads as zero entries") {
        TmpDir tmp;
        const fs::path hfile = tmp.file("hist_empty");

        {
            History h{hfile, 100};
            // No pushes.
            h.save();
        }

        History h2{hfile, 100};
        CHECK(h2.size() == 0);
    }

    TEST_CASE("save is atomic — tmp file is cleaned up on success") {
        TmpDir tmp;
        const fs::path hfile    = tmp.file("hist_atomic");
        const fs::path tmp_file = fs::path{hfile}.concat(".tmp");

        History h{hfile, 100};
        h.push("line1");
        h.save();

        // After successful save, the .tmp file must not exist.
        CHECK_FALSE(fs::exists(tmp_file));
        CHECK(fs::exists(hfile));
    }

    TEST_CASE("load respects cap: keeps newest entries") {
        TmpDir tmp;
        const fs::path hfile = tmp.file("hist_cap");

        // Write 10 entries directly to the file.
        {
            std::ofstream out{hfile};
            for (int i = 1; i <= 10; ++i) {
                out << "cmd_" << i << '\n';
            }
        }

        // Load with cap=5: should keep cmd_6 … cmd_10 (newest 5).
        History h{hfile, 5};
        CHECK(h.size() == 5);
        CHECK(h.at(0).value() == "cmd_10");
        CHECK(h.at(4).value() == "cmd_6");
    }

    TEST_CASE("blank lines in history file are skipped on load") {
        TmpDir tmp;
        const fs::path hfile = tmp.file("hist_blanks");

        {
            std::ofstream out{hfile};
            out << "line1\n";
            out << "\n";          // blank
            out << "   \n";       // whitespace-only
            out << "line2\n";
        }

        History h{hfile, 100};
        CHECK(h.size() == 2);
        CHECK(h.at(0).value() == "line2");
        CHECK(h.at(1).value() == "line1");
    }

    TEST_CASE("save then load preserves order across multiple push cycles") {
        TmpDir tmp;
        const fs::path hfile = tmp.file("hist_order");

        {
            History h{hfile, 100};
            h.push("first");
            h.push("second");
            h.push("third");
            h.save();
        }

        {
            History h{hfile, 100};
            // After loading, push more entries.
            h.push("fourth");
            h.push("fifth");
            h.save();
        }

        History h_final{hfile, 100};
        CHECK(h_final.size() == 5);
        CHECK(h_final.at(0).value() == "fifth");
        CHECK(h_final.at(1).value() == "fourth");
        CHECK(h_final.at(2).value() == "third");
        CHECK(h_final.at(3).value() == "second");
        CHECK(h_final.at(4).value() == "first");
    }
}

// ---------------------------------------------------------------------------
// Persistence disabled (empty path)
// ---------------------------------------------------------------------------

TEST_SUITE("History — persistence disabled") {
    TEST_CASE("empty persist_file disables save and load (no throw, no file)") {
        History h{fs::path{}, 100};
        h.push("will not persist");
        CHECK_NOTHROW(h.save());
        CHECK(h.persist_file().empty());
    }
}

// ---------------------------------------------------------------------------
// BATBOX_HISTORY_SIZE env-var override
// ---------------------------------------------------------------------------

TEST_SUITE("History — BATBOX_HISTORY_SIZE env override") {
    TEST_CASE("BATBOX_HISTORY_SIZE caps entries") {
        TmpDir tmp;
        // Set cap to 3 via env var.
        EnvGuard guard{"BATBOX_HISTORY_SIZE", "3"};
        History h{tmp.file(), 1000};  // constructor cap ignored when env set
        for (int i = 0; i < 10; ++i) {
            h.push("e" + std::to_string(i));
        }
        CHECK(h.size() == 3);
    }

    TEST_CASE("invalid BATBOX_HISTORY_SIZE falls back to constructor cap") {
        TmpDir tmp;
        EnvGuard guard{"BATBOX_HISTORY_SIZE", "notanumber"};
        History h{tmp.file(), 7};
        CHECK(h.cap() == 7);
    }
}
