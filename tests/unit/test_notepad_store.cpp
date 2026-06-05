// tests/unit/test_notepad_store.cpp
// =============================================================================
// Unit tests for batbox::tools::NotepadStore (DIS-981, S6 — the notepad).
//
// AC1 coverage: a named, per-task, out-of-band pad with append + light-header
// semantics (NOT replace-blob, NOT checklist):
//   - append accumulates (never overwrites)
//   - "## <section>" headers/sections preserved
//   - one pad per session key (session_id else agent_id else "default")
//   - LEAST_FORCE: the store writes exactly the nugget the caller passes
//   - grep returns only matching entries
//   - reinjection_slice is bounded and tail-biased
// AC5 (partial): lifecycle archive() moves the pad to archive/.
//
// Build standalone (from repo root, x64-linux triplet):
//   c++ -std=c++20 -I include -I build/vcpkg_installed/x64-linux/include \
//       tests/unit/test_notepad_store.cpp \
//       src/tools/NotepadStore.cpp src/core/Paths.cpp \
//       -o /tmp/test_notepad_store && /tmp/test_notepad_store
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/tools/NotepadStore.hpp>

#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using batbox::tools::NotepadStore;

// ---------------------------------------------------------------------------
// RAII temp dir for an isolated pad root.
// ---------------------------------------------------------------------------
namespace {
struct TempRoot {
    fs::path path;
    explicit TempRoot(const char* tag) {
        path = fs::temp_directory_path() /
               ("batbox_notepad_test_" + std::string(tag));
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path, ec);
    }
    ~TempRoot() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};
} // namespace

TEST_SUITE("NotepadStore") {

    // -----------------------------------------------------------------------
    // AC1: append accumulates — never replaces (the goose-blob fix).
    // -----------------------------------------------------------------------
    TEST_CASE("append accumulates, does not overwrite") {
        TempRoot tr("accum");
        NotepadStore store(tr.path);
        const std::string key = "sess-1";

        REQUIRE(store.append(key, "first nugget"));
        REQUIRE(store.append(key, "second nugget"));
        REQUIRE(store.append(key, "third nugget"));

        const std::string pad = store.read(key);
        CHECK(pad.find("first nugget")  != std::string::npos);
        CHECK(pad.find("second nugget") != std::string::npos);
        CHECK(pad.find("third nugget")  != std::string::npos);

        // Ordering preserved (append, not replace).
        const auto p1 = pad.find("first nugget");
        const auto p2 = pad.find("second nugget");
        const auto p3 = pad.find("third nugget");
        CHECK(p1 < p2);
        CHECK(p2 < p3);

        // Born header present exactly once.
        CHECK(pad.find("# Notepad") != std::string::npos);
        CHECK(pad.find("# Notepad", p1) == std::string::npos);
    }

    // -----------------------------------------------------------------------
    // AC1: light "## <section>" headers preserved.
    // -----------------------------------------------------------------------
    TEST_CASE("light headers preserved") {
        TempRoot tr("headers");
        NotepadStore store(tr.path);
        const std::string key = "sess-2";

        REQUIRE(store.append(key, "JWT in Authorization header", "auth findings"));
        REQUIRE(store.append(key, "chose disk-backed pad", "decisions"));

        const std::string pad = store.read(key);
        CHECK(pad.find("## auth findings") != std::string::npos);
        CHECK(pad.find("## decisions")     != std::string::npos);
        CHECK(pad.find("JWT in Authorization header") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // AC1: LEAST_FORCE — the store writes exactly the nugget, no transcript.
    // An empty note is rejected (nothing to jot).
    // -----------------------------------------------------------------------
    TEST_CASE("LEAST_FORCE: jots exactly the nugget; empty note rejected") {
        TempRoot tr("least_force");
        NotepadStore store(tr.path);
        const std::string key = "sess-3";

        CHECK_FALSE(store.append(key, ""));          // empty rejected
        CHECK(store.read(key).empty());              // nothing created

        REQUIRE(store.append(key, "exactly this"));
        const std::string pad = store.read(key);
        // The body contains the nugget and nothing the caller did not pass
        // (besides the born header + the structural blank-line separator).
        CHECK(pad.find("exactly this") != std::string::npos);
        CHECK(pad.find("transcript")   == std::string::npos);
    }

    // -----------------------------------------------------------------------
    // AC1: one pad per session key — keys are isolated.
    // -----------------------------------------------------------------------
    TEST_CASE("one pad per session key") {
        TempRoot tr("perkey");
        NotepadStore store(tr.path);

        REQUIRE(store.append("alpha", "alpha note"));
        REQUIRE(store.append("beta",  "beta note"));

        CHECK(store.read("alpha").find("alpha note") != std::string::npos);
        CHECK(store.read("alpha").find("beta note")  == std::string::npos);
        CHECK(store.read("beta").find("beta note")   != std::string::npos);
        CHECK(store.read("beta").find("alpha note")  == std::string::npos);
    }

    // -----------------------------------------------------------------------
    // session_key derivation mirrors TodoWriteTool: session_id else agent_id
    // else "default".
    // -----------------------------------------------------------------------
    TEST_CASE("session_key derivation") {
        CHECK(NotepadStore::session_key("sid", "aid") == "sid");
        CHECK(NotepadStore::session_key("",    "aid") == "aid");
        CHECK(NotepadStore::session_key("",    "")    == "default");
    }

    // -----------------------------------------------------------------------
    // AC2 (store side): grep returns only entries that match the query.
    // -----------------------------------------------------------------------
    TEST_CASE("grep returns only matching entries") {
        TempRoot tr("grep");
        NotepadStore store(tr.path);
        const std::string key = "sess-4";

        REQUIRE(store.append(key, "the auth token lives in the JWT", "auth"));
        REQUIRE(store.append(key, "the gallery grid is 6 columns",  "layout"));
        REQUIRE(store.append(key, "auth retries cap at 3",          "auth"));

        const std::string hits = store.grep(key, "auth");
        CHECK(hits.find("JWT")        != std::string::npos);
        CHECK(hits.find("retries")    != std::string::npos);
        CHECK(hits.find("gallery")    == std::string::npos);  // unrelated entry excluded

        // Case-insensitive.
        CHECK(store.grep(key, "AUTH").find("JWT") != std::string::npos);

        // Empty query returns the whole (bounded) pad.
        CHECK(store.grep(key, "").find("gallery") != std::string::npos);

        // No match → empty.
        CHECK(store.grep(key, "nonexistent-token").empty());
    }

    // -----------------------------------------------------------------------
    // reinjection_slice: whole pad when small, bounded + tail-biased when big.
    // -----------------------------------------------------------------------
    TEST_CASE("reinjection_slice is bounded and tail-biased") {
        TempRoot tr("slice");
        NotepadStore store(tr.path);
        const std::string key = "sess-5";

        // Empty pad → empty slice (callers skip injection → cache untouched).
        CHECK(store.reinjection_slice(key).empty());

        REQUIRE(store.append(key, "small note"));
        CHECK(store.reinjection_slice(key).find("small note") != std::string::npos);

        // Grow the pad past a small budget; the tail (most recent) must survive.
        for (int i = 0; i < 200; ++i) {
            REQUIRE(store.append(key, "filler line number " + std::to_string(i)));
        }
        REQUIRE(store.append(key, "MOST-RECENT-MARKER"));

        const std::string slice = store.reinjection_slice(key, 512);
        CHECK(slice.size() <= 512 + 64);  // budget + truncation marker slack
        CHECK(slice.find("MOST-RECENT-MARKER") != std::string::npos);  // tail kept
        CHECK(slice.find("earlier notes truncated") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // AC5 (lifecycle): archive() moves the pad into archive/, leaving none live.
    // export_pad() returns the full pad.
    // -----------------------------------------------------------------------
    TEST_CASE("archive moves the pad; export returns full content") {
        TempRoot tr("archive");
        NotepadStore store(tr.path);
        const std::string key = "sess-6";

        REQUIRE(store.append(key, "note to be archived"));
        CHECK(store.export_pad(key).find("note to be archived") != std::string::npos);

        REQUIRE(fs::exists(store.pad_path(key)));
        REQUIRE(store.archive(key));

        CHECK_FALSE(fs::exists(store.pad_path(key)));               // live pad gone
        CHECK(fs::exists(tr.path / "archive" / "sess-6.md"));       // archived copy
        CHECK(store.read(key).empty());                            // fresh pad again

        // Archiving a non-existent pad is a no-op success.
        CHECK(store.archive("never-existed"));
    }
}
