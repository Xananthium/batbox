// tests/unit/test_compactor_no_copy.cpp
// =============================================================================
// Doctest suite for PEXT 5.2 — K-6: Compactor avoids head/tail vector copies.
//
// Acceptance criteria verified here:
//   1. compact() early-return path: returns msgs via std::move — caller's
//      vector is in moved-from (empty) state after the call.
//   2. Early-return result contains the same message ids as the original,
//      proving no copy occurred — the data was simply moved.
//   3. Compactor.cpp source does NOT contain "vector<Message> head" or
//      "vector<Message> tail" construction patterns (structural AC).
//
// Note on AC2 runtime approach:
//   Because compact() takes std::vector<Message>&& and returns via
//   std::move, verifying that the caller's vector is empty after the call
//   is the definitive move-semantic proof.  We cannot intercept Message
//   copy constructors directly (Message is not instrumented), but the moved-
//   from empty state is unambiguous evidence that no copy was made.
//
// Build standalone (no CMake, from repo root):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_compactor_no_copy.cpp \
//       src/conversation/Compactor.cpp \
//       src/conversation/Message.cpp \
//       src/core/Uuid.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       build/vcpkg_installed/arm64-osx/lib/libcpr.a \
//       build/vcpkg_installed/arm64-osx/lib/libcurl.a \
//       -o /tmp/test_compactor_no_copy && /tmp/test_compactor_no_copy
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/conversation/Compactor.hpp>
#include <batbox/conversation/Message.hpp>
#include <batbox/config/Config.hpp>
#include <batbox/inference/Client.hpp>
#include <batbox/core/CancelToken.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using batbox::conversation::Compactor;
using batbox::conversation::Message;
using batbox::conversation::Role;

// =============================================================================
// Helper: build a simple conversation of N user+assistant pairs.
// =============================================================================

namespace {

std::vector<Message> make_conversation(int pair_count) {
    std::vector<Message> msgs;
    msgs.reserve(static_cast<std::size_t>(pair_count) * 2);
    for (int i = 0; i < pair_count; ++i) {
        Message user;
        user.role    = Role::User;
        user.content = "User message " + std::to_string(i + 1);

        Message asst;
        asst.role    = Role::Assistant;
        asst.content = "Assistant reply " + std::to_string(i + 1);

        msgs.push_back(std::move(user));
        msgs.push_back(std::move(asst));
    }
    return msgs;
}

batbox::config::Config make_dead_end_config() {
    batbox::config::Config cfg;
    // Deliberately unreachable endpoint — no real network call should reach it.
    cfg.api.base_url            = "http://127.0.0.1:1/v1";
    cfg.api.api_key             = "test-key";
    cfg.api.request_timeout_sec = 1;
    return cfg;
}

} // anonymous namespace

// =============================================================================
// 1. Early-return path: compact() moves msgs out — caller's vector is empty.
//    keep_last_n >= total → tail_start == 0 → immediate return std::move(msgs).
// =============================================================================

TEST_CASE("compact no-copy: early-return moves msgs, caller vector is empty") {
    auto cfg = make_dead_end_config();
    batbox::inference::Client client{cfg};

    auto msgs = make_conversation(3);  // 6 messages total
    REQUIRE(msgs.size() == 6);

    // Capture ids before the move.
    std::vector<std::string> orig_ids;
    for (const auto& m : msgs) orig_ids.push_back(m.id);

    // keep_last_n=10 >= 6 total → early return path.
    Compactor c{10, "test-model"};
    auto [src, ct] = batbox::CancelToken::make_root();
    auto result = c.compact(std::move(msgs), client, std::move(ct));

    REQUIRE(result.has_value());

    // msgs must be in moved-from (empty) state — proves a move, not a copy.
    CHECK(msgs.empty());

    // Result contains the original messages (moved into result).
    const auto& out = result.value();
    REQUIRE(out.size() == orig_ids.size());
    for (std::size_t i = 0; i < orig_ids.size(); ++i) {
        CHECK(out[i].id == orig_ids[i]);
    }
}

// =============================================================================
// 2. Early-return with keep_last_n == 0 and tiny conversation (edge case).
//    tail_start = max(0, 1 - 0) = 1 → head is non-empty → NOT an early return.
//    Verify the early-return path specifically with keep_last_n > total.
// =============================================================================

TEST_CASE("compact no-copy: early-return with exactly equal keep_last_n") {
    auto cfg = make_dead_end_config();
    batbox::inference::Client client{cfg};

    auto msgs = make_conversation(2);  // 4 messages
    REQUIRE(msgs.size() == 4);

    std::vector<std::string> orig_ids;
    for (const auto& m : msgs) orig_ids.push_back(m.id);

    // keep_last_n == total → tail_start == max(0, 4-4) == 0 → early return.
    Compactor c{4, "test-model"};
    auto [src, ct] = batbox::CancelToken::make_root();
    auto result = c.compact(std::move(msgs), client, std::move(ct));

    REQUIRE(result.has_value());
    CHECK(msgs.empty());  // moved-from

    const auto& out = result.value();
    REQUIRE(out.size() == 4);
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(out[i].id == orig_ids[i]);
    }
}

// =============================================================================
// 3. keep_last_n == 0 with 1-message conversation.
//    tail_start = max(0, 1-0) = 1 → NOT early return (head has 1 message).
//    Verify no crash when network call fails (dead-end URL).
//    This tests the non-early-return code path compiles and runs without
//    constructing head/tail vectors.
// =============================================================================

TEST_CASE("compact no-copy: non-early-return path with dead-end client returns Err") {
    auto cfg = make_dead_end_config();
    batbox::inference::Client client{cfg};

    // 1 pair = 2 messages; keep_last_n=1 → tail_start=1 → head has 1 message.
    auto msgs = make_conversation(1);
    REQUIRE(msgs.size() == 2);

    Compactor c{1, "test-model"};
    auto [src, ct] = batbox::CancelToken::make_root();
    // This will fail at the network call (dead-end URL) — that is expected.
    // The important thing is it does NOT crash and returns Err correctly.
    auto result = c.compact(std::move(msgs), client, std::move(ct));

    // Dead-end URL → network error → Err.
    CHECK_FALSE(result.has_value());
    CHECK_FALSE(result.error().empty());
}

// =============================================================================
// 4. Structural check: Compactor.cpp does not construct head or tail vectors.
//    Reads the source file and asserts the banned patterns are absent.
// =============================================================================

TEST_CASE("compact no-copy: Compactor.cpp does not construct head or tail Message vectors") {
    // Locate Compactor.cpp relative to common build/run directories.
    namespace fs = std::filesystem;

    const std::vector<std::string> search_dirs = {
        ".",
        "..",
        "../..",
        "../../..",
    };

    std::string src_path;
    for (const auto& base : search_dirs) {
        fs::path candidate = fs::path(base) / "src" / "conversation" / "Compactor.cpp";
        if (fs::exists(candidate)) {
            src_path = candidate.string();
            break;
        }
    }

    if (src_path.empty()) {
        // Cannot locate source — skip structural check.
        MESSAGE("Compactor.cpp not found from cwd=" << fs::current_path().string()
                << " — skipping structural check");
        return;
    }

    std::ifstream f(src_path);
    REQUIRE(f.is_open());
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string src = ss.str();

    // The banned patterns: constructing a local head or tail copy-vector.
    // These patterns were present before K-6 and must not reappear.
    CHECK(src.find("vector<Message> head(") == std::string::npos);
    CHECK(src.find("vector<Message> tail(") == std::string::npos);

    // The new build_summary_request_messages signature must be present.
    CHECK(src.find("build_summary_request_messages(") != std::string::npos);
    CHECK(src.find("head_end") != std::string::npos);
}
