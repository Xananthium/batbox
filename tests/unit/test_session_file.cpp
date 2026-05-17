// tests/unit/test_session_file.cpp
// =============================================================================
// doctest suite for batbox::session::SessionFile
//
// Covers:
//   1. write_initial + append_message + read_session_file round-trip
//   2. gzip threshold trigger (>= GZIP_THRESHOLD_BYTES → .json.gz)
//   3. Crash-mid-append recovery (malformed tail truncated to last valid "]\n}")
//   4. Atomic temp+rename (temp file cleaned up, dest is final)
//
// Build + run (standalone, no CMake needed — from repo root):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_session_file.cpp src/session/SessionFile.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libz.a \
//       -o /tmp/test_session_file && /tmp/test_session_file
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/session/SessionFile.hpp>
#include <batbox/core/Uuid.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace batbox;
using namespace batbox::session;

// =============================================================================
// Test helpers
// =============================================================================

/// RAII temp-directory: created in system tmp; removed on destruction.
struct TmpDir {
    fs::path path;

    TmpDir() {
        auto base = fs::temp_directory_path() / ("batbox_sf_test_" + Uuid::v4().to_string());
        fs::create_directories(base);
        path = base;
    }

    ~TmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    fs::path operator/(const std::string& name) const { return path / name; }
};

/// Build a minimal SessionFile for testing.
static SessionFile make_session(const std::string& model = "gpt-4o") {
    SessionFile sf;
    sf.id            = Uuid::v4();
    sf.created_at    = std::chrono::system_clock::now();
    sf.updated_at    = sf.created_at;
    sf.model_at_start = model;
    sf.working_dir   = "/tmp/test_project";
    sf.messages      = {};
    sf.tool_calls_summary = Json::object();
    sf.usage_total   = {};
    sf.permission_rules_used = {};
    return sf;
}

/// Build a message JSON object.
static Json make_message(const std::string& role, const std::string& content) {
    return Json{{"role", role}, {"content", content}};
}

// =============================================================================
// Suite 1: create + append + load round-trip
// =============================================================================
TEST_SUITE("SessionFile — round-trip") {

    TEST_CASE("write_initial produces a readable file with empty messages array") {
        TmpDir tmp;
        auto path = tmp / "session.json";

        auto sf = make_session();
        auto res = write_initial(path, sf);
        REQUIRE(res.has_value());
        REQUIRE(fs::exists(path));

        auto loaded = read_session_file(path);
        REQUIRE(loaded.has_value());
        CHECK(loaded->id == sf.id);
        CHECK(loaded->messages.empty());
        CHECK(loaded->model_at_start == "gpt-4o");
    }

    TEST_CASE("append_message keeps file valid JSON after each append") {
        TmpDir tmp;
        auto path = tmp / "session.json";

        auto sf = make_session();
        REQUIRE(write_initial(path, sf));

        // Append first message.
        fs::path current_path = path;
        auto msg1 = make_message("user", "hello");
        auto r1 = append_message(path, msg1, &current_path);
        REQUIRE(r1.has_value());

        {
            auto loaded = read_session_file(current_path);
            REQUIRE(loaded.has_value());
            REQUIRE(loaded->messages.size() == 1);
            CHECK(loaded->messages[0]["role"] == "user");
            CHECK(loaded->messages[0]["content"] == "hello");
        }

        // Append second message.
        auto msg2 = make_message("assistant", "world");
        auto r2 = append_message(current_path, msg2, &current_path);
        REQUIRE(r2.has_value());

        {
            auto loaded = read_session_file(current_path);
            REQUIRE(loaded.has_value());
            REQUIRE(loaded->messages.size() == 2);
            CHECK(loaded->messages[1]["role"] == "assistant");
            CHECK(loaded->messages[1]["content"] == "world");
        }
    }

    TEST_CASE("round-trip preserves all SessionFile fields") {
        TmpDir tmp;
        auto path = tmp / "full.json";

        SessionFile sf = make_session("claude-3-5-sonnet");
        sf.working_dir = "/home/user/projects/batbox";
        sf.usage_total.prompt_tokens     = 1234;
        sf.usage_total.completion_tokens = 567;
        sf.permission_rules_used = { Json{{"tool","Bash"},{"pattern","*"}} };

        REQUIRE(write_initial(path, sf));

        // Append a message with tool_calls.
        Json msg = {
            {"role", "assistant"},
            {"content", nullptr},
            {"tool_calls", Json::array({
                Json{{"id","call_abc"},{"type","function"},
                     {"function",{{"name","Read"},{"arguments","{}"}}}}
            })}
        };
        REQUIRE(append_message(path, msg));

        auto loaded = read_session_file(path);
        REQUIRE(loaded.has_value());
        CHECK(loaded->id == sf.id);
        CHECK(loaded->model_at_start == "claude-3-5-sonnet");
        CHECK(loaded->working_dir == sf.working_dir);
        CHECK(loaded->usage_total.prompt_tokens == 1234);
        CHECK(loaded->usage_total.completion_tokens == 567);
        REQUIRE(loaded->permission_rules_used.size() == 1);
        CHECK(loaded->permission_rules_used[0]["tool"] == "Bash");
        REQUIRE(loaded->messages.size() == 1);
        CHECK(loaded->messages[0]["role"] == "assistant");
    }

    TEST_CASE("append_message works for ten messages in sequence") {
        TmpDir tmp;
        auto path = tmp / "ten.json";
        fs::path cur = path;

        REQUIRE(write_initial(path, make_session()));

        for (int i = 0; i < 10; ++i) {
            auto msg = make_message(i % 2 == 0 ? "user" : "assistant",
                                    "message " + std::to_string(i));
            REQUIRE(append_message(cur, msg, &cur));
        }

        auto loaded = read_session_file(cur);
        REQUIRE(loaded.has_value());
        CHECK(loaded->messages.size() == 10);
        for (int i = 0; i < 10; ++i) {
            std::string expected = "message " + std::to_string(i);
            CHECK(loaded->messages[i]["content"] == expected);
        }
    }
}

// =============================================================================
// Suite 2: gzip threshold trigger
// =============================================================================
TEST_SUITE("SessionFile — gzip threshold") {

    TEST_CASE("file above GZIP_THRESHOLD_BYTES is compressed to .json.gz") {
        TmpDir tmp;
        auto path = tmp / "big.json";
        fs::path cur = path;

        auto sf = make_session();
        REQUIRE(write_initial(path, sf));

        // Build a message large enough to push the file over 1 MB.
        // GZIP_THRESHOLD_BYTES = 1,000,000.  Write several 200 KB messages.
        std::string big_content(200'000, 'A');
        for (int i = 0; i < 6; ++i) {
            auto msg = make_message("user", big_content + std::to_string(i));
            REQUIRE(append_message(cur, msg, &cur));
        }

        // After crossing the threshold, cur should now point to .json.gz.
        const bool is_gz = (cur.extension().string() == ".gz");
        CHECK(is_gz);
        if (is_gz) {
            CHECK(fs::exists(cur));
            // The original .json should be gone.
            CHECK_FALSE(fs::exists(path));
        }
    }

    TEST_CASE("save_compressed produces readable .json.gz") {
        TmpDir tmp;
        auto path = tmp / "compact.json";

        auto sf = make_session();
        sf.messages = { make_message("user", "hello") };
        REQUIRE(write_initial(path, sf));

        auto gz_res = save_compressed(path, sf);
        REQUIRE(gz_res.has_value());
        auto gz_path = *gz_res;
        CHECK(gz_path.extension().string() == ".gz");
        CHECK(fs::exists(gz_path));
        CHECK_FALSE(fs::exists(path));  // original removed

        auto loaded = read_session_file(gz_path);
        REQUIRE(loaded.has_value());
        CHECK(loaded->id == sf.id);
    }

    TEST_CASE("read_session_file auto-detects .json vs .json.gz") {
        TmpDir tmp;
        auto json_path = tmp / "detect.json";
        auto sf = make_session("detect-model");
        sf.messages = { make_message("user", "detect") };
        REQUIRE(write_initial(json_path, sf));

        // Compress.
        auto gz_res = save_compressed(json_path, sf);
        REQUIRE(gz_res.has_value());

        auto loaded = read_session_file(*gz_res);
        REQUIRE(loaded.has_value());
        CHECK(loaded->model_at_start == "detect-model");
        REQUIRE(loaded->messages.size() == 1);
        CHECK(loaded->messages[0]["content"] == "detect");
    }
}

// =============================================================================
// Suite 3: crash mid-append recovery
// =============================================================================
TEST_SUITE("SessionFile — crash recovery") {

    TEST_CASE("crash mid-append does not corrupt prior messages") {
        TmpDir tmp;
        auto path = tmp / "crash.json";

        auto sf = make_session();
        REQUIRE(write_initial(path, sf));

        // Append two good messages.
        REQUIRE(append_message(path, make_message("user", "first")));
        REQUIRE(append_message(path, make_message("assistant", "second")));

        // Simulate a crash mid-append: append garbage AFTER the valid "]\n}".
        {
            std::ofstream f(path, std::ios::app | std::ios::binary);
            f << ",{\"role\":\"user\",\"content\":\"INCOMPLETE";
            // deliberately do NOT close the JSON — simulate a crash here
        }

        // The file now has a malformed tail.  read_session_file must recover.
        auto loaded = read_session_file(path);
        REQUIRE(loaded.has_value());
        // Both previously committed messages must be intact.
        REQUIRE(loaded->messages.size() == 2);
        CHECK(loaded->messages[0]["content"] == "first");
        CHECK(loaded->messages[1]["content"] == "second");
    }

    TEST_CASE("recovery truncates file on disk for subsequent reads") {
        TmpDir tmp;
        auto path = tmp / "crash2.json";
        auto sf = make_session();
        REQUIRE(write_initial(path, sf));
        REQUIRE(append_message(path, make_message("user", "msg1")));

        // Inject garbage tail.
        {
            std::ofstream f(path, std::ios::app | std::ios::binary);
            f << "CORRUPT_BYTES_HERE";
        }

        auto size_before = fs::file_size(path);

        // First read — triggers recovery + truncation.
        auto loaded1 = read_session_file(path);
        REQUIRE(loaded1.has_value());

        auto size_after = fs::file_size(path);
        CHECK(size_after < size_before);

        // Second read should also be clean.
        auto loaded2 = read_session_file(path);
        REQUIRE(loaded2.has_value());
        CHECK(loaded2->messages.size() == 1);
    }

    TEST_CASE("empty messages array survives a simulated truncation") {
        TmpDir tmp;
        auto path = tmp / "empty_crash.json";
        auto sf = make_session();
        REQUIRE(write_initial(path, sf));

        // Append garbage (no valid message was ever committed beyond initial).
        {
            std::ofstream f(path, std::ios::app | std::ios::binary);
            f << ",GARBAGE";
        }

        auto loaded = read_session_file(path);
        REQUIRE(loaded.has_value());
        CHECK(loaded->messages.empty());
    }
}

// =============================================================================
// Suite 4: atomic temp+rename
// =============================================================================
TEST_SUITE("SessionFile — atomic writes") {

    TEST_CASE("write_initial leaves no .tmp file on success") {
        TmpDir tmp;
        auto path = tmp / "atomic.json";

        REQUIRE(write_initial(path, make_session()));

        auto tmp_path = path;
        tmp_path += ".tmp";
        CHECK_FALSE(fs::exists(tmp_path));
        CHECK(fs::exists(path));
    }

    TEST_CASE("save_compressed leaves no .tmp file on success") {
        TmpDir tmp;
        auto path = tmp / "catomic.json";
        auto sf = make_session();
        sf.messages = { make_message("user", "atomic test") };
        REQUIRE(write_initial(path, sf));

        auto gz_res = save_compressed(path, sf);
        REQUIRE(gz_res.has_value());

        auto tmp_gz = *gz_res;
        tmp_gz += ".tmp";
        CHECK_FALSE(fs::exists(tmp_gz));
        CHECK(fs::exists(*gz_res));
    }

    TEST_CASE("write_initial is idempotent — second call overwrites first") {
        TmpDir tmp;
        auto path = tmp / "idem.json";

        auto sf1 = make_session("model-v1");
        auto sf2 = make_session("model-v2");

        REQUIRE(write_initial(path, sf1));
        REQUIRE(write_initial(path, sf2));

        auto loaded = read_session_file(path);
        REQUIRE(loaded.has_value());
        CHECK(loaded->model_at_start == "model-v2");
    }
}
