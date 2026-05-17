// tests/integration/test_plan_approval_card.cpp
//
// Integration / unit tests for batbox::tui::PlanApprovalCard (TUI-PLAN-T2).
//
// These tests mirror the test_permission_card.cpp pattern:
//   - await_user_decision() blocks until a key is pressed
//   - OnEvent() resolves the card with the correct PlanApprovalResult
//   - pending() tracks the blocking state
//   - OnRender() produces non-empty output
//
// Build (standalone, no CMake, from repo root):
//   c++ -std=c++20                                                          \
//       -I$(pwd)/include                                                    \
//       -I$(pwd)/build/vcpkg_installed/arm64-osx/include                   \
//       tests/integration/test_plan_approval_card.cpp                      \
//       src/tui/PlanApprovalCard.cpp                                       \
//       src/tui/Events.cpp src/tui/ThemeApply.cpp                          \
//       src/theme/Theme.cpp src/theme/themes.cpp                           \
//       src/core/Json.cpp                                                   \
//       -L$(pwd)/build/vcpkg_installed/arm64-osx/lib                       \
//       -lftxui-component -lftxui-dom -lftxui-screen                       \
//       -o /tmp/test_plan_approval_card && /tmp/test_plan_approval_card
//
// Acceptance criteria tested:
//   [AC1] 'a' key    → PlanApprovalResult::Kind::Approved
//   [AC2] 'A' key    → PlanApprovalResult::Kind::Approved (case-insensitive)
//   [AC3] Enter key  → PlanApprovalResult::Kind::Approved
//   [AC4] 'r' key    → PlanApprovalResult::Kind::Rejected
//   [AC5] 'R' key    → PlanApprovalResult::Kind::Rejected (case-insensitive)
//   [AC6] Esc key    → PlanApprovalResult::Kind::Rejected (cancel = reject)
//   [AC7] 'e' key    → PlanApprovalResult::Kind::Edited, edit_feedback = plan_text
//   [AC8] 'E' key    → PlanApprovalResult::Kind::Edited (case-insensitive)
//   [AC9] Worker thread blocks in await_user_decision() until key is pressed.
//   [AC10] pending() returns true while blocked and false after resolve.
//   [AC11] OnRender() produces non-empty output without crashing.
//   [AC12] plan_text() accessor returns the current plan text.
//   [AC13] Card can be reused across multiple sequential decisions.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/tui/PlanApprovalCard.hpp>
#include <batbox/tui/ThemeApply.hpp>
#include <batbox/theme/Theme.hpp>
#include <batbox/core/Json.hpp>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

using namespace batbox::tui;
using namespace batbox::theme;

// =============================================================================
// Helpers
// =============================================================================

static Theme make_test_theme() {
    return theme_from_name("miss-kittin");
}

static const std::string kSamplePlan =
    "## Plan\n"
    "1. Create src/foo.cpp\n"
    "2. Add unit tests\n"
    "3. Update CMakeLists.txt\n";

// Helper: run await_user_decision on a background thread, fire event on the
// calling thread after a short delay, then join and return the result.
static PlanApprovalResult fire_key_and_await(
    PlanApprovalCard& card,
    const std::string& plan_text,
    ftxui::Event ev,
    std::chrono::milliseconds delay_ms = std::chrono::milliseconds(20))
{
    PlanApprovalResult result = PlanApprovalResult::rejected();

    std::thread worker([&]() {
        result = card.await_user_decision(plan_text);
    });

    std::this_thread::sleep_for(delay_ms);
    card.OnEvent(ev);
    worker.join();

    return result;
}

// =============================================================================
// [AC1] 'a' key — Approve (lowercase)
// =============================================================================
TEST_SUITE("PlanApprovalCard — approve lowercase [a]") {
    TEST_CASE("'a' produces Approved") {
        const auto theme = make_test_theme();
        auto card = std::make_shared<PlanApprovalCard>(theme);

        auto result = fire_key_and_await(*card, kSamplePlan,
                                         ftxui::Event::Character('a'));

        CHECK(result.kind == PlanApprovalResult::Kind::Approved);
        CHECK(result.edit_feedback.empty());
    }
}

// =============================================================================
// [AC2] 'A' key — Approve (uppercase, case-insensitive)
// =============================================================================
TEST_SUITE("PlanApprovalCard — approve uppercase [A]") {
    TEST_CASE("'A' produces Approved") {
        const auto theme = make_test_theme();
        auto card = std::make_shared<PlanApprovalCard>(theme);

        auto result = fire_key_and_await(*card, kSamplePlan,
                                         ftxui::Event::Character('A'));

        CHECK(result.kind == PlanApprovalResult::Kind::Approved);
        CHECK(result.edit_feedback.empty());
    }
}

// =============================================================================
// [AC3] Enter key — Approve (unambiguous affirmative)
// =============================================================================
TEST_SUITE("PlanApprovalCard — approve Enter") {
    TEST_CASE("Enter produces Approved") {
        const auto theme = make_test_theme();
        auto card = std::make_shared<PlanApprovalCard>(theme);

        auto result = fire_key_and_await(*card, kSamplePlan,
                                         ftxui::Event::Return);

        CHECK(result.kind == PlanApprovalResult::Kind::Approved);
        CHECK(result.edit_feedback.empty());
    }
}

// =============================================================================
// [AC4] 'r' key — Reject (lowercase)
// =============================================================================
TEST_SUITE("PlanApprovalCard — reject lowercase [r]") {
    TEST_CASE("'r' produces Rejected") {
        const auto theme = make_test_theme();
        auto card = std::make_shared<PlanApprovalCard>(theme);

        auto result = fire_key_and_await(*card, kSamplePlan,
                                         ftxui::Event::Character('r'));

        CHECK(result.kind == PlanApprovalResult::Kind::Rejected);
        CHECK(result.edit_feedback.empty());
    }
}

// =============================================================================
// [AC5] 'R' key — Reject (uppercase)
// =============================================================================
TEST_SUITE("PlanApprovalCard — reject uppercase [R]") {
    TEST_CASE("'R' produces Rejected") {
        const auto theme = make_test_theme();
        auto card = std::make_shared<PlanApprovalCard>(theme);

        auto result = fire_key_and_await(*card, kSamplePlan,
                                         ftxui::Event::Character('R'));

        CHECK(result.kind == PlanApprovalResult::Kind::Rejected);
        CHECK(result.edit_feedback.empty());
    }
}

// =============================================================================
// [AC6] Esc key — Reject (cancel = reject)
// =============================================================================
TEST_SUITE("PlanApprovalCard — cancel [Esc]") {
    TEST_CASE("Esc produces Rejected") {
        const auto theme = make_test_theme();
        auto card = std::make_shared<PlanApprovalCard>(theme);

        auto result = fire_key_and_await(*card, kSamplePlan,
                                         ftxui::Event::Escape);

        CHECK(result.kind == PlanApprovalResult::Kind::Rejected);
        CHECK(result.edit_feedback.empty());
    }
}

// =============================================================================
// [AC7] 'e' key — Edit (returns plan text as feedback)
// =============================================================================
TEST_SUITE("PlanApprovalCard — edit [e]") {
    TEST_CASE("'e' produces Edited with plan text in edit_feedback") {
        const auto theme = make_test_theme();
        auto card = std::make_shared<PlanApprovalCard>(theme);

        auto result = fire_key_and_await(*card, kSamplePlan,
                                         ftxui::Event::Character('e'));

        CHECK(result.kind == PlanApprovalResult::Kind::Edited);
        // edit_feedback should be the plan text
        REQUIRE(!result.edit_feedback.empty());
        CHECK(result.edit_feedback.find("Plan") != std::string::npos);
    }
}

// =============================================================================
// [AC8] 'E' key — Edit (uppercase)
// =============================================================================
TEST_SUITE("PlanApprovalCard — edit uppercase [E]") {
    TEST_CASE("'E' produces Edited with plan text in edit_feedback") {
        const auto theme = make_test_theme();
        auto card = std::make_shared<PlanApprovalCard>(theme);

        const std::string plan = "Step 1: write code\nStep 2: test it";
        auto result = fire_key_and_await(*card, plan,
                                         ftxui::Event::Character('E'));

        CHECK(result.kind == PlanApprovalResult::Kind::Edited);
        REQUIRE(!result.edit_feedback.empty());
        CHECK(result.edit_feedback.find("Step 1") != std::string::npos);
    }
}

// =============================================================================
// [AC9] Worker thread blocks until key pressed
// =============================================================================
TEST_SUITE("PlanApprovalCard — blocking await") {
    TEST_CASE("await_user_decision blocks until resolved") {
        const auto theme = make_test_theme();
        auto card = std::make_shared<PlanApprovalCard>(theme);

        std::atomic<bool> worker_started{false};
        std::atomic<bool> worker_done{false};

        std::thread worker([&]() {
            worker_started.store(true, std::memory_order_release);
            (void)card->await_user_decision(kSamplePlan);
            worker_done.store(true, std::memory_order_release);
        });

        // Wait for worker to start.
        while (!worker_started.load(std::memory_order_acquire))
            std::this_thread::yield();

        // Give it time to enter the wait — it should not have returned yet.
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        CHECK(!worker_done.load(std::memory_order_acquire));

        // Now resolve it.
        card->OnEvent(ftxui::Event::Character('a'));
        worker.join();
        CHECK(worker_done.load(std::memory_order_acquire));
    }
}

// =============================================================================
// [AC10] pending() lifecycle
// =============================================================================
TEST_SUITE("PlanApprovalCard — pending() accessor") {
    TEST_CASE("pending() returns false before await, true during, false after") {
        const auto theme = make_test_theme();
        auto card = std::make_shared<PlanApprovalCard>(theme);

        CHECK(!card->pending());  // not yet started

        std::thread worker([&]() {
            (void)card->await_user_decision(kSamplePlan);
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(card->pending());

        card->OnEvent(ftxui::Event::Character('r'));
        worker.join();
        CHECK(!card->pending());
    }
}

// =============================================================================
// [AC11] OnRender() does not crash and produces non-empty output
// =============================================================================
TEST_SUITE("PlanApprovalCard — render smoke tests") {
    TEST_CASE("OnRender() without pending await does not crash") {
        const auto theme = make_test_theme();
        auto card = std::make_shared<PlanApprovalCard>(theme);

        auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(80),
                                             ftxui::Dimension::Fixed(24));
        REQUIRE_NOTHROW(ftxui::Render(screen, card->OnRender()));
        const std::string output = screen.ToString();
        CHECK(!output.empty());
    }

    TEST_CASE("OnRender() while await is pending contains key hints") {
        const auto theme = make_test_theme();
        auto card = std::make_shared<PlanApprovalCard>(theme);

        std::thread worker([&]() {
            (void)card->await_user_decision(kSamplePlan);
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(80),
                                             ftxui::Dimension::Fixed(30));
        REQUIRE_NOTHROW(ftxui::Render(screen, card->OnRender()));
        const std::string output = screen.ToString();
        CHECK(!output.empty());
        // Should contain approval and reject hints.
        CHECK(output.find("[A]") != std::string::npos);
        CHECK(output.find("[R]") != std::string::npos);

        card->OnEvent(ftxui::Event::Character('r'));
        worker.join();
    }

    TEST_CASE("OnRender() while pending contains plan text") {
        const auto theme = make_test_theme();
        auto card = std::make_shared<PlanApprovalCard>(theme);
        const std::string plan = "unique-plan-keyword-xyzzy";

        std::thread worker([&]() {
            (void)card->await_user_decision(plan);
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(80),
                                             ftxui::Dimension::Fixed(30));
        ftxui::Render(screen, card->OnRender());
        const std::string output = screen.ToString();
        CHECK(output.find("unique-plan-keyword-xyzzy") != std::string::npos);

        card->OnEvent(ftxui::Event::Escape);
        worker.join();
    }
}

// =============================================================================
// [AC12] plan_text() accessor
// =============================================================================
TEST_SUITE("PlanApprovalCard — plan_text() accessor") {
    TEST_CASE("plan_text() is empty before await and set during await") {
        const auto theme = make_test_theme();
        auto card = std::make_shared<PlanApprovalCard>(theme);

        CHECK(card->plan_text().empty());

        const std::string plan = "## Phase 1\nDo the thing.\n";
        std::thread worker([&]() {
            (void)card->await_user_decision(plan);
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(card->plan_text().find("Phase 1") != std::string::npos);

        card->OnEvent(ftxui::Event::Character('a'));
        worker.join();
    }
}

// =============================================================================
// [AC13] Card can be reused across multiple sequential decisions
// =============================================================================
TEST_SUITE("PlanApprovalCard — reuse across multiple requests") {
    TEST_CASE("Card can handle two sequential plan approvals") {
        const auto theme = make_test_theme();
        auto card = std::make_shared<PlanApprovalCard>(theme);

        // First: approve
        {
            auto result = fire_key_and_await(*card, "Plan v1",
                                             ftxui::Event::Character('a'));
            CHECK(result.kind == PlanApprovalResult::Kind::Approved);
        }

        // Second: reject
        {
            auto result = fire_key_and_await(*card, "Plan v2",
                                             ftxui::Event::Character('r'));
            CHECK(result.kind == PlanApprovalResult::Kind::Rejected);
        }

        // Third: edit
        {
            auto result = fire_key_and_await(*card, "Plan v3",
                                             ftxui::Event::Character('e'));
            CHECK(result.kind == PlanApprovalResult::Kind::Edited);
            CHECK(result.edit_feedback.find("Plan v3") != std::string::npos);
        }
    }
}
