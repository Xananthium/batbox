// tests/unit/test_tui_thinking_indicator.cpp
//
// doctest suite for the TUI-T15 "thinking..." status row indicator.
//
// Tests cover:
//   1. ThinkingStarted / ThinkingStopped event round-trips.
//   2. Event sentinel constant identity.
//   3. Payload isolation: extractors reject wrong event types.
//   4. Double-extraction returns nullopt (payload erased after first read).
//   5. Distinct identity from all existing sentinel constants.
//
// Build (standalone, no CMake):
//   c++ -std=c++20                                                \
//       -I<project>/include                                       \
//       -I<build>/vcpkg_installed/arm64-osx/include              \
//       tests/unit/test_tui_thinking_indicator.cpp               \
//       src/tui/Events.cpp                                        \
//       -L<build>/vcpkg_installed/arm64-osx/lib                  \
//       -lftxui-component -lftxui-dom -lftxui-screen             \
//       -o /tmp/test_tui_thinking_indicator &&                    \
//       /tmp/test_tui_thinking_indicator
//
// Or via CMake (add to tests/CMakeLists.txt):
//   batbox_add_unit_test(test_tui_thinking_indicator
//       unit/test_tui_thinking_indicator.cpp  batbox_core)
//   target_sources(test_tui_thinking_indicator PRIVATE
//       ${PROJECT_SOURCE_DIR}/src/tui/Events.cpp)
//   target_link_libraries(test_tui_thinking_indicator PRIVATE
//       ftxui::component  ftxui::dom  ftxui::screen)

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "batbox/tui/Events.hpp"

#include <string>

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
// ThinkingStarted event (TUI-T15)
// =============================================================================
TEST_SUITE("TUI Events — ThinkingStarted (TUI-T15)") {

    TEST_CASE("[TUI-T15] make_thinking_started_event produces non-empty special string") {
        auto ev = make_thinking_started_event();
        CHECK_FALSE(ev.input().empty());
        (void)extract_thinking_started(ev);  // clean up
    }

    TEST_CASE("[TUI-T15] thinking-started event input starts with canonical prefix") {
        auto ev = make_thinking_started_event();
        CHECK(event_has_prefix(ev, "batbox.thinking-started"));
        (void)extract_thinking_started(ev);  // clean up
    }

    TEST_CASE("[TUI-T15] extract_thinking_started returns valid payload") {
        auto ev = make_thinking_started_event();
        auto payload = extract_thinking_started(ev);
        REQUIRE(payload.has_value());
    }

    TEST_CASE("[TUI-T15] extract_thinking_started returns nullopt for wrong event type") {
        auto ev = make_token_event("not a thinking event");
        CHECK_FALSE(extract_thinking_started(ev).has_value());
        (void)extract_token(ev);  // clean up
    }

    TEST_CASE("[TUI-T15] extract_thinking_started returns nullopt on double-extraction (payload erased)") {
        auto ev = make_thinking_started_event();
        auto first  = extract_thinking_started(ev);
        auto second = extract_thinking_started(ev);
        REQUIRE(first.has_value());
        CHECK_FALSE(second.has_value());
    }

    TEST_CASE("[TUI-T15] extract_thinking_started does not match thinking-stopped events") {
        auto ev = make_thinking_stopped_event();
        CHECK_FALSE(extract_thinking_started(ev).has_value());
        (void)extract_thinking_stopped(ev);  // clean up
    }

    TEST_CASE("[TUI-T15] Events::ThinkingStarted identity constant matches canonical name") {
        CHECK(Events::ThinkingStarted.input() == std::string("batbox.thinking-started"));
    }

    TEST_CASE("[TUI-T15] payload ThinkingStarted event does not compare equal to sentinel") {
        auto ev = make_thinking_started_event();
        CHECK(ev != Events::ThinkingStarted);
        (void)extract_thinking_started(ev);  // clean up
    }

    TEST_CASE("[TUI-T15] Events::ThinkingStarted is distinct from all existing sentinel constants") {
        CHECK(Events::ThinkingStarted != Events::Token);
        CHECK(Events::ThinkingStarted != Events::AgentsDirty);
        CHECK(Events::ThinkingStarted != Events::DemonDirty);
        CHECK(Events::ThinkingStarted != Events::StatusUpdate);
        CHECK(Events::ThinkingStarted != Events::ModalShow);
        CHECK(Events::ThinkingStarted != Events::ModalHide);
        CHECK(Events::ThinkingStarted != Events::UserMessage);
        CHECK(Events::ThinkingStarted != Events::StreamDone);
        CHECK(Events::ThinkingStarted != Events::MessageAppended);
        CHECK(Events::ThinkingStarted != Events::ToolRunning);
        CHECK(Events::ThinkingStarted != Events::ToolDone);
        CHECK(Events::ThinkingStarted != Events::ThinkingStopped);
    }
}

// =============================================================================
// ThinkingStopped event (TUI-T15)
// =============================================================================
TEST_SUITE("TUI Events — ThinkingStopped (TUI-T15)") {

    TEST_CASE("[TUI-T15] make_thinking_stopped_event produces non-empty special string") {
        auto ev = make_thinking_stopped_event();
        CHECK_FALSE(ev.input().empty());
        (void)extract_thinking_stopped(ev);  // clean up
    }

    TEST_CASE("[TUI-T15] thinking-stopped event input starts with canonical prefix") {
        auto ev = make_thinking_stopped_event();
        CHECK(event_has_prefix(ev, "batbox.thinking-stopped"));
        (void)extract_thinking_stopped(ev);  // clean up
    }

    TEST_CASE("[TUI-T15] extract_thinking_stopped returns valid payload") {
        auto ev = make_thinking_stopped_event();
        auto payload = extract_thinking_stopped(ev);
        REQUIRE(payload.has_value());
    }

    TEST_CASE("[TUI-T15] extract_thinking_stopped returns nullopt for wrong event type") {
        auto ev = make_token_event("not a thinking-stopped event");
        CHECK_FALSE(extract_thinking_stopped(ev).has_value());
        (void)extract_token(ev);  // clean up
    }

    TEST_CASE("[TUI-T15] extract_thinking_stopped returns nullopt on double-extraction (payload erased)") {
        auto ev = make_thinking_stopped_event();
        auto first  = extract_thinking_stopped(ev);
        auto second = extract_thinking_stopped(ev);
        REQUIRE(first.has_value());
        CHECK_FALSE(second.has_value());
    }

    TEST_CASE("[TUI-T15] extract_thinking_stopped does not match thinking-started events") {
        auto ev = make_thinking_started_event();
        CHECK_FALSE(extract_thinking_stopped(ev).has_value());
        (void)extract_thinking_started(ev);  // clean up
    }

    TEST_CASE("[TUI-T15] Events::ThinkingStopped identity constant matches canonical name") {
        CHECK(Events::ThinkingStopped.input() == std::string("batbox.thinking-stopped"));
    }

    TEST_CASE("[TUI-T15] payload ThinkingStopped event does not compare equal to sentinel") {
        auto ev = make_thinking_stopped_event();
        CHECK(ev != Events::ThinkingStopped);
        (void)extract_thinking_stopped(ev);  // clean up
    }

    TEST_CASE("[TUI-T15] Events::ThinkingStopped is distinct from all existing sentinel constants") {
        CHECK(Events::ThinkingStopped != Events::Token);
        CHECK(Events::ThinkingStopped != Events::AgentsDirty);
        CHECK(Events::ThinkingStopped != Events::DemonDirty);
        CHECK(Events::ThinkingStopped != Events::StatusUpdate);
        CHECK(Events::ThinkingStopped != Events::ModalShow);
        CHECK(Events::ThinkingStopped != Events::ModalHide);
        CHECK(Events::ThinkingStopped != Events::UserMessage);
        CHECK(Events::ThinkingStopped != Events::StreamDone);
        CHECK(Events::ThinkingStopped != Events::MessageAppended);
        CHECK(Events::ThinkingStopped != Events::ToolRunning);
        CHECK(Events::ThinkingStopped != Events::ToolDone);
        CHECK(Events::ThinkingStopped != Events::ThinkingStarted);
    }
}

// =============================================================================
// ThinkingStarted/ThinkingStopped round-trip sequence (TUI-T15)
// =============================================================================
TEST_SUITE("TUI Events — ThinkingStarted/ThinkingStopped sequence (TUI-T15)") {

    TEST_CASE("[TUI-T15] thinking-started then thinking-stopped processes independently") {
        auto started_ev = make_thinking_started_event();
        auto stopped_ev = make_thinking_stopped_event();

        auto started_payload = extract_thinking_started(started_ev);
        auto stopped_payload = extract_thinking_stopped(stopped_ev);

        REQUIRE(started_payload.has_value());
        REQUIRE(stopped_payload.has_value());
    }

    TEST_CASE("[TUI-T15] multiple concurrent thinking-started events have unique keys") {
        auto ev1 = make_thinking_started_event();
        auto ev2 = make_thinking_started_event();
        auto ev3 = make_thinking_started_event();

        // All events must have distinct special strings.
        CHECK(ev1.input() != ev2.input());
        CHECK(ev2.input() != ev3.input());
        CHECK(ev1.input() != ev3.input());

        // Each payload is recoverable individually.
        auto p1 = extract_thinking_started(ev1);
        auto p2 = extract_thinking_started(ev2);
        auto p3 = extract_thinking_started(ev3);

        REQUIRE(p1.has_value());
        REQUIRE(p2.has_value());
        REQUIRE(p3.has_value());
    }

    TEST_CASE("[TUI-T15] thinking-started and tool-running/tool-done are independent") {
        // These can coexist in the event queue (priority is handled in InputBar render).
        auto thinking_ev = make_thinking_started_event();
        auto tool_ev     = make_tool_running_event("Bash");
        auto done_ev     = make_tool_done_event();

        // Neither extractor cross-matches.
        CHECK_FALSE(extract_thinking_started(tool_ev).has_value());
        CHECK_FALSE(extract_tool_running(thinking_ev).has_value());

        // Each extracts its own payload correctly.
        auto thinking_p = extract_thinking_started(thinking_ev);
        auto tool_p     = extract_tool_running(tool_ev);
        auto done_p     = extract_tool_done(done_ev);

        REQUIRE(thinking_p.has_value());
        REQUIRE(tool_p.has_value());
        CHECK(tool_p->tool_name == "Bash");
        REQUIRE(done_p.has_value());
    }

    TEST_CASE("[TUI-T15] thinking-stopped does not match tool-done events") {
        auto done_ev = make_tool_done_event();
        CHECK_FALSE(extract_thinking_stopped(done_ev).has_value());
        (void)extract_tool_done(done_ev);  // clean up
    }

    TEST_CASE("[TUI-T15] thinking-started does not match tool-running events") {
        auto running_ev = make_tool_running_event("Read");
        CHECK_FALSE(extract_thinking_started(running_ev).has_value());
        (void)extract_tool_running(running_ev);  // clean up
    }
}
