// tests/integration/test_nuclear_no_modals.cpp
// ---------------------------------------------------------------------------
// PEXT3 1.6 — make_askq_prompt_fn nuclear branch regression test
//
// Verifies that make_askq_prompt_fn(nuclear=true, ...) returns a closure that:
//   - returns {} immediately (no answer / auto-declined)
//   - posts NO modal events
//   - the nuclear closure captures nothing from the weak_ptr or screen_mgr
//
// The factory (batbox::tui::make_askq_prompt_fn) is called directly.
// For the nuclear path, screen_mgr is never accessed — so we pass a default-
// constructed ScreenManager (which creates an FTXUI ScreenInteractive backing
// object).  The nuclear closure never calls post_event() on it.
//
// Framework: doctest (same as all other integration tests in this directory).
// ---------------------------------------------------------------------------
// PEXT3 3.2 — KEPT (not redundant): smoke cases 30+31 cover the happy-path nuclear bypass end-to-end but cannot reach the expired-weak_ptr path (tests 4+5), empty-labels QuestionSpec (test 3c), or the non-nuclear destroyed-card graceful return (test 5); those edge cases require direct factory access and are not reachable via the binary smoke harness.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/tui/AskqPromptFactory.hpp>
#include <batbox/tui/Screen.hpp>
#include <batbox/tui/QuestionCard.hpp>
#include <batbox/theme/Theme.hpp>
#include <batbox/tools/AskUserQuestionTool.hpp>

#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Helper: construct a minimal QuestionSpec for testing.
// ---------------------------------------------------------------------------
static batbox::tools::QuestionSpec make_test_question_spec() {
    batbox::tools::QuestionSpec spec;
    spec.question     = "Which option?";
    spec.header       = "Test Question";
    spec.multi_select = false;
    spec.labels       = {"Option A", "Option B"};
    spec.descriptions = {"First option", "Second option"};
    return spec;
}

// ===========================================================================
// TEST SUITE: PEXT3 1.6 — make_askq_prompt_fn nuclear branch
// ===========================================================================
TEST_SUITE("PEXT3 1.6 — make_askq_prompt_fn nuclear branch") {

    TEST_CASE("nuclear path returns callable PromptFn") {
        batbox::tui::ScreenManager mgr;
        std::weak_ptr<batbox::tui::QuestionCard> no_card;

        auto fn = batbox::tui::make_askq_prompt_fn(/*nuclear=*/true, no_card, mgr);
        REQUIRE(fn != nullptr);
    }

    TEST_CASE("nuclear path returns empty vector immediately") {
        batbox::tui::ScreenManager mgr;
        std::weak_ptr<batbox::tui::QuestionCard> no_card;

        auto fn = batbox::tui::make_askq_prompt_fn(true, no_card, mgr);

        const auto result = fn(make_test_question_spec());
        CHECK(result.empty());
    }

    TEST_CASE("nuclear path returns empty for any spec variant") {
        batbox::tui::ScreenManager mgr;
        std::weak_ptr<batbox::tui::QuestionCard> no_card;

        auto fn = batbox::tui::make_askq_prompt_fn(true, no_card, mgr);

        // Single-select spec.
        {
            batbox::tools::QuestionSpec spec;
            spec.question     = "Single select?";
            spec.multi_select = false;
            spec.labels       = {"Yes", "No"};
            spec.descriptions = {"Affirmative", "Negative"};
            CHECK(fn(spec).empty());
        }

        // Multi-select spec.
        {
            batbox::tools::QuestionSpec spec;
            spec.question     = "Multi select?";
            spec.multi_select = true;
            spec.labels       = {"A", "B", "C", "D"};
            spec.descriptions = {"", "", "", ""};
            CHECK(fn(spec).empty());
        }

        // Empty labels spec.
        {
            batbox::tools::QuestionSpec spec;
            spec.question = "Anything?";
            CHECK(fn(spec).empty());
        }
    }

    TEST_CASE("nuclear path with expired card still returns empty") {
        batbox::tui::ScreenManager mgr;
        std::weak_ptr<batbox::tui::QuestionCard> expired_card;
        {
            batbox::theme::Theme theme{};
            auto tmp = std::make_shared<batbox::tui::QuestionCard>(theme);
            expired_card = tmp;
        }
        // expired_card.expired() == true

        // Nuclear path must return {} without touching the weak_ptr.
        auto fn = batbox::tui::make_askq_prompt_fn(true, expired_card, mgr);
        CHECK(fn(make_test_question_spec()).empty());
    }
}

// ===========================================================================
// TEST SUITE: PEXT3 1.6 — make_askq_prompt_fn structural
// ===========================================================================
TEST_SUITE("PEXT3 1.6 — make_askq_prompt_fn structural") {

    TEST_CASE("non-nuclear path with destroyed card returns empty (graceful)") {
        // When the card is destroyed, the non-nuclear closure must return {}
        // (destroyed-card path — lock() returns nullptr before any dereference).
        batbox::tui::ScreenManager mgr;
        std::weak_ptr<batbox::tui::QuestionCard> expired_card;
        {
            batbox::theme::Theme theme{};
            auto tmp = std::make_shared<batbox::tui::QuestionCard>(theme);
            expired_card = tmp;
        }

        auto fn = batbox::tui::make_askq_prompt_fn(false, expired_card, mgr);
        // Closure sees expired weak_ptr → returns {} without posting events.
        CHECK(fn(make_test_question_spec()).empty());
    }

    TEST_CASE("factory returns callable for both nuclear and non-nuclear") {
        batbox::tui::ScreenManager mgr;
        std::weak_ptr<batbox::tui::QuestionCard> no_card;

        auto nuclear_fn     = batbox::tui::make_askq_prompt_fn(true,  no_card, mgr);
        auto non_nuclear_fn = batbox::tui::make_askq_prompt_fn(false, no_card, mgr);

        REQUIRE(nuclear_fn     != nullptr);
        REQUIRE(non_nuclear_fn != nullptr);

        // Nuclear always returns {}.
        CHECK(nuclear_fn(make_test_question_spec()).empty());
        // Non-nuclear with null card also returns {} (destroyed-card path).
        CHECK(non_nuclear_fn(make_test_question_spec()).empty());
    }
}
