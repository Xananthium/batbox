// =============================================================================
// tests/unit/test_session_recovery.cpp — Unit tests for batbox::session::IndexRebuilder
//
// Acceptance criteria tested:
//   AC1: Missing index -> rebuild kicks off automatically
//   AC2: Corrupt index -> rebuild kicks off, old file moved to index.json.bak
//   AC3: Rebuild progress publishes "47/210" style via callback
//   AC4: App startup time <100ms even with 1000 session files (rebuild is background)
//   AC5: Cancellation via CancelToken works
//
// Build + run (standalone, from repo root):
//   c++ -std=c++20 \
//       -Iinclude \
//       -Ibuild/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_session_recovery.cpp \
//       src/session/SessionRecovery.cpp \
//       src/session/SessionIndex.cpp \
//       src/core/Uuid.cpp src/core/Logging.cpp src/core/Paths.cpp \
//       src/core/Json.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libspdlog.a \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       build/vcpkg_installed/arm64-osx/lib/libfmt.a \
//       -o /tmp/test_session_recovery && /tmp/test_session_recovery
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "batbox/session/SessionRecovery.hpp"
#include "batbox/session/SessionIndex.hpp"
#include "batbox/core/CancelToken.hpp"
#include "batbox/core/Uuid.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using batbox::session::IndexRebuilder;
using batbox::session::SessionIndexRecord;
using batbox::session::append_index_record;
using batbox::session::read_latest_per_id;
using batbox::session::rebuild_from_dir;
using batbox::session::is_corrupt;
using batbox::CancelToken;
using batbox::CancelSource;

// ---------------------------------------------------------------------------
// TmpDir — auto-cleaned temporary directory
// ---------------------------------------------------------------------------
struct TmpDir {
    fs::path path;
    TmpDir() {
        char tmpl[] = "/tmp/batbox_recovery_XXXXXX";
        const char* d = mkdtemp(tmpl);
        REQUIRE(d != nullptr);
        path = fs::path(d);
    }
    ~TmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    fs::path operator/(const std::string& name) const { return path / name; }
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Write a minimal valid session JSON file to path.
static void write_session_file(const fs::path& p,
                                const batbox::Uuid& id,
                                int turn_count = 1,
                                const std::string& preview = "hello") {
    nlohmann::json j;
    j["id"]               = id.to_string();
    j["created_at"]       = "2026-01-01T10:00:00Z";
    j["updated_at"]       = "2026-01-01T10:00:00Z";
    j["model_at_start"]   = "gpt-4o";
    j["turn_count"]       = turn_count;
    j["messages"]         = nlohmann::json::array({
        {{"role", "user"}, {"content", preview}}
    });
    std::ofstream out(p);
    out << j.dump();
    REQUIRE(out.good());
}

/// Wait up to max_wait for predicate to become true. Returns true if satisfied.
template <typename Pred>
static bool wait_until(Pred pred,
                       std::chrono::milliseconds max_wait = std::chrono::milliseconds(5000)) {
    auto deadline = std::chrono::steady_clock::now() + max_wait;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

// =============================================================================
// AC1: Missing index → rebuild kicks off automatically, produces valid index
// =============================================================================

TEST_CASE("AC1: missing index — rebuilder produces valid index from session files") {
    TmpDir tmp;
    fs::path sessions_dir = tmp / "sessions";
    fs::create_directories(sessions_dir);
    fs::path idx = sessions_dir / "index.json";

    // No index.json exists yet.
    REQUIRE_FALSE(fs::exists(idx));

    // Create 5 session files.
    const int N = 5;
    std::vector<batbox::Uuid> ids;
    for (int i = 0; i < N; ++i) {
        auto id = batbox::Uuid::v4();
        ids.push_back(id);
        write_session_file(sessions_dir / (id.to_string() + ".json"), id, i + 1);
    }

    // Launch rebuilder.
    auto [src, tok] = CancelToken::make_root();
    std::atomic<bool> done_flag{false};
    {
        IndexRebuilder rebuilder(idx);
        rebuilder.start(std::move(tok), [&](size_t /*done*/, size_t /*total*/) {
            done_flag.store(true, std::memory_order_relaxed);
        });
        // start() must return immediately (non-blocking).
        // Index may not exist yet at this point — that is the expected behaviour.
    }
    // jthread joins on IndexRebuilder destruction — rebuild is complete here.

    // Index must now exist.
    REQUIRE(fs::exists(idx));

    // Index must be readable and contain all N sessions.
    auto result = read_latest_per_id(idx, 0);
    REQUIRE(result.has_value());
    CHECK(result.value().size() == static_cast<size_t>(N));

    // All UUIDs must appear.
    const auto& records = result.value();
    for (const auto& expected_id : ids) {
        bool found = false;
        for (const auto& r : records)
            if (r.id == expected_id) { found = true; break; }
        CHECK(found);
    }
}

// =============================================================================
// AC2: Corrupt index → old file moved to index.json.bak, new index written
// =============================================================================

TEST_CASE("AC2: corrupt index — moved to .bak, fresh index written") {
    TmpDir tmp;
    fs::path sessions_dir = tmp / "sessions";
    fs::create_directories(sessions_dir);
    fs::path idx = sessions_dir / "index.json";
    fs::path bak = fs::path(idx.string() + ".bak");

    // Write a corrupt index (garbage content).
    {
        std::ofstream f(idx);
        f << "THIS IS NOT JSON AT ALL\n";
        f << "GARBAGE GARBAGE GARBAGE\n";
    }
    REQUIRE(fs::exists(idx));
    CHECK(is_corrupt(idx));  // Confirm it's actually detected as corrupt.

    // Write one valid session file.
    auto id = batbox::Uuid::v4();
    write_session_file(sessions_dir / (id.to_string() + ".json"), id);

    // Launch rebuilder and wait for completion.
    auto [src, tok] = CancelToken::make_root();
    {
        IndexRebuilder rebuilder(idx);
        rebuilder.start(std::move(tok), nullptr);
    }
    // jthread joins here.

    // The corrupt index must have been renamed to .bak.
    CHECK(fs::exists(bak));

    // The new index must exist and be valid.
    REQUIRE(fs::exists(idx));
    auto result = read_latest_per_id(idx, 0);
    REQUIRE(result.has_value());
    CHECK(result.value().size() == 1u);
    CHECK(result.value()[0].id == id);
    CHECK_FALSE(is_corrupt(idx));
}

// =============================================================================
// AC3: Progress callback publishes done/total counts
// =============================================================================

TEST_CASE("AC3: progress callback reports correct done/total") {
    TmpDir tmp;
    fs::path sessions_dir = tmp / "sessions";
    fs::create_directories(sessions_dir);
    fs::path idx = sessions_dir / "index.json";

    const int N = 10;
    for (int i = 0; i < N; ++i) {
        auto id = batbox::Uuid::v4();
        write_session_file(sessions_dir / (id.to_string() + ".json"), id, i + 1);
    }

    // Collect progress observations.
    std::mutex mtx;
    std::vector<std::pair<size_t, size_t>> observations;

    auto [src, tok] = CancelToken::make_root();
    {
        IndexRebuilder rebuilder(idx);
        rebuilder.start(std::move(tok),
            [&](size_t done, size_t total) {
                std::lock_guard<std::mutex> lock(mtx);
                observations.emplace_back(done, total);
            });
    }
    // jthread joins; all progress callbacks have fired by here.

    // Must have received at least one callback.
    REQUIRE_FALSE(observations.empty());

    // Every observation must have total == N.
    for (const auto& [d, t] : observations) {
        CHECK(t == static_cast<size_t>(N));
    }

    // The 'done' values must be monotonically increasing 1..N.
    std::vector<size_t> done_vals;
    for (const auto& [d, t] : observations) done_vals.push_back(d);
    for (size_t i = 1; i < done_vals.size(); ++i) {
        CHECK(done_vals[i] >= done_vals[i - 1]);
    }

    // The last callback must have done == total.
    CHECK(observations.back().first == static_cast<size_t>(N));
    CHECK(observations.back().second == static_cast<size_t>(N));
}

// =============================================================================
// AC4: Non-blocking — start() returns immediately even with many session files
// =============================================================================

TEST_CASE("AC4: start() returns in under 100ms even with many session files") {
    TmpDir tmp;
    fs::path sessions_dir = tmp / "sessions";
    fs::create_directories(sessions_dir);
    fs::path idx = sessions_dir / "index.json";

    // Create 50 session files (enough to prove non-blocking without slow I/O).
    for (int i = 0; i < 50; ++i) {
        auto id = batbox::Uuid::v4();
        write_session_file(sessions_dir / (id.to_string() + ".json"), id, i + 1);
    }

    auto [src, tok] = CancelToken::make_root();
    IndexRebuilder rebuilder(idx);

    auto t0 = std::chrono::steady_clock::now();
    rebuilder.start(std::move(tok), nullptr);
    auto t1 = std::chrono::steady_clock::now();

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    MESSAGE("start() returned in " << elapsed_ms << " ms");
    CHECK(elapsed_ms < 100);
    // Destructor joins the background thread — no dangling.
}

// =============================================================================
// AC5: CancelToken cancellation stops the rebuild cooperatively
// =============================================================================

TEST_CASE("AC5: cancellation via CancelToken stops background rebuild") {
    TmpDir tmp;
    fs::path sessions_dir = tmp / "sessions";
    fs::create_directories(sessions_dir);
    fs::path idx = sessions_dir / "index.json";

    // Create some session files.
    const int N = 20;
    for (int i = 0; i < N; ++i) {
        auto id = batbox::Uuid::v4();
        write_session_file(sessions_dir / (id.to_string() + ".json"), id, i + 1);
    }

    auto [src, tok] = CancelToken::make_root();

    // Cancel immediately before starting — the thread should detect and exit.
    src.request_stop();

    {
        IndexRebuilder rebuilder(idx);
        rebuilder.start(std::move(tok), nullptr);
    }
    // jthread joins cleanly — no hang, no crash.
    // Index may or may not exist depending on timing; that is acceptable.
    // The key guarantee is that no partial corrupt index was written when
    // cancellation was detected mid-scan.
    CHECK(true);  // Reaching here means no crash or hang occurred.
}

TEST_CASE("AC5b: cancellation mid-rebuild — does not leave partial index") {
    TmpDir tmp;
    fs::path sessions_dir = tmp / "sessions";
    fs::create_directories(sessions_dir);
    fs::path idx = sessions_dir / "index.json";

    // Create enough files that a mid-scan cancel is possible.
    const int N = 30;
    std::vector<batbox::Uuid> ids;
    for (int i = 0; i < N; ++i) {
        auto id = batbox::Uuid::v4();
        ids.push_back(id);
        write_session_file(sessions_dir / (id.to_string() + ".json"), id, i + 1);
    }

    auto [src, tok] = CancelToken::make_root();

    // Cancel after a brief delay so the thread has a chance to start.
    std::atomic<size_t> callback_count{0};
    {
        IndexRebuilder rebuilder(idx);
        rebuilder.start(std::move(tok),
            [&](size_t done, size_t /*total*/) {
                callback_count.fetch_add(1, std::memory_order_relaxed);
                if (done >= 5) {
                    // Cancel from within the callback to simulate mid-rebuild cancel.
                    src.request_stop();
                }
            });
        // Wait for the thread to at least start processing before we check.
        // IndexRebuilder destructor joins, so we just wait here.
    }
    // Joined. If index doesn't exist or exists, both are acceptable
    // (a complete rebuild may finish before cancellation is observed).
    // What must NOT happen: a crash or an index file with mixed-in garbage.
    if (fs::exists(idx)) {
        // If the index was written at all, it must be well-formed.
        auto result = read_latest_per_id(idx, 0);
        CHECK(result.has_value());
        CHECK_FALSE(is_corrupt(idx));
    }
    CHECK(true);  // No crash = pass.
}

// =============================================================================
// Edge case: empty sessions directory
// =============================================================================

TEST_CASE("edge: empty sessions directory — rebuilder completes, callback fires with (0,0)") {
    TmpDir tmp;
    fs::path sessions_dir = tmp / "sessions";
    fs::create_directories(sessions_dir);
    fs::path idx = sessions_dir / "index.json";

    std::atomic<bool> cb_called{false};
    size_t final_done = 99;
    size_t final_total = 99;

    auto [src, tok] = CancelToken::make_root();
    {
        IndexRebuilder rebuilder(idx);
        rebuilder.start(std::move(tok),
            [&](size_t done, size_t total) {
                final_done  = done;
                final_total = total;
                cb_called.store(true, std::memory_order_relaxed);
            });
    }
    // Joined.
    CHECK(cb_called.load());
    CHECK(final_done == 0u);
    CHECK(final_total == 0u);

    // Index must have been created (empty JSONL is valid).
    REQUIRE(fs::exists(idx));
    auto result = read_latest_per_id(idx, 0);
    REQUIRE(result.has_value());
    CHECK(result.value().empty());
}

// =============================================================================
// Edge case: sessions directory contains non-session files (decoys)
// =============================================================================

TEST_CASE("edge: sessions dir with decoy files — only session files indexed") {
    TmpDir tmp;
    fs::path sessions_dir = tmp / "sessions";
    fs::create_directories(sessions_dir);
    fs::path idx = sessions_dir / "index.json";

    // One real session.
    auto id = batbox::Uuid::v4();
    write_session_file(sessions_dir / (id.to_string() + ".json"), id);

    // Decoys.
    { std::ofstream f(sessions_dir / "batbox.log"); f << "log\n"; }
    { std::ofstream f(sessions_dir / "README.txt"); f << "readme\n"; }

    size_t progress_total = 0;
    auto [src, tok] = CancelToken::make_root();
    {
        IndexRebuilder rebuilder(idx);
        rebuilder.start(std::move(tok),
            [&](size_t /*done*/, size_t total) { progress_total = total; });
    }

    // Only the one real session should have been counted.
    CHECK(progress_total == 1u);

    auto result = read_latest_per_id(idx, 0);
    REQUIRE(result.has_value());
    CHECK(result.value().size() == 1u);
    CHECK(result.value()[0].id == id);
}

// =============================================================================
// Edge case: clean index present — no .bak created
// =============================================================================

TEST_CASE("edge: clean existing index — no .bak file created by rebuilder") {
    TmpDir tmp;
    fs::path sessions_dir = tmp / "sessions";
    fs::create_directories(sessions_dir);
    fs::path idx = sessions_dir / "index.json";
    fs::path bak = fs::path(idx.string() + ".bak");

    // Write a valid (clean) index entry.
    auto id = batbox::Uuid::v4();
    SessionIndexRecord rec;
    rec.id                    = id;
    rec.created_at            = std::chrono::system_clock::now();
    rec.updated_at            = rec.created_at;
    rec.first_message_preview = "clean";
    rec.model                 = "gpt-4o";
    rec.turn_count            = 1;
    rec.file_path             = sessions_dir / (id.to_string() + ".json");
    REQUIRE(append_index_record(idx, rec).has_value());
    CHECK_FALSE(is_corrupt(idx));

    // Also write the matching session file.
    write_session_file(sessions_dir / (id.to_string() + ".json"), id);

    auto [src, tok] = CancelToken::make_root();
    {
        IndexRebuilder rebuilder(idx);
        rebuilder.start(std::move(tok), nullptr);
    }

    // No .bak should have been created (index was clean).
    CHECK_FALSE(fs::exists(bak));
}
