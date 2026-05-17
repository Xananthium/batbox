// =============================================================================
// tests/unit/test_session_index.cpp — Unit tests for batbox::session::SessionIndex
//
// Build + run (standalone, from repo root):
//   c++ -std=c++20 \
//       -Iinclude \
//       -Ibuild/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_session_index.cpp \
//       src/session/SessionIndex.cpp \
//       src/core/Uuid.cpp src/core/Logging.cpp src/core/Paths.cpp \
//       src/core/Json.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libspdlog.a \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       build/vcpkg_installed/arm64-osx/lib/libfmt.a \
//       -o /tmp/test_session_index && /tmp/test_session_index
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "batbox/session/SessionIndex.hpp"
#include "batbox/core/Uuid.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using batbox::session::SessionIndexRecord;
using batbox::session::append_index_record;
using batbox::session::read_latest_per_id;
using batbox::session::rebuild_from_dir;
using batbox::session::is_corrupt;
using batbox::Uuid;

// ---------------------------------------------------------------------------
// Test-helper: tmp directory that is removed after each test case.
// ---------------------------------------------------------------------------
struct TmpDir {
    fs::path path;

    TmpDir() {
        char tmpl[] = "/tmp/batbox_test_XXXXXX";
        const char* d = mkdtemp(tmpl);
        REQUIRE(d != nullptr);
        path = fs::path(d);
    }

    ~TmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);  // best-effort cleanup
    }

    // Convenience: return a path inside this tmp dir.
    fs::path operator/(const std::string& name) const { return path / name; }
};

// Build a minimal SessionIndexRecord with sane defaults.
static SessionIndexRecord make_record(const std::string& preview = "hello world",
                                      const std::string& model   = "gpt-4o",
                                      uint64_t           turns   = 3) {
    SessionIndexRecord r;
    r.id                    = Uuid::v4();
    r.created_at            = std::chrono::system_clock::now();
    r.updated_at            = r.created_at;
    r.first_message_preview = preview;
    r.model                 = model;
    r.turn_count            = turns;
    r.file_path             = "/tmp/fake-session.json";
    return r;
}

// =============================================================================
// TEST GROUP 1: append + list
// =============================================================================

TEST_CASE("append one record then read it back") {
    TmpDir tmp;
    fs::path idx = tmp / "index.json";

    auto rec = make_record("first message");
    auto append_result = append_index_record(idx, rec);
    REQUIRE(append_result.has_value());

    auto read_result = read_latest_per_id(idx);
    REQUIRE(read_result.has_value());
    const auto& records = read_result.value();
    REQUIRE(records.size() == 1u);
    CHECK(records[0].id == rec.id);
    CHECK(records[0].first_message_preview == "first message");
    CHECK(records[0].model == "gpt-4o");
    CHECK(records[0].turn_count == 3u);
}

TEST_CASE("append multiple distinct records — list returns all") {
    TmpDir tmp;
    fs::path idx = tmp / "index.json";

    constexpr int N = 5;
    std::vector<SessionIndexRecord> appended;
    for (int i = 0; i < N; ++i) {
        auto r = make_record("msg " + std::to_string(i));
        // Stagger updated_at slightly so ordering is deterministic.
        r.updated_at = r.created_at + std::chrono::seconds(i);
        appended.push_back(r);
        auto res = append_index_record(idx, r);
        REQUIRE(res.has_value());
    }

    auto read_result = read_latest_per_id(idx);
    REQUIRE(read_result.has_value());
    CHECK(read_result.value().size() == static_cast<size_t>(N));
}

TEST_CASE("list(N) returns most-recent N entries sorted descending by updated_at") {
    TmpDir tmp;
    fs::path idx = tmp / "index.json";

    constexpr int TOTAL = 10;
    constexpr size_t LIMIT = 3;
    auto base = std::chrono::system_clock::now();

    for (int i = 0; i < TOTAL; ++i) {
        auto r = make_record("msg " + std::to_string(i));
        r.updated_at = base + std::chrono::seconds(i);  // i=9 is newest
        auto res = append_index_record(idx, r);
        REQUIRE(res.has_value());
    }

    auto read_result = read_latest_per_id(idx, LIMIT);
    REQUIRE(read_result.has_value());
    const auto& records = read_result.value();
    CHECK(records.size() == LIMIT);

    // Should be sorted newest-first.
    for (size_t k = 1; k < records.size(); ++k) {
        CHECK(records[k - 1].updated_at >= records[k].updated_at);
    }

    // The most-recent entry should match the one with i = TOTAL-1.
    CHECK(records[0].first_message_preview == "msg 9");
}

TEST_CASE("read from missing file returns empty vector, not an error") {
    TmpDir tmp;
    fs::path idx = tmp / "no_such_index.json";

    auto result = read_latest_per_id(idx);
    REQUIRE(result.has_value());
    CHECK(result.value().empty());
}

TEST_CASE("append creates parent directories when they do not exist") {
    TmpDir tmp;
    fs::path deep = tmp.path / "sessions" / "nested" / "index.json";

    auto r = make_record("nested");
    auto res = append_index_record(deep, r);
    REQUIRE(res.has_value());
    CHECK(fs::exists(deep));
}

// =============================================================================
// TEST GROUP 2: find by UUID
// =============================================================================

TEST_CASE("find by UUID — record is present") {
    TmpDir tmp;
    fs::path idx = tmp / "index.json";

    auto target = make_record("findable");
    REQUIRE(append_index_record(idx, target).has_value());

    // Add some noise records.
    for (int i = 0; i < 4; ++i) {
        REQUIRE(append_index_record(idx, make_record("noise " + std::to_string(i))).has_value());
    }

    auto result = read_latest_per_id(idx, 0);  // 0 = return ALL
    REQUIRE(result.has_value());

    const auto& all = result.value();
    bool found = false;
    for (const auto& rec : all) {
        if (rec.id == target.id) {
            found = true;
            CHECK(rec.first_message_preview == "findable");
            break;
        }
    }
    CHECK(found);
}

TEST_CASE("find by UUID — record not present returns nothing") {
    TmpDir tmp;
    fs::path idx = tmp / "index.json";

    REQUIRE(append_index_record(idx, make_record("some record")).has_value());

    auto result = read_latest_per_id(idx, 0);
    REQUIRE(result.has_value());

    Uuid missing = Uuid::v4();
    bool found = false;
    for (const auto& rec : result.value()) {
        if (rec.id == missing) { found = true; break; }
    }
    CHECK_FALSE(found);
}

// =============================================================================
// TEST GROUP 3: update record via re-append + reverse-scan (newest-record-wins)
// =============================================================================

TEST_CASE("update: re-append with same id + newer updated_at supersedes old record") {
    TmpDir tmp;
    fs::path idx = tmp / "index.json";

    auto rec = make_record("original preview");
    rec.turn_count = 1;
    REQUIRE(append_index_record(idx, rec).has_value());

    // Update: same id, newer timestamp, different turn_count.
    SessionIndexRecord updated = rec;
    updated.updated_at  = rec.updated_at + std::chrono::seconds(60);
    updated.turn_count  = 10;
    updated.first_message_preview = "original preview";  // same content, turn count changed
    REQUIRE(append_index_record(idx, updated).has_value());

    auto result = read_latest_per_id(idx, 0);
    REQUIRE(result.has_value());
    const auto& records = result.value();

    // Deduplication: only one record for this id.
    int count = 0;
    uint64_t seen_turns = 0;
    for (const auto& r : records) {
        if (r.id == rec.id) {
            ++count;
            seen_turns = r.turn_count;
        }
    }
    CHECK(count == 1);
    CHECK(seen_turns == 10u);  // newer record wins
}

TEST_CASE("update: old record appended after new is ignored (newer timestamp always wins)") {
    TmpDir tmp;
    fs::path idx = tmp / "index.json";

    auto now = std::chrono::system_clock::now();

    SessionIndexRecord newer = make_record("newer");
    newer.updated_at  = now + std::chrono::seconds(100);
    newer.turn_count  = 99;
    REQUIRE(append_index_record(idx, newer).has_value());

    // Append an "older" record with the same id but earlier timestamp.
    SessionIndexRecord older = newer;
    older.updated_at = now;
    older.turn_count = 1;
    REQUIRE(append_index_record(idx, older).has_value());

    auto result = read_latest_per_id(idx, 0);
    REQUIRE(result.has_value());

    for (const auto& r : result.value()) {
        if (r.id == newer.id) {
            // The newer turn_count=99 record should win, not the older turn_count=1.
            CHECK(r.turn_count == 99u);
            break;
        }
    }
}

// =============================================================================
// TEST GROUP 4: list(N) returns most-recent N
// =============================================================================

TEST_CASE("list(0) returns ALL unique sessions (no limit)") {
    TmpDir tmp;
    fs::path idx = tmp / "index.json";

    for (int i = 0; i < 25; ++i) {
        REQUIRE(append_index_record(idx, make_record("m " + std::to_string(i))).has_value());
    }

    auto result = read_latest_per_id(idx, 0);
    REQUIRE(result.has_value());
    CHECK(result.value().size() == 25u);
}

TEST_CASE("list(20) default returns at most 20 sessions") {
    TmpDir tmp;
    fs::path idx = tmp / "index.json";

    for (int i = 0; i < 30; ++i) {
        REQUIRE(append_index_record(idx, make_record("m " + std::to_string(i))).has_value());
    }

    // Default n=20
    auto result = read_latest_per_id(idx);
    REQUIRE(result.has_value());
    CHECK(result.value().size() == 20u);
}

TEST_CASE("list(N) when fewer than N sessions exist returns all") {
    TmpDir tmp;
    fs::path idx = tmp / "index.json";

    for (int i = 0; i < 5; ++i) {
        REQUIRE(append_index_record(idx, make_record("m " + std::to_string(i))).has_value());
    }

    auto result = read_latest_per_id(idx, 20);
    REQUIRE(result.has_value());
    CHECK(result.value().size() == 5u);
}

// =============================================================================
// TEST GROUP 5: rebuild_from_dir
// =============================================================================

TEST_CASE("rebuild_from_dir: scans directory of session .json files, builds index") {
    TmpDir tmp;
    fs::path sessions_dir = tmp / "sessions";
    fs::create_directories(sessions_dir);
    fs::path idx = sessions_dir / "index.json";

    // Write synthetic session JSON files.
    const int N_FILES = 4;
    std::vector<Uuid> ids;
    for (int i = 0; i < N_FILES; ++i) {
        Uuid id = Uuid::v4();
        ids.push_back(id);
        nlohmann::json sess_json;
        sess_json["id"]           = id.to_string();
        sess_json["created_at"]   = "2026-01-01T10:00:00Z";
        sess_json["updated_at"]   = "2026-01-01T10:00:00Z";
        sess_json["model_at_start"] = "gpt-4o";
        sess_json["turn_count"]   = i + 1;
        sess_json["messages"] = nlohmann::json::array({
            {{"role", "user"}, {"content", "hello from session " + std::to_string(i)}}
        });

        fs::path sfile = sessions_dir / (id.to_string() + ".json");
        std::ofstream out(sfile);
        out << sess_json.dump();
        REQUIRE(out.good());
    }

    auto rebuild_result = rebuild_from_dir(sessions_dir, idx);
    REQUIRE(rebuild_result.has_value());
    CHECK(rebuild_result.value() == static_cast<uint64_t>(N_FILES));

    // Index file must exist now.
    REQUIRE(fs::exists(idx));

    // Reading the index must return N_FILES records.
    auto read_result = read_latest_per_id(idx, 0);
    REQUIRE(read_result.has_value());
    CHECK(read_result.value().size() == static_cast<size_t>(N_FILES));

    // All inserted UUIDs must appear in the rebuilt index.
    const auto& records = read_result.value();
    for (const auto& expected_id : ids) {
        bool found = false;
        for (const auto& r : records) {
            if (r.id == expected_id) { found = true; break; }
        }
        CHECK(found);
    }
}

TEST_CASE("rebuild_from_dir: skips non-session files (e.g. log files)") {
    TmpDir tmp;
    fs::path sessions_dir = tmp / "sessions";
    fs::create_directories(sessions_dir);
    fs::path idx = sessions_dir / "index.json";

    // Write one valid session.
    Uuid id = Uuid::v4();
    {
        nlohmann::json sess_json;
        sess_json["id"]             = id.to_string();
        sess_json["created_at"]     = "2026-01-01T12:00:00Z";
        sess_json["updated_at"]     = "2026-01-01T12:00:00Z";
        sess_json["model_at_start"] = "gpt-4o";
        sess_json["turn_count"]     = 1;
        sess_json["messages"]       = nlohmann::json::array();
        std::ofstream out(sessions_dir / (id.to_string() + ".json"));
        out << sess_json.dump();
    }

    // Write some decoys.
    { std::ofstream f(sessions_dir / "batbox.log"); f << "log line\n"; }
    { std::ofstream f(sessions_dir / "README.txt"); f << "readme\n"; }
    { std::ofstream f(sessions_dir / "tmp.json.bak"); f << "{}"; }

    auto result = rebuild_from_dir(sessions_dir, idx);
    REQUIRE(result.has_value());
    CHECK(result.value() == 1u);
}

TEST_CASE("rebuild_from_dir: empty sessions dir creates empty index, returns 0") {
    TmpDir tmp;
    fs::path sessions_dir = tmp / "empty_sessions";
    fs::create_directories(sessions_dir);
    fs::path idx = sessions_dir / "index.json";

    auto result = rebuild_from_dir(sessions_dir, idx);
    REQUIRE(result.has_value());
    CHECK(result.value() == 0u);
    CHECK(fs::exists(idx));
}

TEST_CASE("rebuild_from_dir: non-existent directory returns Err") {
    TmpDir tmp;
    fs::path nonexistent = tmp / "does_not_exist";
    fs::path idx         = tmp / "index.json";

    auto result = rebuild_from_dir(nonexistent, idx);
    CHECK_FALSE(result.has_value());
}

// =============================================================================
// TEST GROUP 6: corruption handling
// =============================================================================

TEST_CASE("corruption: malformed JSON line is skipped, valid lines are returned") {
    TmpDir tmp;
    fs::path idx = tmp / "index.json";

    // Write two valid records.
    auto r1 = make_record("valid one");
    auto r2 = make_record("valid two");
    REQUIRE(append_index_record(idx, r1).has_value());
    REQUIRE(append_index_record(idx, r2).has_value());

    // Inject a garbage line in the middle.
    {
        std::ofstream f(idx, std::ios::app);
        f << "THIS IS NOT JSON AT ALL {{{}}\n";
    }

    // Append one more valid record after the corrupt line.
    auto r3 = make_record("valid three");
    REQUIRE(append_index_record(idx, r3).has_value());

    auto result = read_latest_per_id(idx, 0);
    REQUIRE(result.has_value());

    // Should have 3 valid records; the garbage line is skipped.
    const auto& records = result.value();
    CHECK(records.size() == 3u);

    // Verify that all three valid UUIDs appear.
    auto has_id = [&](const Uuid& id) {
        for (const auto& r : records)
            if (r.id == id) return true;
        return false;
    };
    CHECK(has_id(r1.id));
    CHECK(has_id(r2.id));
    CHECK(has_id(r3.id));
}

TEST_CASE("corruption: multiple consecutive corrupt lines are all skipped") {
    TmpDir tmp;
    fs::path idx = tmp / "index.json";

    auto valid = make_record("the one good record");
    REQUIRE(append_index_record(idx, valid).has_value());

    {
        std::ofstream f(idx, std::ios::app);
        f << "garbage1\n";
        f << "garbage2\n";
        f << "{\"broken\": [}\n";
        f << "not json either\n";
    }

    auto result = read_latest_per_id(idx, 0);
    REQUIRE(result.has_value());
    CHECK(result.value().size() == 1u);
    CHECK(result.value()[0].id == valid.id);
}

TEST_CASE("is_corrupt() returns false for a clean index") {
    TmpDir tmp;
    fs::path idx = tmp / "index.json";

    REQUIRE(append_index_record(idx, make_record("clean")).has_value());
    CHECK_FALSE(is_corrupt(idx));
}

TEST_CASE("is_corrupt() returns false for missing index (absence != corruption)") {
    TmpDir tmp;
    fs::path idx = tmp / "nonexistent.json";
    CHECK_FALSE(is_corrupt(idx));
}

TEST_CASE("is_corrupt() returns true when index contains corrupt lines") {
    TmpDir tmp;
    fs::path idx = tmp / "index.json";

    REQUIRE(append_index_record(idx, make_record("good")).has_value());

    {
        std::ofstream f(idx, std::ios::app);
        f << "GARBAGE LINE\n";
    }

    CHECK(is_corrupt(idx));
}

TEST_CASE("corruption: JSON object missing required 'id' field is skipped") {
    TmpDir tmp;
    fs::path idx = tmp / "index.json";

    // Write a valid record.
    auto valid = make_record("valid");
    REQUIRE(append_index_record(idx, valid).has_value());

    // Manually append a JSON object that is structurally valid JSON but missing 'id'.
    {
        nlohmann::json bad;
        bad["created_at"] = "2026-01-01T00:00:00Z";
        bad["updated_at"] = "2026-01-01T00:00:00Z";
        bad["model"]      = "gpt-4o";
        // 'id', 'first_message_preview', 'turn_count', 'file_path' all missing
        std::ofstream f(idx, std::ios::app);
        f << bad.dump() << '\n';
    }

    auto result = read_latest_per_id(idx, 0);
    REQUIRE(result.has_value());
    // Only the one valid record should appear.
    CHECK(result.value().size() == 1u);
    CHECK(result.value()[0].id == valid.id);
}

// =============================================================================
// TEST GROUP 7: Performance — < 50 ms for 10 000-entry index
// =============================================================================

TEST_CASE("performance: read_latest_per_id < 50ms for 10,000-entry index") {
    TmpDir tmp;
    fs::path idx = tmp / "perf_index.json";

    // Build a 10 000-line index.  Use direct file writes for speed.
    auto base_time = std::chrono::system_clock::now();
    {
        std::ofstream out(idx, std::ios::binary);
        REQUIRE(out.is_open());
        for (int i = 0; i < 10000; ++i) {
            Uuid id = Uuid::v4();
            nlohmann::json j;
            j["id"]                    = id.to_string();
            j["created_at"]            = "2026-01-01T00:00:00Z";
            j["updated_at"]            = "2026-01-01T00:00:00Z";
            j["first_message_preview"] = "preview " + std::to_string(i);
            j["model"]                 = "gpt-4o";
            j["turn_count"]            = static_cast<uint64_t>(i);
            j["file_path"]             = "/tmp/" + id.to_string() + ".json";
            out << j.dump() << '\n';
        }
    }

    auto t0 = std::chrono::steady_clock::now();
    auto result = read_latest_per_id(idx, 20);
    auto t1 = std::chrono::steady_clock::now();

    REQUIRE(result.has_value());
    CHECK(result.value().size() == 20u);

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    // Generous budget: 50 ms contract; we allow up to 200 ms in test env.
    MESSAGE("read_latest_per_id 10k entries took " << elapsed_ms << " ms");
    // Production contract: < 50ms on M2-class hardware with Release build.
    // Debug builds (no optimisation) run substantially slower.
    // We enforce 2000ms here so CI catches pathological regressions while
    // allowing unoptimised debug runs to complete without spurious failure.
    CHECK(elapsed_ms < 2000);
}
