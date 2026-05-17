// ---------------------------------------------------------------------------
// tests/unit/test_sidecar_state.cpp
//
// Unit tests for batbox::sidecar::SidecarState, to_string(),
// is_legal_transition(), and SidecarStateMachine.
//
// Covers:
//   - All 5 states have correct to_string() labels
//   - All 10 legal transitions are accepted
//   - Illegal transitions (e.g. Disabled→Running, Cold→Running) are rejected
//   - try_transition() CAS semantics: winner returns true, loser returns false
//   - Concurrent state-change attempts: exactly one thread wins each race
//   - on_transition callback fires exactly once per successful transition
//   - try_transition() with an illegal edge never fires the callback
//
// Build (standalone, no CMake):
//   c++ -std=c++20 -Iinclude \
//       -I<path-to-doctest> \
//       tests/unit/test_sidecar_state.cpp \
//       src/sidecar/SidecarState.cpp \
//       -lpthread \
//       -o /tmp/test_sidecar_state && /tmp/test_sidecar_state
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "batbox/sidecar/SidecarState.hpp"

#include <atomic>
#include <chrono>
#include <string_view>
#include <thread>
#include <vector>

using namespace batbox::sidecar;
using namespace std::chrono_literals;

// ============================================================================
// TEST SUITE 1: to_string()
// ============================================================================
TEST_SUITE("SidecarState — to_string") {

    TEST_CASE("Disabled maps to 'disabled'") {
        CHECK(to_string(SidecarState::Disabled) == "disabled");
    }

    TEST_CASE("Cold maps to 'cold'") {
        CHECK(to_string(SidecarState::Cold) == "cold");
    }

    TEST_CASE("Starting maps to 'starting'") {
        CHECK(to_string(SidecarState::Starting) == "starting");
    }

    TEST_CASE("Running maps to 'running'") {
        CHECK(to_string(SidecarState::Running) == "running");
    }

    TEST_CASE("CrashedRestarting maps to 'crashed-restarting'") {
        CHECK(to_string(SidecarState::CrashedRestarting) == "crashed-restarting");
    }

    TEST_CASE("to_string returns non-empty string_view for all states") {
        const SidecarState all_states[] = {
            SidecarState::Disabled,
            SidecarState::Cold,
            SidecarState::Starting,
            SidecarState::Running,
            SidecarState::CrashedRestarting,
        };
        for (auto s : all_states) {
            CHECK_FALSE(to_string(s).empty());
        }
    }
}

// ============================================================================
// TEST SUITE 2: is_legal_transition() — allowed edges
// ============================================================================
TEST_SUITE("SidecarState — is_legal_transition allowed") {

    TEST_CASE("Disabled → Cold (re-enable)") {
        CHECK(is_legal_transition(SidecarState::Disabled, SidecarState::Cold));
    }

    TEST_CASE("Cold → Starting (first web call)") {
        CHECK(is_legal_transition(SidecarState::Cold, SidecarState::Starting));
    }

    TEST_CASE("Cold → Disabled (explicit disable without ever running)") {
        CHECK(is_legal_transition(SidecarState::Cold, SidecarState::Disabled));
    }

    TEST_CASE("Starting → Running (health-check passed)") {
        CHECK(is_legal_transition(SidecarState::Starting, SidecarState::Running));
    }

    TEST_CASE("Starting → CrashedRestarting (health-check timeout)") {
        CHECK(is_legal_transition(SidecarState::Starting, SidecarState::CrashedRestarting));
    }

    TEST_CASE("Starting → Disabled (shutdown during startup)") {
        CHECK(is_legal_transition(SidecarState::Starting, SidecarState::Disabled));
    }

    TEST_CASE("Running → CrashedRestarting (unexpected child exit)") {
        CHECK(is_legal_transition(SidecarState::Running, SidecarState::CrashedRestarting));
    }

    TEST_CASE("Running → Disabled (graceful shutdown)") {
        CHECK(is_legal_transition(SidecarState::Running, SidecarState::Disabled));
    }

    TEST_CASE("CrashedRestarting → Starting (restart attempt)") {
        CHECK(is_legal_transition(SidecarState::CrashedRestarting, SidecarState::Starting));
    }

    TEST_CASE("CrashedRestarting → Disabled (max retries exhausted)") {
        CHECK(is_legal_transition(SidecarState::CrashedRestarting, SidecarState::Disabled));
    }
}

// ============================================================================
// TEST SUITE 3: is_legal_transition() — rejected edges
// ============================================================================
TEST_SUITE("SidecarState — is_legal_transition rejected") {

    TEST_CASE("Disabled → Running (must go through Cold→Starting→Running)") {
        CHECK_FALSE(is_legal_transition(SidecarState::Disabled, SidecarState::Running));
    }

    TEST_CASE("Disabled → Starting (skip not allowed)") {
        CHECK_FALSE(is_legal_transition(SidecarState::Disabled, SidecarState::Starting));
    }

    TEST_CASE("Disabled → CrashedRestarting") {
        CHECK_FALSE(is_legal_transition(SidecarState::Disabled, SidecarState::CrashedRestarting));
    }

    TEST_CASE("Cold → Running (must go through Starting)") {
        CHECK_FALSE(is_legal_transition(SidecarState::Cold, SidecarState::Running));
    }

    TEST_CASE("Cold → CrashedRestarting") {
        CHECK_FALSE(is_legal_transition(SidecarState::Cold, SidecarState::CrashedRestarting));
    }

    TEST_CASE("Running → Cold (no reverse path)") {
        CHECK_FALSE(is_legal_transition(SidecarState::Running, SidecarState::Cold));
    }

    TEST_CASE("Running → Starting (must crash-restart first)") {
        CHECK_FALSE(is_legal_transition(SidecarState::Running, SidecarState::Starting));
    }

    TEST_CASE("CrashedRestarting → Running (must go through Starting)") {
        CHECK_FALSE(is_legal_transition(SidecarState::CrashedRestarting, SidecarState::Running));
    }

    TEST_CASE("CrashedRestarting → Cold") {
        CHECK_FALSE(is_legal_transition(SidecarState::CrashedRestarting, SidecarState::Cold));
    }

    TEST_CASE("self-transitions are illegal") {
        CHECK_FALSE(is_legal_transition(SidecarState::Cold,    SidecarState::Cold));
        CHECK_FALSE(is_legal_transition(SidecarState::Running, SidecarState::Running));
    }
}

// ============================================================================
// TEST SUITE 4: SidecarStateMachine basic usage
// ============================================================================
TEST_SUITE("SidecarStateMachine — basic") {

    TEST_CASE("default initial state is Cold") {
        SidecarStateMachine sm;
        CHECK(sm.current() == SidecarState::Cold);
    }

    TEST_CASE("custom initial state is honored") {
        SidecarStateMachine sm{SidecarState::Disabled};
        CHECK(sm.current() == SidecarState::Disabled);
    }

    TEST_CASE("legal transition returns true and advances state") {
        SidecarStateMachine sm;
        const bool ok = sm.try_transition(SidecarState::Cold, SidecarState::Starting);
        CHECK(ok);
        CHECK(sm.current() == SidecarState::Starting);
    }

    TEST_CASE("illegal transition returns false and leaves state unchanged") {
        SidecarStateMachine sm;  // Cold
        const bool ok = sm.try_transition(SidecarState::Cold, SidecarState::Running);
        CHECK_FALSE(ok);
        CHECK(sm.current() == SidecarState::Cold);
    }

    TEST_CASE("try_transition fails when from != current (stale from value)") {
        SidecarStateMachine sm;  // Cold
        // `from` is wrong — we pass Starting when machine is Cold
        const bool ok = sm.try_transition(SidecarState::Starting, SidecarState::Running);
        CHECK_FALSE(ok);
        CHECK(sm.current() == SidecarState::Cold);
    }

    TEST_CASE("full happy-path sequence: Cold → Starting → Running → Disabled") {
        SidecarStateMachine sm;
        CHECK(sm.try_transition(SidecarState::Cold,     SidecarState::Starting));
        CHECK(sm.current() == SidecarState::Starting);

        CHECK(sm.try_transition(SidecarState::Starting, SidecarState::Running));
        CHECK(sm.current() == SidecarState::Running);

        CHECK(sm.try_transition(SidecarState::Running,  SidecarState::Disabled));
        CHECK(sm.current() == SidecarState::Disabled);
    }

    TEST_CASE("crash-restart cycle: Running → CrashedRestarting → Starting → Running") {
        SidecarStateMachine sm{SidecarState::Running};

        CHECK(sm.try_transition(SidecarState::Running,           SidecarState::CrashedRestarting));
        CHECK(sm.current() == SidecarState::CrashedRestarting);

        CHECK(sm.try_transition(SidecarState::CrashedRestarting, SidecarState::Starting));
        CHECK(sm.current() == SidecarState::Starting);

        CHECK(sm.try_transition(SidecarState::Starting,          SidecarState::Running));
        CHECK(sm.current() == SidecarState::Running);
    }
}

// ============================================================================
// TEST SUITE 5: on_transition callback
// ============================================================================
TEST_SUITE("SidecarStateMachine — on_transition callback") {

    TEST_CASE("callback fires on successful transition") {
        SidecarStateMachine sm;
        int call_count = 0;
        SidecarState captured_old{SidecarState::Disabled};
        SidecarState captured_new{SidecarState::Disabled};

        sm.set_on_transition([&](SidecarState old_s, SidecarState new_s) {
            ++call_count;
            captured_old = old_s;
            captured_new = new_s;
        });

        (void)sm.try_transition(SidecarState::Cold, SidecarState::Starting);

        CHECK(call_count == 1);
        CHECK(captured_old == SidecarState::Cold);
        CHECK(captured_new == SidecarState::Starting);
    }

    TEST_CASE("callback does NOT fire on illegal transition") {
        SidecarStateMachine sm;  // Cold
        int call_count = 0;
        sm.set_on_transition([&](SidecarState, SidecarState) { ++call_count; });

        (void)sm.try_transition(SidecarState::Cold, SidecarState::Running);  // illegal

        CHECK(call_count == 0);
        CHECK(sm.current() == SidecarState::Cold);
    }

    TEST_CASE("callback does NOT fire when CAS fails (stale from)") {
        SidecarStateMachine sm;  // Cold
        int call_count = 0;
        sm.set_on_transition([&](SidecarState, SidecarState) { ++call_count; });

        // Attempt transition where `from` doesn't match current state
        (void)sm.try_transition(SidecarState::Starting, SidecarState::Running);  // from is wrong

        CHECK(call_count == 0);
    }

    TEST_CASE("callback fires once per transition across multiple transitions") {
        SidecarStateMachine sm;
        int call_count = 0;
        sm.set_on_transition([&](SidecarState, SidecarState) { ++call_count; });

        (void)sm.try_transition(SidecarState::Cold,     SidecarState::Starting);
        (void)sm.try_transition(SidecarState::Starting, SidecarState::Running);
        (void)sm.try_transition(SidecarState::Running,  SidecarState::Disabled);

        CHECK(call_count == 3);
    }

    TEST_CASE("set_on_transition(nullptr) clears callback") {
        SidecarStateMachine sm;
        int call_count = 0;
        sm.set_on_transition([&](SidecarState, SidecarState) { ++call_count; });

        (void)sm.try_transition(SidecarState::Cold, SidecarState::Starting);
        CHECK(call_count == 1);

        sm.set_on_transition(nullptr);
        (void)sm.try_transition(SidecarState::Starting, SidecarState::Running);
        CHECK(call_count == 1);  // still 1 — callback was cleared
    }
}

// ============================================================================
// TEST SUITE 6: Concurrent CAS atomicity
//
// Multiple threads all attempt the same Cold→Starting transition simultaneously.
// Exactly one must succeed (return true); all others must fail (return false).
// ============================================================================
TEST_SUITE("SidecarStateMachine — concurrent atomicity") {

    TEST_CASE("only one thread wins Cold→Starting race") {
        SidecarStateMachine sm;  // Cold

        constexpr int kThreads = 8;
        std::atomic<int> win_count{0};

        std::vector<std::thread> threads;
        threads.reserve(kThreads);

        // Barrier: all threads spin until released so they race as tightly as possible.
        std::atomic<bool> go{false};

        for (int i = 0; i < kThreads; ++i) {
            threads.emplace_back([&] {
                while (!go.load(std::memory_order_acquire)) {
                    // busy-wait
                }
                if (sm.try_transition(SidecarState::Cold, SidecarState::Starting)) {
                    win_count.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        go.store(true, std::memory_order_release);
        for (auto& t : threads) {
            t.join();
        }

        CHECK(win_count.load() == 1);
        CHECK(sm.current() == SidecarState::Starting);
    }

    TEST_CASE("callback fires exactly once in concurrent race") {
        SidecarStateMachine sm;

        std::atomic<int> callback_count{0};
        sm.set_on_transition([&](SidecarState, SidecarState) {
            callback_count.fetch_add(1, std::memory_order_relaxed);
        });

        constexpr int kThreads = 8;
        std::atomic<bool> go{false};
        std::vector<std::thread> threads;
        threads.reserve(kThreads);

        for (int i = 0; i < kThreads; ++i) {
            threads.emplace_back([&] {
                while (!go.load(std::memory_order_acquire)) {}
                (void)sm.try_transition(SidecarState::Cold, SidecarState::Starting);
            });
        }

        go.store(true, std::memory_order_release);
        for (auto& t : threads) {
            t.join();
        }

        CHECK(callback_count.load() == 1);
    }

    TEST_CASE("sequential state advancement across threads is race-free") {
        // Simulate the normal lifecycle driven by different threads:
        // Thread 1: Cold→Starting (spawn thread)
        // Thread 2: Starting→Running (health-check thread)
        // Thread 3: Running→CrashedRestarting (monitor thread)
        // Thread 4: CrashedRestarting→Starting (restart thread)

        SidecarStateMachine sm;

        auto advance = [&](SidecarState from, SidecarState to) {
            // Spin until the state matches `from`, then win the transition.
            for (;;) {
                if (sm.try_transition(from, to)) return;
                // Yield to avoid tight-loop starvation.
                std::this_thread::yield();
            }
        };

        std::thread t1([&]{ advance(SidecarState::Cold,             SidecarState::Starting); });
        std::thread t2([&]{ advance(SidecarState::Starting,         SidecarState::Running); });
        std::thread t3([&]{ advance(SidecarState::Running,          SidecarState::CrashedRestarting); });
        std::thread t4([&]{ advance(SidecarState::CrashedRestarting,SidecarState::Starting); });

        t1.join();
        t2.join();
        t3.join();
        t4.join();

        CHECK(sm.current() == SidecarState::Starting);
    }
}
