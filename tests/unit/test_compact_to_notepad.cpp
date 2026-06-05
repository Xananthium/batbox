// tests/unit/test_compact_to_notepad.cpp
// =============================================================================
// S5 (DIS-983) — Compactor::compact_to_notepad: the notepad-sink prune.
//
// Covers:
//   AC2 — the sink is the notepad, not an LLM summary: head tool-output bodies
//         are pruned to tombstones; the protected tail and authored text turns
//         are preserved verbatim; no network call is made.
//   AC3 — gold-preservation invariant: a large tool output is distilled to the
//         notepad, compaction prunes the raw, and the gold is still reachable
//         post-compaction via the re-injected notepad. Tombstone cites the pad.
//   AC4 — the gate still holds: after compaction estimate_tokens() is below the
//         compaction threshold (needs_compact(post)==false).
//   AC6 — the notepad survives + re-injects across the prune: a ChatRequest
//         built from the compacted stream still carries the pad's tail via
//         apply_notepad_reminder().
//
// compact_to_notepad() is network-free, so this test makes NO HTTP call. The
// cpr/curl chain is linked only because Compactor.cpp's legacy compact() shares
// the translation unit with Client — nothing here invokes it.
//
// Build standalone (from repo root, x64-linux triplet):
//   c++ -std=c++20 -I include -I build/vcpkg_installed/x64-linux/include \
//       tests/unit/test_compact_to_notepad.cpp \
//       src/conversation/Compactor.cpp src/conversation/ContextWindow.cpp \
//       src/conversation/Message.cpp src/conversation/NotepadReminder.cpp \
//       src/inference/ChatRequest.cpp \
//       src/tools/NotepadStore.cpp \
//       src/core/Uuid.cpp src/core/Paths.cpp \
//       build/vcpkg_installed/x64-linux/lib/libsimdjson.a \
//       -o /tmp/test_compact_to_notepad && /tmp/test_compact_to_notepad
//   (Uuid.cpp must be the DIS-969-fixed copy:
//    `git show fix/linux-build-breakage-dis969:src/core/Uuid.cpp`.)
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/conversation/Compactor.hpp>
#include <batbox/conversation/ContextWindow.hpp>
#include <batbox/conversation/Message.hpp>
#include <batbox/conversation/NotepadReminder.hpp>
#include <batbox/inference/ChatRequest.hpp>
#include <batbox/tools/NotepadStore.hpp>
#include <batbox/config/Config.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace batbox::conversation;
using batbox::tools::NotepadStore;

namespace {

Message user_msg(const std::string& body) {
    Message m; m.role = Role::User; m.content = body; return m;
}
Message asst_msg(const std::string& body) {
    Message m; m.role = Role::Assistant; m.content = body; return m;
}
Message tool_msg(const std::string& name, const std::string& body) {
    Message m;
    m.role         = Role::Tool;
    m.tool_name    = name;
    m.tool_call_id = "call_" + name;
    m.content      = body;
    return m;
}

struct TempRoot {
    fs::path path;
    TempRoot() {
        path = fs::temp_directory_path() / "batbox_compact_to_notepad_test";
        std::error_code ec; fs::remove_all(path, ec); fs::create_directories(path, ec);
    }
    ~TempRoot() { std::error_code ec; fs::remove_all(path, ec); }
};

} // namespace

TEST_SUITE("S5 compact_to_notepad") {

    // -----------------------------------------------------------------------
    // AC2 — structural prune shape.
    // -----------------------------------------------------------------------
    TEST_CASE("head tool bodies become tombstones; tail + authored text verbatim") {
        std::vector<Message> msgs;
        msgs.push_back(user_msg("first user request"));            // [0] head
        msgs.push_back(asst_msg("an assistant reply with prose")); // [1] head
        msgs.push_back(tool_msg("bash", std::string(4000, 'X')));  // [2] head tool
        msgs.push_back(tool_msg("read", std::string(4000, 'Y')));  // [3] head tool
        msgs.push_back(user_msg("recent user turn"));              // [4] tail
        msgs.push_back(asst_msg("recent assistant turn"));         // [5] tail

        Compactor compactor{2};  // keep last 2 verbatim
        auto res = compactor.compact_to_notepad(msgs, "/tmp/pad/session.md");
        REQUIRE(res.has_value());
        const auto& out = res.value();

        REQUIRE(out.size() == msgs.size());  // 1:1 — bodies rewritten, not dropped

        // Head tool bodies are tombstones (raw gone), role + correlation kept.
        CHECK(out[2].role == Role::Tool);
        CHECK(out[2].tool_call_id == msgs[2].tool_call_id);
        CHECK(out[2].content.find("[batbox-compacted]") != std::string::npos);
        CHECK(out[2].content.find(std::string(4000, 'X')) == std::string::npos);
        CHECK(out[2].content.find("4000 bytes") != std::string::npos);
        CHECK(out[3].content.find("[batbox-compacted]") != std::string::npos);
        CHECK(out[3].content.find(std::string(4000, 'Y')) == std::string::npos);

        // Authored head text is preserved verbatim.
        CHECK(out[0].content == "first user request");
        CHECK(out[1].content == "an assistant reply with prose");

        // Protected tail is verbatim.
        CHECK(out[4].content == "recent user turn");
        CHECK(out[5].content == "recent assistant turn");
    }

    TEST_CASE("no-op when the conversation is too short to split") {
        std::vector<Message> msgs;
        msgs.push_back(user_msg("u"));
        msgs.push_back(tool_msg("bash", std::string(9000, 'Z')));
        Compactor compactor{10};  // keep_last_n exceeds size → head empty
        auto res = compactor.compact_to_notepad(msgs, "/tmp/pad.md");
        REQUIRE(res.has_value());
        CHECK(res.value().size() == 2);
        CHECK(res.value()[1].content == std::string(9000, 'Z'));  // untouched
    }

    // -----------------------------------------------------------------------
    // AC3 — gold-preservation: raw pruned, gold reachable via the notepad.
    // -----------------------------------------------------------------------
    TEST_CASE("the gold survives in the notepad while the raw is pruned") {
        TempRoot tr;
        NotepadStore pad(tr.path);
        const std::string key = "compaction-session";

        // S1+S6: the distiller/agent wrote the GOLD line to the pad before the
        // raw entered the head.
        const std::string gold =
            "GOLD: build fails because vcpkg triplet is x64-linux not arm64-osx";
        REQUIRE(pad.append(key, gold, "findings"));
        const std::string pad_ref = pad.pad_path(key).string();

        // A big raw tool dump in the head — the thing that ate the window.
        const std::string raw(63000, 'R');
        std::vector<Message> msgs;
        msgs.push_back(user_msg("please build it"));
        msgs.push_back(tool_msg("bash", raw));     // head — to be pruned
        msgs.push_back(user_msg("recent"));        // tail
        msgs.push_back(asst_msg("recent reply"));  // tail

        Compactor compactor{2};
        auto res = compactor.compact_to_notepad(msgs, pad_ref);
        REQUIRE(res.has_value());
        const auto& out = res.value();

        // (a) the raw left the context entirely.
        for (const auto& m : out) {
            CHECK(m.content.find(raw) == std::string::npos);
        }
        // (c) a tombstone remains, and it cites the pad (b).
        CHECK(out[1].content.find("[batbox-compacted]") != std::string::npos);
        CHECK(out[1].content.find(pad_ref) != std::string::npos);

        // (b) the gold is still reachable in the notepad post-compaction.
        CHECK(pad.read(key).find(gold) != std::string::npos);
        CHECK(pad.reinjection_slice(key).find(gold) != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // AC4 — the gate still holds: post-compaction estimate is below threshold.
    // -----------------------------------------------------------------------
    TEST_CASE("needs_compact(post) == false after the prune") {
        // Three large tool dumps in the head dominate the token budget.
        std::vector<Message> msgs;
        msgs.push_back(user_msg("kick off"));
        msgs.push_back(tool_msg("bash", std::string(30000, 'A')));
        msgs.push_back(tool_msg("read", std::string(30000, 'B')));
        msgs.push_back(tool_msg("grep", std::string(30000, 'C')));
        msgs.push_back(user_msg("recent user"));
        msgs.push_back(asst_msg("recent assistant"));

        batbox::config::Config cfg;          // default: auto_compact_at_pct=80
        ContextWindow cw{cfg};
        cw.set_model_context_limit(8000);    // small window so the gate fires

        const std::size_t pre = cw.estimate_tokens(msgs);
        REQUIRE(cw.needs_compact(pre));      // gate fires pre-compaction

        Compactor compactor{2};
        auto res = compactor.compact_to_notepad(msgs, "/tmp/pad/session.md");
        REQUIRE(res.has_value());

        const std::size_t post = cw.estimate_tokens(res.value());
        CHECK(post < pre);
        CHECK_FALSE(cw.needs_compact(post)); // the decisive Compactor invariant
    }

    // -----------------------------------------------------------------------
    // AC6 — the notepad re-injects across the prune.
    // -----------------------------------------------------------------------
    TEST_CASE("a request built from the compacted stream still carries the pad tail") {
        TempRoot tr;
        NotepadStore pad(tr.path);
        const std::string key = "reinject-session";
        const std::string gold = "GOLD: keep the discnxt backlink in the footer";
        REQUIRE(pad.append(key, gold, "decisions"));

        std::vector<Message> msgs;
        msgs.push_back(user_msg("do the thing"));
        msgs.push_back(tool_msg("bash", std::string(20000, 'Q')));
        msgs.push_back(user_msg("recent"));
        msgs.push_back(asst_msg("recent reply"));

        Compactor compactor{2};
        auto res = compactor.compact_to_notepad(msgs, pad.pad_path(key).string());
        REQUIRE(res.has_value());

        // Rebuild the wire request from the COMPACTED messages, then re-inject
        // the pad tail (exactly what Conversation does after a prune).
        batbox::inference::ChatRequest req;
        for (const auto& m : res.value()) {
            batbox::inference::WireMessage w;
            w.role    = std::string(to_wire_role(m.role));
            w.content = m.content;
            req.messages.push_back(std::move(w));
        }
        const std::string slice = pad.reinjection_slice(key);
        const bool injected = apply_notepad_reminder(req, slice);

        CHECK(injected);
        REQUIRE_FALSE(req.messages.empty());
        // The pad reminder is the FINAL (tail) message and carries the gold.
        CHECK(req.messages.back().content.value_or("").find(gold)
              != std::string::npos);
    }
}
