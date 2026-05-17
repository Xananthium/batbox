// tests/unit/test_tui_events.cpp
//
// doctest suite for batbox::tui custom event subtypes (CPP 1.5).
//
// Build (standalone, no CMake):
//   c++ -std=c++20                                            \
//       -I/path/to/project/include                            \
//       -I/path/to/project/build/vcpkg_installed/arm64-osx/include \
//       -I/path/to/project/build/vcpkg_installed/arm64-osx/include/ftxui \
//       tests/unit/test_tui_events.cpp src/tui/Events.cpp    \
//       -L/path/to/project/build/vcpkg_installed/arm64-osx/lib \
//       -lftxui-component -lftxui-dom -lftxui-screen         \
//       -o /tmp/test_tui_events && /tmp/test_tui_events
//
// Or via CMake (add to tests/CMakeLists.txt):
//   batbox_add_unit_test(test_tui_events
//       unit/test_tui_events.cpp  batbox_tui)

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "batbox/tui/Events.hpp"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace batbox::tui;

// =============================================================================
// Helpers
// =============================================================================

/// True when the event's special string starts with the given prefix.
static bool event_has_prefix(const ftxui::Event& ev, const char* prefix) {
    const std::string& s = ev.input();
    std::size_t plen = std::strlen(prefix);
    return s.size() > plen &&
           s.compare(0, plen, prefix) == 0 &&
           s[plen] == ':';
}

// =============================================================================
// Token event
// =============================================================================
TEST_SUITE("TUI Events — Token") {

    TEST_CASE("make_token_event produces non-empty special string") {
        auto ev = make_token_event("hello", "agent-1");
        CHECK_FALSE(ev.input().empty());
    }

    TEST_CASE("token event input starts with canonical prefix") {
        auto ev = make_token_event("world");
        CHECK(event_has_prefix(ev, "batbox.token"));
    }

    TEST_CASE("extract_token recovers text and agent_id") {
        auto ev = make_token_event("fragment", "bot-42");
        auto payload = extract_token(ev);
        REQUIRE(payload.has_value());
        CHECK(payload->text      == "fragment");
        CHECK(payload->agent_id  == "bot-42");
    }

    TEST_CASE("extract_token works with empty agent_id (primary stream)") {
        auto ev = make_token_event("chunk");
        auto payload = extract_token(ev);
        REQUIRE(payload.has_value());
        CHECK(payload->text     == "chunk");
        CHECK(payload->agent_id == "");
    }

    TEST_CASE("extract_token returns nullopt for wrong event type") {
        auto ev = make_agents_dirty_event("a1");
        CHECK_FALSE(extract_token(ev).has_value());
    }

    TEST_CASE("extract_token returns nullopt on double-extraction (payload erased)") {
        auto ev = make_token_event("once");
        auto first  = extract_token(ev);
        auto second = extract_token(ev);
        REQUIRE(first.has_value());
        CHECK_FALSE(second.has_value());
    }

    TEST_CASE("Events::Token identity constant matches canonical name") {
        // The sentinel constant uses the bare name without a seq suffix;
        // it is used by OnEvent handlers for type-testing.
        CHECK(Events::Token.input() == std::string("batbox.token"));
    }
}

// =============================================================================
// AgentsDirty event
// =============================================================================
TEST_SUITE("TUI Events — AgentsDirty") {

    TEST_CASE("make_agents_dirty_event stores payload") {
        auto ev = make_agents_dirty_event("agent-99", 5, 300, "running");
        auto p  = extract_agents_dirty(ev);
        REQUIRE(p.has_value());
        CHECK(p->agent_id == "agent-99");
        CHECK(p->step     == 5u);
        CHECK(p->tokens   == 300u);
        CHECK(p->status   == "running");
    }

    TEST_CASE("agents-dirty event with all defaults") {
        auto ev = make_agents_dirty_event();
        auto p  = extract_agents_dirty(ev);
        REQUIRE(p.has_value());
        CHECK(p->agent_id == "");
        CHECK(p->step     == 0u);
        CHECK(p->tokens   == 0u);
        CHECK(p->status   == "");
    }

    TEST_CASE("Events::AgentsDirty identity constant") {
        CHECK(Events::AgentsDirty.input() == std::string("batbox.agents-dirty"));
    }

    TEST_CASE("extract_agents_dirty rejects wrong type") {
        auto ev = make_token_event("x");
        CHECK_FALSE(extract_agents_dirty(ev).has_value());
    }
}

// =============================================================================
// DemonDirty event
// =============================================================================
TEST_SUITE("TUI Events — DemonDirty") {

    TEST_CASE("make_demon_dirty_event stores demon_id") {
        auto ev = make_demon_dirty_event("demon-7");
        auto p  = extract_demon_dirty(ev);
        REQUIRE(p.has_value());
        CHECK(p->demon_id == "demon-7");
    }

    TEST_CASE("demon-dirty with empty demon_id means full refresh") {
        auto ev = make_demon_dirty_event();
        auto p  = extract_demon_dirty(ev);
        REQUIRE(p.has_value());
        CHECK(p->demon_id == "");
    }

    TEST_CASE("Events::DemonDirty identity constant") {
        CHECK(Events::DemonDirty.input() == std::string("batbox.demon-dirty"));
    }
}

// =============================================================================
// StatusUpdate event
// =============================================================================
TEST_SUITE("TUI Events — StatusUpdate") {

    TEST_CASE("make_status_update_event stores state and detail") {
        auto ev = make_status_update_event(SidecarState::Running, "pid=1234");
        auto p  = extract_status_update(ev);
        REQUIRE(p.has_value());
        CHECK(p->state  == SidecarState::Running);
        CHECK(p->detail == "pid=1234");
    }

    TEST_CASE("all SidecarState values round-trip") {
        for (auto st : {SidecarState::Cold, SidecarState::Starting,
                        SidecarState::Running, SidecarState::Crashed}) {
            auto ev = make_status_update_event(st);
            auto p  = extract_status_update(ev);
            REQUIRE(p.has_value());
            CHECK(p->state == st);
        }
    }

    TEST_CASE("Events::StatusUpdate identity constant") {
        CHECK(Events::StatusUpdate.input() == std::string("batbox.status-update"));
    }

    TEST_CASE("extract_status_update rejects wrong type") {
        auto ev = make_demon_dirty_event("d");
        CHECK_FALSE(extract_status_update(ev).has_value());
    }
}

// =============================================================================
// ModalShow event
// =============================================================================
TEST_SUITE("TUI Events — ModalShow") {

    TEST_CASE("make_modal_show_event stores all fields") {
        bool called = false;
        ModalResult received = ModalResult::Deny;

        auto ev = make_modal_show_event(
            "Allow tool?",
            "Tool **write_file** wants to create `/tmp/foo.txt`.",
            "write_file",
            [&](ModalResult r) { called = true; received = r; }
        );

        auto p = extract_modal_show(ev);
        REQUIRE(p.has_value());
        CHECK(p->title     == "Allow tool?");
        CHECK(p->body      == "Tool **write_file** wants to create `/tmp/foo.txt`.");
        CHECK(p->tool_name == "write_file");
        REQUIRE(static_cast<bool>(p->callback));

        // Invoke the callback and verify it wired through correctly.
        p->callback(ModalResult::AlwaysAllow);
        CHECK(called);
        CHECK(received == ModalResult::AlwaysAllow);
    }

    TEST_CASE("modal_show with empty tool_name (generic modal)") {
        auto ev = make_modal_show_event("Confirm?", "Are you sure?", "",
                                        [](ModalResult) {});
        auto p = extract_modal_show(ev);
        REQUIRE(p.has_value());
        CHECK(p->tool_name == "");
    }

    TEST_CASE("Events::ModalShow identity constant") {
        CHECK(Events::ModalShow.input() == std::string("batbox.modal-show"));
    }
}

// =============================================================================
// ModalHide event
// =============================================================================
TEST_SUITE("TUI Events — ModalHide") {

    TEST_CASE("make_modal_hide_event stores ModalResult::Allow") {
        auto ev = make_modal_hide_event(ModalResult::Allow);
        auto p  = extract_modal_hide(ev);
        REQUIRE(p.has_value());
        CHECK(p->result == ModalResult::Allow);
    }

    TEST_CASE("make_modal_hide_event stores ModalResult::Deny") {
        auto ev = make_modal_hide_event(ModalResult::Deny);
        auto p  = extract_modal_hide(ev);
        REQUIRE(p.has_value());
        CHECK(p->result == ModalResult::Deny);
    }

    TEST_CASE("make_modal_hide_event stores ModalResult::AlwaysAllow") {
        auto ev = make_modal_hide_event(ModalResult::AlwaysAllow);
        auto p  = extract_modal_hide(ev);
        REQUIRE(p.has_value());
        CHECK(p->result == ModalResult::AlwaysAllow);
    }

    TEST_CASE("Events::ModalHide identity constant") {
        CHECK(Events::ModalHide.input() == std::string("batbox.modal-hide"));
    }

    TEST_CASE("extract_modal_hide rejects wrong type") {
        auto ev = make_token_event("t");
        CHECK_FALSE(extract_modal_hide(ev).has_value());
    }
}

// =============================================================================
// Cross-type identity: each constant uniquely identifies its type
// =============================================================================
TEST_SUITE("TUI Events — Identity isolation") {

    TEST_CASE("All six sentinel Events are distinct from each other") {
        std::vector<ftxui::Event> sentinels = {
            Events::Token,
            Events::AgentsDirty,
            Events::DemonDirty,
            Events::StatusUpdate,
            Events::ModalShow,
            Events::ModalHide,
        };
        for (std::size_t i = 0; i < sentinels.size(); ++i) {
            for (std::size_t j = i + 1; j < sentinels.size(); ++j) {
                CHECK(sentinels[i] != sentinels[j]);
            }
        }
    }

    TEST_CASE("payload events do not compare equal to sentinel constants") {
        // A payload event embeds a seq suffix so it never equals the bare sentinel.
        auto tok  = make_token_event("x");
        auto ad   = make_agents_dirty_event();
        auto dd   = make_demon_dirty_event();
        auto su   = make_status_update_event(SidecarState::Cold);
        auto ms   = make_modal_show_event("t", "b", "", [](ModalResult){});
        auto mh   = make_modal_hide_event(ModalResult::Allow);

        CHECK(tok != Events::Token);
        CHECK(ad  != Events::AgentsDirty);
        CHECK(dd  != Events::DemonDirty);
        CHECK(su  != Events::StatusUpdate);
        CHECK(ms  != Events::ModalShow);
        CHECK(mh  != Events::ModalHide);

        // Consume payloads to keep registry clean.
        (void)extract_token(tok);
        (void)extract_agents_dirty(ad);
        (void)extract_demon_dirty(dd);
        (void)extract_status_update(su);
        (void)extract_modal_show(ms);
        (void)extract_modal_hide(mh);
    }
}

// =============================================================================
// Concurrency: many background threads post events simultaneously
// =============================================================================
TEST_SUITE("TUI Events — Concurrency") {

    TEST_CASE("concurrent factory calls produce unique keys with recoverable payloads") {
        constexpr int kN = 50;
        std::vector<ftxui::Event> events(kN);
        std::vector<std::thread>  threads;
        threads.reserve(kN);

        for (int i = 0; i < kN; ++i) {
            threads.emplace_back([&events, i] {
                events[static_cast<std::size_t>(i)] =
                    make_token_event(std::to_string(i),
                                     "agent-" + std::to_string(i));
            });
        }
        for (auto& t : threads) t.join();

        // All keys must be unique.
        for (int i = 0; i < kN; ++i) {
            for (int j = i + 1; j < kN; ++j) {
                CHECK(events[static_cast<std::size_t>(i)].input() !=
                      events[static_cast<std::size_t>(j)].input());
            }
        }

        // Every payload must be recoverable exactly once.
        std::atomic<int> recovered{0};
        for (int i = 0; i < kN; ++i) {
            auto p = extract_token(events[static_cast<std::size_t>(i)]);
            if (p.has_value()) ++recovered;
        }
        CHECK(recovered.load() == kN);
    }
}

// =============================================================================
// UserMessage event
// =============================================================================
TEST_SUITE("TUI Events — UserMessage") {

    TEST_CASE("make_user_message_event produces non-empty special string") {
        auto ev = make_user_message_event("hello world");
        CHECK_FALSE(ev.input().empty());
    }

    TEST_CASE("user-message event input starts with canonical prefix") {
        auto ev = make_user_message_event("test");
        CHECK(event_has_prefix(ev, "batbox.user-message"));
    }

    TEST_CASE("extract_user_message recovers text") {
        auto ev = make_user_message_event("What is the meaning of life?");
        auto payload = extract_user_message(ev);
        REQUIRE(payload.has_value());
        CHECK(payload->text == "What is the meaning of life?");
    }

    TEST_CASE("extract_user_message returns nullopt for wrong event type") {
        auto ev = make_token_event("not a user message");
        CHECK_FALSE(extract_user_message(ev).has_value());
        (void)extract_token(ev);  // clean up registry
    }

    TEST_CASE("extract_user_message returns nullopt on double-extraction (payload erased)") {
        auto ev = make_user_message_event("once only");
        auto first  = extract_user_message(ev);
        auto second = extract_user_message(ev);
        REQUIRE(first.has_value());
        CHECK(first->text == "once only");
        CHECK_FALSE(second.has_value());
    }

    TEST_CASE("Events::UserMessage identity constant matches canonical name") {
        CHECK(Events::UserMessage.input() == std::string("batbox.user-message"));
    }

    TEST_CASE("Events::UserMessage is distinct from all other sentinel constants") {
        CHECK(Events::UserMessage != Events::Token);
        CHECK(Events::UserMessage != Events::AgentsDirty);
        CHECK(Events::UserMessage != Events::DemonDirty);
        CHECK(Events::UserMessage != Events::StatusUpdate);
        CHECK(Events::UserMessage != Events::ModalShow);
        CHECK(Events::UserMessage != Events::ModalHide);
        CHECK(Events::UserMessage != Events::StreamDone);
    }

    TEST_CASE("payload UserMessage event does not compare equal to sentinel") {
        auto ev = make_user_message_event("payload");
        CHECK(ev != Events::UserMessage);
        (void)extract_user_message(ev);  // clean up
    }
}

// =============================================================================
// StreamDone event
// =============================================================================
TEST_SUITE("TUI Events — StreamDone") {

    TEST_CASE("make_stream_done_event produces non-empty special string") {
        auto ev = make_stream_done_event();
        CHECK_FALSE(ev.input().empty());
    }

    TEST_CASE("stream-done event input starts with canonical prefix") {
        auto ev = make_stream_done_event();
        CHECK(event_has_prefix(ev, "batbox.stream-done"));
        (void)extract_stream_done(ev);  // clean up
    }

    TEST_CASE("extract_stream_done recovers had_error=false (success path)") {
        auto ev = make_stream_done_event(false);
        auto payload = extract_stream_done(ev);
        REQUIRE(payload.has_value());
        CHECK(payload->had_error == false);
        CHECK(payload->role == "assistant");
    }

    TEST_CASE("extract_stream_done recovers had_error=true (error path)") {
        auto ev = make_stream_done_event(true);
        auto payload = extract_stream_done(ev);
        REQUIRE(payload.has_value());
        CHECK(payload->had_error == true);
        CHECK(payload->role == "assistant");
    }

    TEST_CASE("make_stream_done_event default argument is had_error=false") {
        auto ev = make_stream_done_event();  // default
        auto payload = extract_stream_done(ev);
        REQUIRE(payload.has_value());
        CHECK(payload->had_error == false);
    }

    TEST_CASE("extract_stream_done returns nullopt for wrong event type") {
        auto ev = make_token_event("not stream done");
        CHECK_FALSE(extract_stream_done(ev).has_value());
        (void)extract_token(ev);  // clean up
    }

    TEST_CASE("extract_stream_done returns nullopt on double-extraction (payload erased)") {
        auto ev = make_stream_done_event();
        auto first  = extract_stream_done(ev);
        auto second = extract_stream_done(ev);
        REQUIRE(first.has_value());
        CHECK_FALSE(second.has_value());
    }

    TEST_CASE("Events::StreamDone identity constant matches canonical name") {
        CHECK(Events::StreamDone.input() == std::string("batbox.stream-done"));
    }

    TEST_CASE("Events::StreamDone is distinct from all other sentinel constants") {
        CHECK(Events::StreamDone != Events::Token);
        CHECK(Events::StreamDone != Events::AgentsDirty);
        CHECK(Events::StreamDone != Events::DemonDirty);
        CHECK(Events::StreamDone != Events::StatusUpdate);
        CHECK(Events::StreamDone != Events::ModalShow);
        CHECK(Events::StreamDone != Events::ModalHide);
        CHECK(Events::StreamDone != Events::UserMessage);
    }

    TEST_CASE("payload StreamDone event does not compare equal to sentinel") {
        auto ev = make_stream_done_event();
        CHECK(ev != Events::StreamDone);
        (void)extract_stream_done(ev);  // clean up
    }
}

// =============================================================================
// ToolRunning event (TUI-T9 / UI-D10)
// =============================================================================
TEST_SUITE("TUI Events — ToolRunning") {

    TEST_CASE("[TUI-T9] make_tool_running_event produces non-empty special string") {
        auto ev = make_tool_running_event("Bash");
        CHECK_FALSE(ev.input().empty());
        (void)extract_tool_running(ev);  // clean up
    }

    TEST_CASE("[TUI-T9] tool-running event input starts with canonical prefix") {
        auto ev = make_tool_running_event("Read");
        CHECK(event_has_prefix(ev, "batbox.tool-running"));
        (void)extract_tool_running(ev);  // clean up
    }

    TEST_CASE("[TUI-T9] extract_tool_running recovers tool_name") {
        auto ev = make_tool_running_event("Bash");
        auto payload = extract_tool_running(ev);
        REQUIRE(payload.has_value());
        CHECK(payload->tool_name == "Bash");
    }

    TEST_CASE("[TUI-T9] extract_tool_running recovers Read tool name") {
        auto ev = make_tool_running_event("Read");
        auto payload = extract_tool_running(ev);
        REQUIRE(payload.has_value());
        CHECK(payload->tool_name == "Read");
    }

    TEST_CASE("[TUI-T9] extract_tool_running returns nullopt for wrong event type") {
        auto ev = make_token_event("not a tool running event");
        CHECK_FALSE(extract_tool_running(ev).has_value());
        (void)extract_token(ev);  // clean up
    }

    TEST_CASE("[TUI-T9] extract_tool_running returns nullopt on double-extraction") {
        auto ev = make_tool_running_event("Bash");
        auto first  = extract_tool_running(ev);
        auto second = extract_tool_running(ev);
        REQUIRE(first.has_value());
        CHECK(first->tool_name == "Bash");
        CHECK_FALSE(second.has_value());
    }

    TEST_CASE("[TUI-T9] Events::ToolRunning identity constant matches canonical name") {
        CHECK(Events::ToolRunning.input() == std::string("batbox.tool-running"));
    }

    TEST_CASE("[TUI-T9] Events::ToolRunning is distinct from all other sentinel constants") {
        CHECK(Events::ToolRunning != Events::Token);
        CHECK(Events::ToolRunning != Events::AgentsDirty);
        CHECK(Events::ToolRunning != Events::DemonDirty);
        CHECK(Events::ToolRunning != Events::StatusUpdate);
        CHECK(Events::ToolRunning != Events::ModalShow);
        CHECK(Events::ToolRunning != Events::ModalHide);
        CHECK(Events::ToolRunning != Events::UserMessage);
        CHECK(Events::ToolRunning != Events::StreamDone);
        CHECK(Events::ToolRunning != Events::MessageAppended);
        CHECK(Events::ToolRunning != Events::ToolDone);
    }

    TEST_CASE("[TUI-T9] payload ToolRunning event does not compare equal to sentinel") {
        auto ev = make_tool_running_event("Bash");
        CHECK(ev != Events::ToolRunning);
        (void)extract_tool_running(ev);  // clean up
    }

    TEST_CASE("[TUI-T9] make_tool_running_event with empty tool_name") {
        auto ev = make_tool_running_event("");
        auto payload = extract_tool_running(ev);
        REQUIRE(payload.has_value());
        CHECK(payload->tool_name == "");
    }
}

// =============================================================================
// ToolDone event (TUI-T9 / UI-D10)
// =============================================================================
TEST_SUITE("TUI Events — ToolDone") {

    TEST_CASE("[TUI-T9] make_tool_done_event produces non-empty special string") {
        auto ev = make_tool_done_event();
        CHECK_FALSE(ev.input().empty());
        (void)extract_tool_done(ev);  // clean up
    }

    TEST_CASE("[TUI-T9] tool-done event input starts with canonical prefix") {
        auto ev = make_tool_done_event();
        CHECK(event_has_prefix(ev, "batbox.tool-done"));
        (void)extract_tool_done(ev);  // clean up
    }

    TEST_CASE("[TUI-T9] extract_tool_done returns valid payload") {
        auto ev = make_tool_done_event();
        auto payload = extract_tool_done(ev);
        REQUIRE(payload.has_value());
    }

    TEST_CASE("[TUI-T9] extract_tool_done returns nullopt for wrong event type") {
        auto ev = make_token_event("not a tool done event");
        CHECK_FALSE(extract_tool_done(ev).has_value());
        (void)extract_token(ev);  // clean up
    }

    TEST_CASE("[TUI-T9] extract_tool_done returns nullopt on double-extraction") {
        auto ev = make_tool_done_event();
        auto first  = extract_tool_done(ev);
        auto second = extract_tool_done(ev);
        REQUIRE(first.has_value());
        CHECK_FALSE(second.has_value());
    }

    TEST_CASE("[TUI-T9] Events::ToolDone identity constant matches canonical name") {
        CHECK(Events::ToolDone.input() == std::string("batbox.tool-done"));
    }

    TEST_CASE("[TUI-T9] Events::ToolDone is distinct from all other sentinel constants") {
        CHECK(Events::ToolDone != Events::Token);
        CHECK(Events::ToolDone != Events::AgentsDirty);
        CHECK(Events::ToolDone != Events::DemonDirty);
        CHECK(Events::ToolDone != Events::StatusUpdate);
        CHECK(Events::ToolDone != Events::ModalShow);
        CHECK(Events::ToolDone != Events::ModalHide);
        CHECK(Events::ToolDone != Events::UserMessage);
        CHECK(Events::ToolDone != Events::StreamDone);
        CHECK(Events::ToolDone != Events::MessageAppended);
        CHECK(Events::ToolDone != Events::ToolRunning);
    }

    TEST_CASE("[TUI-T9] payload ToolDone event does not compare equal to sentinel") {
        auto ev = make_tool_done_event();
        CHECK(ev != Events::ToolDone);
        (void)extract_tool_done(ev);  // clean up
    }
}

// =============================================================================
// ToolRunning/ToolDone round-trip sequence (TUI-T9)
// =============================================================================
TEST_SUITE("TUI Events — ToolRunning/ToolDone sequence") {

    TEST_CASE("[TUI-T9] tool-running then tool-done sequence processes independently") {
        // Simulate the sequence: tool_running → dispatch → tool_done
        auto running_ev = make_tool_running_event("Bash");
        auto done_ev    = make_tool_done_event();

        auto running_payload = extract_tool_running(running_ev);
        auto done_payload    = extract_tool_done(done_ev);

        REQUIRE(running_payload.has_value());
        CHECK(running_payload->tool_name == "Bash");
        REQUIRE(done_payload.has_value());
    }

    TEST_CASE("[TUI-T9] multiple concurrent tool_running events have unique keys") {
        auto ev1 = make_tool_running_event("Bash");
        auto ev2 = make_tool_running_event("Read");
        auto ev3 = make_tool_running_event("Write");

        // All events must have distinct special strings.
        CHECK(ev1.input() != ev2.input());
        CHECK(ev2.input() != ev3.input());
        CHECK(ev1.input() != ev3.input());

        // Each payload is recoverable individually.
        auto p1 = extract_tool_running(ev1);
        auto p2 = extract_tool_running(ev2);
        auto p3 = extract_tool_running(ev3);

        REQUIRE(p1.has_value()); CHECK(p1->tool_name == "Bash");
        REQUIRE(p2.has_value()); CHECK(p2->tool_name == "Read");
        REQUIRE(p3.has_value()); CHECK(p3->tool_name == "Write");
    }

    TEST_CASE("[TUI-T9] extract_tool_running does not match tool_done events") {
        auto done_ev = make_tool_done_event();
        CHECK_FALSE(extract_tool_running(done_ev).has_value());
        (void)extract_tool_done(done_ev);  // clean up
    }

    TEST_CASE("[TUI-T9] extract_tool_done does not match tool_running events") {
        auto running_ev = make_tool_running_event("Bash");
        CHECK_FALSE(extract_tool_done(running_ev).has_value());
        (void)extract_tool_running(running_ev);  // clean up
    }
}

// =============================================================================
// QuestionShow event (TUI-ASKQ-T1)
// =============================================================================
TEST_SUITE("TUI Events — QuestionShow") {

    TEST_CASE("[TUI-ASKQ-T1] make_question_show_event produces non-empty special string") {
        bool cb_called = false;
        QuestionShowPayload p;
        p.header           = "Framework?";
        p.question         = "Which framework do you prefer?";
        p.multi_select     = false;
        p.labels           = {"React", "Vue", "Angular"};
        p.descriptions     = {"Meta library", "Progressive", "Google"};
        p.allow_freeform   = false;
        p.allow_escape_hatch = false;
        p.callback = [&](const QuestionResolvedPayload&) { cb_called = true; };

        auto ev = make_question_show_event(std::move(p));
        CHECK_FALSE(ev.input().empty());
        (void)extract_question_show(ev);  // clean up
    }

    TEST_CASE("[TUI-ASKQ-T1] question-show event input starts with canonical prefix") {
        QuestionShowPayload p;
        p.header   = "Choice";
        p.question = "Pick one?";
        p.labels   = {"A", "B"};
        p.callback = [](const QuestionResolvedPayload&) {};

        auto ev = make_question_show_event(std::move(p));
        CHECK(event_has_prefix(ev, "batbox.question-show"));
        (void)extract_question_show(ev);  // clean up
    }

    TEST_CASE("[TUI-ASKQ-T1] extract_question_show recovers all fields") {
        bool cb_invoked = false;

        QuestionShowPayload p;
        p.header             = "Chip";
        p.question           = "Bold heading?";
        p.multi_select       = true;
        p.labels             = {"Alpha", "Beta", "Gamma"};
        p.descriptions       = {"First", "Second", "Third"};
        p.allow_freeform     = true;
        p.allow_escape_hatch = true;
        p.callback = [&](const QuestionResolvedPayload& r) {
            cb_invoked = true;
            (void)r;
        };

        auto ev      = make_question_show_event(std::move(p));
        auto payload = extract_question_show(ev);

        REQUIRE(payload.has_value());
        CHECK(payload->header             == "Chip");
        CHECK(payload->question           == "Bold heading?");
        CHECK(payload->multi_select       == true);
        REQUIRE(payload->labels.size()    == 3u);
        CHECK(payload->labels[0]          == "Alpha");
        CHECK(payload->labels[1]          == "Beta");
        CHECK(payload->labels[2]          == "Gamma");
        REQUIRE(payload->descriptions.size() == 3u);
        CHECK(payload->descriptions[0]    == "First");
        CHECK(payload->descriptions[1]    == "Second");
        CHECK(payload->descriptions[2]    == "Third");
        CHECK(payload->allow_freeform     == true);
        CHECK(payload->allow_escape_hatch == true);
        REQUIRE(static_cast<bool>(payload->callback));

        // Invoke callback and verify it reaches the lambda.
        QuestionResolvedPayload resolved;
        resolved.chosen_labels = {"Alpha"};
        payload->callback(resolved);
        CHECK(cb_invoked);
    }

    TEST_CASE("[TUI-ASKQ-T1] extract_question_show: empty descriptions vector preserved") {
        QuestionShowPayload p;
        p.header   = "H";
        p.question = "Q?";
        p.labels   = {"X", "Y"};
        // descriptions intentionally left empty
        p.callback = [](const QuestionResolvedPayload&) {};

        auto ev = make_question_show_event(std::move(p));
        auto payload = extract_question_show(ev);

        REQUIRE(payload.has_value());
        CHECK(payload->descriptions.empty());
        CHECK(payload->labels.size() == 2u);
    }

    TEST_CASE("[TUI-ASKQ-T1] extract_question_show: multi_select defaults to false") {
        QuestionShowPayload p;
        p.header   = "H";
        p.question = "Q?";
        p.labels   = {"A"};
        p.callback = [](const QuestionResolvedPayload&) {};

        auto ev = make_question_show_event(std::move(p));
        auto payload = extract_question_show(ev);

        REQUIRE(payload.has_value());
        CHECK(payload->multi_select == false);
        CHECK(payload->allow_freeform == false);
        CHECK(payload->allow_escape_hatch == false);
    }

    TEST_CASE("[TUI-ASKQ-T1] extract_question_show returns nullopt for wrong event type") {
        auto wrong_ev = make_token_event("not question-show");
        CHECK_FALSE(extract_question_show(wrong_ev).has_value());
        (void)extract_token(wrong_ev);  // clean up
    }

    TEST_CASE("[TUI-ASKQ-T1] extract_question_show returns nullopt on double-extraction (payload erased)") {
        QuestionShowPayload p;
        p.header   = "H";
        p.question = "Q?";
        p.labels   = {"One"};
        p.callback = [](const QuestionResolvedPayload&) {};

        auto ev     = make_question_show_event(std::move(p));
        auto first  = extract_question_show(ev);
        auto second = extract_question_show(ev);
        REQUIRE(first.has_value());
        CHECK_FALSE(second.has_value());
    }

    TEST_CASE("[TUI-ASKQ-T1] Events::QuestionShow identity constant matches canonical name") {
        CHECK(Events::QuestionShow.input() == std::string("batbox.question-show"));
    }

    TEST_CASE("[TUI-ASKQ-T1] Events::QuestionShow is distinct from all other sentinel constants") {
        CHECK(Events::QuestionShow != Events::Token);
        CHECK(Events::QuestionShow != Events::AgentsDirty);
        CHECK(Events::QuestionShow != Events::DemonDirty);
        CHECK(Events::QuestionShow != Events::StatusUpdate);
        CHECK(Events::QuestionShow != Events::ModalShow);
        CHECK(Events::QuestionShow != Events::ModalHide);
        CHECK(Events::QuestionShow != Events::UserMessage);
        CHECK(Events::QuestionShow != Events::StreamDone);
        CHECK(Events::QuestionShow != Events::MessageAppended);
        CHECK(Events::QuestionShow != Events::ToolRunning);
        CHECK(Events::QuestionShow != Events::ToolDone);
        CHECK(Events::QuestionShow != Events::ThinkingStarted);
        CHECK(Events::QuestionShow != Events::ThinkingStopped);
        CHECK(Events::QuestionShow != Events::SpinnerTick);
        CHECK(Events::QuestionShow != Events::PlanApprovalShow);
        CHECK(Events::QuestionShow != Events::QuestionResolved);
    }

    TEST_CASE("[TUI-ASKQ-T1] payload QuestionShow event does not compare equal to sentinel") {
        QuestionShowPayload p;
        p.header   = "H";
        p.question = "Q?";
        p.labels   = {"One"};
        p.callback = [](const QuestionResolvedPayload&) {};

        auto ev = make_question_show_event(std::move(p));
        CHECK(ev != Events::QuestionShow);
        (void)extract_question_show(ev);  // clean up
    }
}

// =============================================================================
// QuestionResolved event (TUI-ASKQ-T1)
// =============================================================================
TEST_SUITE("TUI Events — QuestionResolved") {

    TEST_CASE("[TUI-ASKQ-T1] make_question_resolved_event produces non-empty special string") {
        QuestionResolvedPayload p;
        p.chosen_labels = {"Option A"};
        auto ev = make_question_resolved_event(std::move(p));
        CHECK_FALSE(ev.input().empty());
        (void)extract_question_resolved(ev);  // clean up
    }

    TEST_CASE("[TUI-ASKQ-T1] question-resolved event input starts with canonical prefix") {
        QuestionResolvedPayload p;
        p.chosen_labels = {"X"};
        auto ev = make_question_resolved_event(std::move(p));
        CHECK(event_has_prefix(ev, "batbox.question-resolved"));
        (void)extract_question_resolved(ev);  // clean up
    }

    TEST_CASE("[TUI-ASKQ-T1] extract_question_resolved recovers all fields — normal selection") {
        QuestionResolvedPayload p;
        p.chosen_labels  = {"React", "Vue"};
        p.freeform_text  = "";
        p.escape_hatch   = false;
        p.cancelled      = false;

        auto ev      = make_question_resolved_event(std::move(p));
        auto payload = extract_question_resolved(ev);

        REQUIRE(payload.has_value());
        REQUIRE(payload->chosen_labels.size() == 2u);
        CHECK(payload->chosen_labels[0] == "React");
        CHECK(payload->chosen_labels[1] == "Vue");
        CHECK(payload->freeform_text     == "");
        CHECK(payload->escape_hatch      == false);
        CHECK(payload->cancelled         == false);
    }

    TEST_CASE("[TUI-ASKQ-T1] extract_question_resolved recovers freeform_text path") {
        QuestionResolvedPayload p;
        p.chosen_labels = {};
        p.freeform_text = "Custom answer typed by user";
        p.escape_hatch  = false;
        p.cancelled     = false;

        auto ev      = make_question_resolved_event(std::move(p));
        auto payload = extract_question_resolved(ev);

        REQUIRE(payload.has_value());
        CHECK(payload->chosen_labels.empty());
        CHECK(payload->freeform_text == "Custom answer typed by user");
        CHECK(payload->escape_hatch  == false);
        CHECK(payload->cancelled     == false);
    }

    TEST_CASE("[TUI-ASKQ-T1] extract_question_resolved recovers escape_hatch path") {
        QuestionResolvedPayload p;
        p.chosen_labels = {};
        p.freeform_text = "";
        p.escape_hatch  = true;
        p.cancelled     = false;

        auto ev      = make_question_resolved_event(std::move(p));
        auto payload = extract_question_resolved(ev);

        REQUIRE(payload.has_value());
        CHECK(payload->chosen_labels.empty());
        CHECK(payload->freeform_text == "");
        CHECK(payload->escape_hatch  == true);
        CHECK(payload->cancelled     == false);
    }

    TEST_CASE("[TUI-ASKQ-T1] extract_question_resolved recovers cancelled=true (Esc)") {
        QuestionResolvedPayload p;
        p.chosen_labels = {};
        p.freeform_text = "";
        p.escape_hatch  = false;
        p.cancelled     = true;

        auto ev      = make_question_resolved_event(std::move(p));
        auto payload = extract_question_resolved(ev);

        REQUIRE(payload.has_value());
        CHECK(payload->chosen_labels.empty());
        CHECK(payload->cancelled == true);
        CHECK(payload->escape_hatch == false);
    }

    TEST_CASE("[TUI-ASKQ-T1] extract_question_resolved: default bools are false") {
        QuestionResolvedPayload p;
        p.chosen_labels = {"A"};
        // escape_hatch and cancelled default to false

        auto ev = make_question_resolved_event(std::move(p));
        auto payload = extract_question_resolved(ev);

        REQUIRE(payload.has_value());
        CHECK(payload->escape_hatch == false);
        CHECK(payload->cancelled    == false);
    }

    TEST_CASE("[TUI-ASKQ-T1] extract_question_resolved returns nullopt for wrong event type") {
        auto wrong_ev = make_token_event("not question-resolved");
        CHECK_FALSE(extract_question_resolved(wrong_ev).has_value());
        (void)extract_token(wrong_ev);  // clean up
    }

    TEST_CASE("[TUI-ASKQ-T1] extract_question_resolved returns nullopt on double-extraction") {
        QuestionResolvedPayload p;
        p.chosen_labels = {"One"};

        auto ev     = make_question_resolved_event(std::move(p));
        auto first  = extract_question_resolved(ev);
        auto second = extract_question_resolved(ev);
        REQUIRE(first.has_value());
        CHECK_FALSE(second.has_value());
    }

    TEST_CASE("[TUI-ASKQ-T1] Events::QuestionResolved identity constant matches canonical name") {
        CHECK(Events::QuestionResolved.input() == std::string("batbox.question-resolved"));
    }

    TEST_CASE("[TUI-ASKQ-T1] Events::QuestionResolved is distinct from all other sentinel constants") {
        CHECK(Events::QuestionResolved != Events::Token);
        CHECK(Events::QuestionResolved != Events::AgentsDirty);
        CHECK(Events::QuestionResolved != Events::DemonDirty);
        CHECK(Events::QuestionResolved != Events::StatusUpdate);
        CHECK(Events::QuestionResolved != Events::ModalShow);
        CHECK(Events::QuestionResolved != Events::ModalHide);
        CHECK(Events::QuestionResolved != Events::UserMessage);
        CHECK(Events::QuestionResolved != Events::StreamDone);
        CHECK(Events::QuestionResolved != Events::MessageAppended);
        CHECK(Events::QuestionResolved != Events::ToolRunning);
        CHECK(Events::QuestionResolved != Events::ToolDone);
        CHECK(Events::QuestionResolved != Events::ThinkingStarted);
        CHECK(Events::QuestionResolved != Events::ThinkingStopped);
        CHECK(Events::QuestionResolved != Events::SpinnerTick);
        CHECK(Events::QuestionResolved != Events::PlanApprovalShow);
        CHECK(Events::QuestionResolved != Events::QuestionShow);
    }

    TEST_CASE("[TUI-ASKQ-T1] payload QuestionResolved event does not compare equal to sentinel") {
        QuestionResolvedPayload p;
        p.chosen_labels = {"A"};
        auto ev = make_question_resolved_event(std::move(p));
        CHECK(ev != Events::QuestionResolved);
        (void)extract_question_resolved(ev);  // clean up
    }
}

// =============================================================================
// QuestionShow / QuestionResolved round-trip + callback invocation (TUI-ASKQ-T1)
// =============================================================================
TEST_SUITE("TUI Events — QuestionShow/QuestionResolved round-trip") {

    TEST_CASE("[TUI-ASKQ-T1] full round-trip: show payload → extract → callback → resolved") {
        std::string received_label;
        bool cb_called = false;

        // Build a question-show payload with callback
        QuestionShowPayload show_p;
        show_p.header   = "Lang?";
        show_p.question = "Which language do you prefer?";
        show_p.labels   = {"C++", "Rust", "Go"};
        show_p.descriptions = {"Fast", "Safe", "Simple"};
        show_p.multi_select = false;
        show_p.allow_freeform = false;
        show_p.allow_escape_hatch = false;
        show_p.callback = [&](const QuestionResolvedPayload& r) {
            cb_called = true;
            if (!r.chosen_labels.empty()) {
                received_label = r.chosen_labels[0];
            }
        };

        // Background thread "posts" the event
        auto show_ev = make_question_show_event(std::move(show_p));

        // UI thread "handles" the event: extracts show payload, renders card,
        // then fires resolved event
        auto extracted_show = extract_question_show(show_ev);
        REQUIRE(extracted_show.has_value());
        CHECK(extracted_show->header   == "Lang?");
        CHECK(extracted_show->labels.size() == 3u);
        REQUIRE(static_cast<bool>(extracted_show->callback));

        // UI thread builds resolved payload (user picked "Rust")
        QuestionResolvedPayload resolved_p;
        resolved_p.chosen_labels = {"Rust"};
        resolved_p.cancelled     = false;

        // UI thread invokes callback
        extracted_show->callback(resolved_p);

        CHECK(cb_called);
        CHECK(received_label == "Rust");

        // Second extraction must return nullopt (consumed)
        CHECK_FALSE(extract_question_show(show_ev).has_value());
    }

    TEST_CASE("[TUI-ASKQ-T1] extract_question_show does not match question_resolved events") {
        QuestionResolvedPayload p;
        p.chosen_labels = {"A"};
        auto resolved_ev = make_question_resolved_event(std::move(p));
        CHECK_FALSE(extract_question_show(resolved_ev).has_value());
        (void)extract_question_resolved(resolved_ev);  // clean up
    }

    TEST_CASE("[TUI-ASKQ-T1] extract_question_resolved does not match question_show events") {
        QuestionShowPayload p;
        p.header   = "H";
        p.question = "Q?";
        p.labels   = {"X"};
        p.callback = [](const QuestionResolvedPayload&) {};

        auto show_ev = make_question_show_event(std::move(p));
        CHECK_FALSE(extract_question_resolved(show_ev).has_value());
        (void)extract_question_show(show_ev);  // clean up
    }

    TEST_CASE("[TUI-ASKQ-T1] multi_select: multiple chosen_labels round-trip") {
        QuestionShowPayload show_p;
        show_p.header      = "Tags";
        show_p.question    = "Pick all that apply:";
        show_p.labels      = {"Tests", "Docs", "Perf", "Security"};
        show_p.multi_select = true;
        show_p.callback    = [](const QuestionResolvedPayload&) {};

        auto show_ev      = make_question_show_event(std::move(show_p));
        auto extracted    = extract_question_show(show_ev);
        REQUIRE(extracted.has_value());
        CHECK(extracted->multi_select == true);

        // Simulate user selecting 3 options
        QuestionResolvedPayload resolved_p;
        resolved_p.chosen_labels = {"Tests", "Perf", "Security"};

        auto resolved_ev  = make_question_resolved_event(std::move(resolved_p));
        auto resolved_out = extract_question_resolved(resolved_ev);

        REQUIRE(resolved_out.has_value());
        REQUIRE(resolved_out->chosen_labels.size() == 3u);
        CHECK(resolved_out->chosen_labels[0] == "Tests");
        CHECK(resolved_out->chosen_labels[1] == "Perf");
        CHECK(resolved_out->chosen_labels[2] == "Security");
        CHECK(resolved_out->cancelled == false);
    }
}
