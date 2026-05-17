// =============================================================================
// tests/unit/test_agent_event.cpp — doctest suite for AgentEvent + AgentEventQueue
//
// Build standalone (no CMake):
//   c++ -std=c++20 -Iinclude \
//       -Ibuild/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_agent_event.cpp src/agents/AgentEvent.cpp \
//       -o /tmp/test_agent_event && /tmp/test_agent_event
//
// Coverage:
//   1.  All 10 Kind values defined and labelled correctly
//   2.  Named constructors set Kind + payload correctly
//   3.  ts is set to a non-zero time point on construction
//   4.  Queue basic push/try_pop round-trip
//   5.  Queue FIFO ordering under sequential push
//   6.  Queue MPSC: 1000 events pushed from 8 threads, drain asserts count
//   7.  Queue drain() returns all pending events and leaves queue empty
//   8.  Queue dirty_seq incremented per push
//   9.  wait_pop returns event when producer pushes
//   10. wait_pop cancellation via stop_token (no hang)
//   11. empty() + size() diagnostics
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "batbox/agents/AgentEvent.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

using batbox::agents::AgentEvent;
using batbox::agents::AgentEventQueue;
using Kind = AgentEvent::Kind;

// ---------------------------------------------------------------------------
// 1. All 10 Kind values exist and kind_label() returns non-empty strings
// ---------------------------------------------------------------------------
TEST_CASE("All 10 Kind values defined") {
    constexpr Kind all_kinds[] = {
        Kind::Started,
        Kind::StepBegan,
        Kind::TokenAppended,
        Kind::ToolCallBegan,
        Kind::ToolCallEnded,
        Kind::Completed,
        Kind::Errored,
        Kind::Cancelled,
        Kind::ParentMessageObserved,
        Kind::Queued,
    };
    CHECK(std::size(all_kinds) == 10u);

    for (Kind k : all_kinds) {
        const char* label = AgentEvent::kind_label(k);
        CHECK(label != nullptr);
        CHECK(std::string(label).size() > 0);
    }
}

TEST_CASE("kind_label returns distinct strings for each Kind") {
    std::vector<std::string> labels;
    for (int i = 0; i < 10; ++i) {
        labels.emplace_back(AgentEvent::kind_label(static_cast<Kind>(i)));
    }
    // All labels must be distinct
    std::sort(labels.begin(), labels.end());
    auto it = std::unique(labels.begin(), labels.end());
    CHECK(it == labels.end());
}

// ---------------------------------------------------------------------------
// 2. Named constructors — Kind + payload
// ---------------------------------------------------------------------------
TEST_CASE("make_started: Kind::Started, payload == display_name") {
    auto e = AgentEvent::make_started("agent-1", "MyAgent");
    CHECK(e.kind == Kind::Started);
    CHECK(e.agent_id == "agent-1");
    CHECK(e.payload == "MyAgent");
}

TEST_CASE("make_step_began: Kind::StepBegan, payload == 'name: desc'") {
    auto e = AgentEvent::make_step_began("agent-2", "ReasonStep", "thinking hard");
    CHECK(e.kind == Kind::StepBegan);
    CHECK(e.payload == "ReasonStep: thinking hard");
}

TEST_CASE("make_token_appended: Kind::TokenAppended, payload == chunk") {
    auto e = AgentEvent::make_token_appended("agent-3", "Hello");
    CHECK(e.kind == Kind::TokenAppended);
    CHECK(e.payload == "Hello");
}

TEST_CASE("make_tool_call_began: Kind::ToolCallBegan, payload == 'tool: json'") {
    auto e = AgentEvent::make_tool_call_began("agent-4", "Bash", "{\"cmd\":\"ls\"}");
    CHECK(e.kind == Kind::ToolCallBegan);
    CHECK(e.payload == "Bash: {\"cmd\":\"ls\"}");
}

TEST_CASE("make_tool_call_ended: Kind::ToolCallEnded") {
    auto e = AgentEvent::make_tool_call_ended("agent-5", "exit 0");
    CHECK(e.kind == Kind::ToolCallEnded);
    CHECK(e.payload == "exit 0");
}

TEST_CASE("make_completed: Kind::Completed") {
    auto e = AgentEvent::make_completed("agent-6", "task done");
    CHECK(e.kind == Kind::Completed);
    CHECK(e.payload == "task done");
}

TEST_CASE("make_errored: Kind::Errored") {
    auto e = AgentEvent::make_errored("agent-7", "HTTP 500");
    CHECK(e.kind == Kind::Errored);
    CHECK(e.payload == "HTTP 500");
}

TEST_CASE("make_cancelled: Kind::Cancelled, reason may be empty") {
    auto e1 = AgentEvent::make_cancelled("agent-8");
    CHECK(e1.kind == Kind::Cancelled);
    CHECK(e1.payload.empty());

    auto e2 = AgentEvent::make_cancelled("agent-8", "user requested");
    CHECK(e2.kind == Kind::Cancelled);
    CHECK(e2.payload == "user requested");
}

TEST_CASE("make_parent_message_observed: Kind::ParentMessageObserved") {
    auto e = AgentEvent::make_parent_message_observed("agent-9", "parent turn");
    CHECK(e.kind == Kind::ParentMessageObserved);
    CHECK(e.payload == "parent turn");
}

TEST_CASE("make_queued: Kind::Queued, position_hint may be empty") {
    auto e1 = AgentEvent::make_queued("agent-10");
    CHECK(e1.kind == Kind::Queued);
    CHECK(e1.payload.empty());

    auto e2 = AgentEvent::make_queued("agent-10", "queued, 1/4");
    CHECK(e2.kind == Kind::Queued);
    CHECK(e2.payload == "queued, 1/4");
}

// ---------------------------------------------------------------------------
// 3. Timestamp is set on construction and is a sane non-epoch time point
// ---------------------------------------------------------------------------
TEST_CASE("AgentEvent::ts is set to approximately now on construction") {
    using Clock = std::chrono::system_clock;
    auto before = Clock::now();
    auto e = AgentEvent::make_started("a", "A");
    auto after  = Clock::now();
    CHECK(e.ts >= before);
    CHECK(e.ts <= after);
}

// ---------------------------------------------------------------------------
// 4. Queue basic push / try_pop round-trip
// ---------------------------------------------------------------------------
TEST_CASE("Queue: push then try_pop returns the event") {
    AgentEventQueue q;
    auto e = AgentEvent::make_completed("agent-a", "done");
    q.push(std::move(e));
    auto result = q.try_pop();
    REQUIRE(result.has_value());
    CHECK(result->kind == Kind::Completed);
    CHECK(result->agent_id == "agent-a");
    CHECK(result->payload == "done");
}

TEST_CASE("Queue: try_pop on empty queue returns nullopt") {
    AgentEventQueue q;
    auto result = q.try_pop();
    CHECK_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// 5. FIFO ordering under sequential push
// ---------------------------------------------------------------------------
TEST_CASE("Queue: FIFO ordering — events pop in push order") {
    AgentEventQueue q;
    for (int i = 0; i < 5; ++i) {
        q.push(AgentEvent::make_token_appended("a", std::to_string(i)));
    }
    for (int i = 0; i < 5; ++i) {
        auto e = q.try_pop();
        REQUIRE(e.has_value());
        CHECK(e->payload == std::to_string(i));
    }
    CHECK_FALSE(q.try_pop().has_value());
}

// ---------------------------------------------------------------------------
// 6. MPSC: 1000 events from 8 producer threads; drain asserts total count
// ---------------------------------------------------------------------------
TEST_CASE("Queue MPSC: 1000 events pushed from 8 threads, drain returns all") {
    constexpr int kThreads = 8;
    constexpr int kPerThread = 125;          // 8 * 125 = 1000
    constexpr int kTotal = kThreads * kPerThread;

    AgentEventQueue q;
    std::atomic<int> ready_count{0};
    std::atomic<bool> go{false};

    // Launch producers; each waits for the start signal before pushing.
    std::vector<std::jthread> producers;
    producers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        producers.emplace_back([&, t](std::stop_token) {
            ++ready_count;
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < kPerThread; ++i) {
                q.push(AgentEvent::make_token_appended(
                    "agent-" + std::to_string(t),
                    std::to_string(t * kPerThread + i)));
            }
        });
    }

    // Wait for all producers to be ready, then release them together.
    while (ready_count.load() < kThreads) {
        std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);

    // Wait for producers to finish.
    producers.clear();    // joins all jthreads

    // Drain and count.
    auto events = q.drain();
    CHECK(static_cast<int>(events.size()) == kTotal);
    CHECK(q.empty());
}

// ---------------------------------------------------------------------------
// 7. drain() semantics — returns all pending, leaves queue empty
// ---------------------------------------------------------------------------
TEST_CASE("Queue drain: removes all events and returns them") {
    AgentEventQueue q;
    for (int i = 0; i < 20; ++i) {
        q.push(AgentEvent::make_token_appended("a", std::to_string(i)));
    }
    auto batch = q.drain();
    CHECK(batch.size() == 20u);
    CHECK(q.empty());
    CHECK(q.size() == 0u);
}

TEST_CASE("Queue drain on empty queue returns empty vector") {
    AgentEventQueue q;
    auto batch = q.drain();
    CHECK(batch.empty());
}

TEST_CASE("Queue drain preserves FIFO order") {
    AgentEventQueue q;
    for (int i = 0; i < 10; ++i) {
        q.push(AgentEvent::make_token_appended("a", std::to_string(i)));
    }
    auto batch = q.drain();
    for (int i = 0; i < 10; ++i) {
        CHECK(batch[static_cast<std::size_t>(i)].payload == std::to_string(i));
    }
}

// ---------------------------------------------------------------------------
// 8. dirty_seq incremented per push
// ---------------------------------------------------------------------------
TEST_CASE("Queue dirty_seq: starts at 0, increments on each push") {
    AgentEventQueue q;
    CHECK(q.dirty_seq() == 0u);
    for (uint64_t i = 1; i <= 10; ++i) {
        q.push(AgentEvent::make_started("a", "A"));
        CHECK(q.dirty_seq() == i);
    }
}

TEST_CASE("Queue dirty_seq: increments not reset by drain/try_pop") {
    AgentEventQueue q;
    q.push(AgentEvent::make_started("a", "A"));
    q.push(AgentEvent::make_started("a", "A"));
    CHECK(q.dirty_seq() == 2u);
    (void)q.drain();
    CHECK(q.dirty_seq() == 2u);   // drain does not reset the seq
    q.push(AgentEvent::make_started("a", "A"));
    CHECK(q.dirty_seq() == 3u);
}

// ---------------------------------------------------------------------------
// 9. wait_pop: returns event when producer pushes
// ---------------------------------------------------------------------------
TEST_CASE("Queue wait_pop: blocks until event is available") {
    AgentEventQueue q;
    std::stop_source stop_src;

    // Consumer runs in a separate thread and waits for one event.
    auto consumer_fut = std::async(std::launch::async, [&]() {
        return q.wait_pop(stop_src.get_token());
    });

    // Give the consumer thread a moment to enter wait state.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Producer pushes one event.
    q.push(AgentEvent::make_completed("agent-x", "result"));

    auto result = consumer_fut.get();
    REQUIRE(result.has_value());
    CHECK(result->kind == Kind::Completed);
    CHECK(result->agent_id == "agent-x");
}

// ---------------------------------------------------------------------------
// 10. wait_pop cancellation via stop_token — must not hang
// ---------------------------------------------------------------------------
TEST_CASE("Queue wait_pop: cancels cleanly via stop_token") {
    AgentEventQueue q;
    std::stop_source stop_src;

    auto consumer_fut = std::async(std::launch::async, [&]() {
        return q.wait_pop(stop_src.get_token());
    });

    // Give the consumer a moment to enter wait state, then cancel.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    stop_src.request_stop();

    auto result = consumer_fut.get();
    CHECK_FALSE(result.has_value());   // nullopt — cancelled before any event
}

// ---------------------------------------------------------------------------
// 11. empty() + size() diagnostics
// ---------------------------------------------------------------------------
TEST_CASE("Queue empty() and size() reflect queue state") {
    AgentEventQueue q;
    CHECK(q.empty());
    CHECK(q.size() == 0u);

    q.push(AgentEvent::make_queued("a", "queued, 1/4"));
    CHECK_FALSE(q.empty());
    CHECK(q.size() == 1u);

    q.push(AgentEvent::make_queued("b", "queued, 2/4"));
    CHECK(q.size() == 2u);

    (void)q.try_pop();
    CHECK(q.size() == 1u);

    (void)q.try_pop();
    CHECK(q.empty());
    CHECK(q.size() == 0u);
}
