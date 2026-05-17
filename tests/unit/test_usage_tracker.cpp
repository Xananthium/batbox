// tests/unit/test_usage_tracker.cpp
// =============================================================================
// doctest suite for:
//   batbox::inference::ModelPricing  (ModelPricing.hpp / ModelPricing.cpp)
//   batbox::inference::UsageTracker  (UsageTracker.hpp / UsageTracker.cpp)
//
// Build standalone (from repo root):
//   c++ -std=c++20 -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_usage_tracker.cpp \
//       src/inference/ModelPricing.cpp src/inference/UsageTracker.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       -o /tmp/test_usage && /tmp/test_usage
//
// Via CMake + CTest:
//   cmake --build build && ctest --test-dir build -R test_usage_tracker -V
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/inference/ModelPricing.hpp>
#include <batbox/inference/UsageTracker.hpp>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using batbox::inference::ModelPricing;
using batbox::inference::UsageDelta;
using batbox::inference::UsageTracker;

// =============================================================================
// Helpers
// =============================================================================

static constexpr double kEps4 = 5e-5;  // tolerance for 4-decimal-place results

/// Round a double to 4 decimal places (mirrors ModelPricing::cost behaviour).
static double round4(double v) {
    return std::round(v * 10'000.0) / 10'000.0;
}

// =============================================================================
// TEST SUITE 1: ModelPricing — built-in table lookups
// =============================================================================
TEST_SUITE("ModelPricing — built-in table") {

    TEST_CASE("gpt-4o known pricing") {
        // gpt-4o: $2.50/M prompt, $10.00/M completion
        // cost(1000, 500) = (1000 * 2.50 + 500 * 10.00) / 1e6
        //                 = (2500 + 5000) / 1e6 = 0.0075
        const double expected = round4((1000.0 * 2.50 + 500.0 * 10.00) / 1'000'000.0);
        const double actual   = ModelPricing::cost("gpt-4o", 1000, 500);
        CHECK_MESSAGE(std::abs(actual - expected) < kEps4,
            "gpt-4o cost mismatch: expected " << expected << " got " << actual);
    }

    TEST_CASE("gpt-4o-mini known pricing") {
        // $0.15/M prompt, $0.60/M completion
        const double expected = round4((2000.0 * 0.15 + 1000.0 * 0.60) / 1'000'000.0);
        const double actual   = ModelPricing::cost("gpt-4o-mini", 2000, 1000);
        CHECK(std::abs(actual - expected) < kEps4);
    }

    TEST_CASE("o1-preview known pricing") {
        // $15.00/M prompt, $60.00/M completion
        const double expected = round4((500.0 * 15.00 + 200.0 * 60.00) / 1'000'000.0);
        const double actual   = ModelPricing::cost("o1-preview", 500, 200);
        CHECK(std::abs(actual - expected) < kEps4);
    }

    TEST_CASE("claude-3-5-sonnet-20241022 returns non-zero cost") {
        const double c = ModelPricing::cost("claude-3-5-sonnet-20241022", 1000, 500);
        CHECK(c > 0.0);
    }

    TEST_CASE("claude-sonnet-4-5 placeholder returns zero cost") {
        // Placeholder models are zero-cost until the user provides an override.
        const double c = ModelPricing::cost("claude-sonnet-4-5", 1000, 1000);
        CHECK(c == doctest::Approx(0.0));
    }

    TEST_CASE("unknown model returns zero cost") {
        const double c = ModelPricing::cost("unknown-model-xyz-999", 5000, 2000);
        CHECK(c == doctest::Approx(0.0));
    }

    TEST_CASE("zero tokens yields zero cost") {
        CHECK(ModelPricing::cost("gpt-4o", 0, 0) == doctest::Approx(0.0));
    }

    TEST_CASE("result is rounded to 4 decimal places") {
        // gpt-4o: cost for 1 prompt token, 1 completion token
        // = (1 * 2.50 + 1 * 10.00) / 1e6 = 0.0000125 -> rounds to 0.0000
        // For a more interesting rounding case, use 13 prompt tokens:
        // = (13 * 2.50 + 7 * 10.00) / 1e6 = (32.5 + 70) / 1e6 = 0.0001025 -> 0.0001
        const double actual = ModelPricing::cost("gpt-4o", 13, 7);
        // Verify the result has at most 4 significant decimal places.
        const double rounded = std::round(actual * 10'000.0) / 10'000.0;
        CHECK(actual == doctest::Approx(rounded).epsilon(1e-10));
    }

    TEST_CASE("cost is deterministic across repeated calls") {
        const double c1 = ModelPricing::cost("gpt-4o", 1000, 500);
        const double c2 = ModelPricing::cost("gpt-4o", 1000, 500);
        CHECK(c1 == doctest::Approx(c2));
    }
}

// =============================================================================
// TEST SUITE 2: ModelPricing — override file
// =============================================================================
TEST_SUITE("ModelPricing — override file") {

    TEST_CASE("override replaces built-in entry for matched model") {
        // Write a temporary override file.
        const std::string tmp_path = "/tmp/batbox_test_pricing_override.json";
        {
            std::ofstream f(tmp_path);
            f << R"JSON({
  "models": [
    {
      "id": "gpt-4o",
      "pricing": {
        "prompt_per_million": 1.00,
        "completion_per_million": 2.00
      }
    }
  ]
})JSON";
        }

        // Set environment variable and re-initialise the table.
        ::setenv("BATBOX_MODEL_PRICING_OVERRIDE", tmp_path.c_str(), 1);
        ModelPricing::reset_for_testing();

        // cost("gpt-4o", 1000, 500) with override = (1000*1 + 500*2) / 1e6 = 0.002
        const double expected = round4((1000.0 * 1.00 + 500.0 * 2.00) / 1'000'000.0);
        const double actual   = ModelPricing::cost("gpt-4o", 1000, 500);
        CHECK(std::abs(actual - expected) < kEps4);

        // Clean up: restore original table.
        ::unsetenv("BATBOX_MODEL_PRICING_OVERRIDE");
        ModelPricing::reset_for_testing();
    }

    TEST_CASE("override does not affect unmatched models") {
        const std::string tmp_path = "/tmp/batbox_test_pricing_override2.json";
        {
            std::ofstream f(tmp_path);
            f << R"JSON({"models":[{"id":"gpt-4o","pricing":{"prompt_per_million":1.00,"completion_per_million":2.00}}]})JSON";
        }
        ::setenv("BATBOX_MODEL_PRICING_OVERRIDE", tmp_path.c_str(), 1);
        ModelPricing::reset_for_testing();

        // gpt-4o-mini should still have its built-in price.
        const double expected = round4((1000.0 * 0.15 + 500.0 * 0.60) / 1'000'000.0);
        const double actual   = ModelPricing::cost("gpt-4o-mini", 1000, 500);
        CHECK(std::abs(actual - expected) < kEps4);

        ::unsetenv("BATBOX_MODEL_PRICING_OVERRIDE");
        ModelPricing::reset_for_testing();
    }

    TEST_CASE("missing override file is silently ignored") {
        ::setenv("BATBOX_MODEL_PRICING_OVERRIDE", "/nonexistent/path/to/pricing.json", 1);
        ModelPricing::reset_for_testing();

        // Built-in table should still work.
        const double c = ModelPricing::cost("gpt-4o", 1000, 500);
        CHECK(c > 0.0);

        ::unsetenv("BATBOX_MODEL_PRICING_OVERRIDE");
        ModelPricing::reset_for_testing();
    }

    TEST_CASE("malformed override file is silently ignored") {
        const std::string tmp_path = "/tmp/batbox_test_pricing_malformed.json";
        {
            std::ofstream f(tmp_path);
            f << "this is not valid JSON {{{";
        }
        ::setenv("BATBOX_MODEL_PRICING_OVERRIDE", tmp_path.c_str(), 1);
        ModelPricing::reset_for_testing();

        // Built-in table should still work.
        const double c = ModelPricing::cost("gpt-4o", 1000, 500);
        CHECK(c > 0.0);

        ::unsetenv("BATBOX_MODEL_PRICING_OVERRIDE");
        ModelPricing::reset_for_testing();
    }
}

// =============================================================================
// TEST SUITE 3: UsageTracker — basic accumulation
// =============================================================================
TEST_SUITE("UsageTracker — basic accumulation") {

    TEST_CASE("fresh tracker has zero totals") {
        UsageTracker tracker;
        const UsageDelta s = tracker.session_total();
        const UsageDelta t = tracker.turn_total();
        CHECK(s.prompt_tokens     == 0);
        CHECK(s.completion_tokens == 0);
        CHECK(s.total_tokens      == 0);
        CHECK(s.cost_usd          == doctest::Approx(0.0));
        CHECK(t.prompt_tokens     == 0);
        CHECK(t.completion_tokens == 0);
        CHECK(t.total_tokens      == 0);
        CHECK(t.cost_usd          == doctest::Approx(0.0));
    }

    TEST_CASE("add updates both session and turn counters") {
        UsageTracker tracker;
        UsageDelta d;
        d.prompt_tokens     = 100;
        d.completion_tokens = 50;
        d.total_tokens      = 150;
        d.cost_usd          = 0.0030;

        tracker.add(d);

        const UsageDelta s = tracker.session_total();
        const UsageDelta t = tracker.turn_total();

        CHECK(s.prompt_tokens     == 100);
        CHECK(s.completion_tokens == 50);
        CHECK(s.total_tokens      == 150);
        CHECK(s.cost_usd          == doctest::Approx(0.0030).epsilon(1e-5));

        CHECK(t.prompt_tokens     == 100);
        CHECK(t.completion_tokens == 50);
        CHECK(t.total_tokens      == 150);
        CHECK(t.cost_usd          == doctest::Approx(0.0030).epsilon(1e-5));
    }

    TEST_CASE("multiple adds accumulate correctly") {
        UsageTracker tracker;

        UsageDelta d1;
        d1.prompt_tokens     = 100;
        d1.completion_tokens = 50;
        d1.total_tokens      = 150;
        d1.cost_usd          = 0.0010;
        tracker.add(d1);

        UsageDelta d2;
        d2.prompt_tokens     = 200;
        d2.completion_tokens = 100;
        d2.total_tokens      = 300;
        d2.cost_usd          = 0.0020;
        tracker.add(d2);

        const UsageDelta s = tracker.session_total();
        CHECK(s.prompt_tokens     == 300);
        CHECK(s.completion_tokens == 150);
        CHECK(s.total_tokens      == 450);
        CHECK(s.cost_usd          == doctest::Approx(0.0030).epsilon(1e-5));
    }

    TEST_CASE("add with zero-cost delta works") {
        UsageTracker tracker;
        UsageDelta d;
        d.prompt_tokens     = 50;
        d.completion_tokens = 25;
        d.total_tokens      = 75;
        d.cost_usd          = 0.0;
        tracker.add(d);

        const UsageDelta s = tracker.session_total();
        CHECK(s.prompt_tokens == 50);
        CHECK(s.cost_usd      == doctest::Approx(0.0));
    }
}

// =============================================================================
// TEST SUITE 4: UsageTracker — session/turn distinction
// =============================================================================
TEST_SUITE("UsageTracker — session/turn distinction") {

    TEST_CASE("reset_turn clears turn but not session") {
        UsageTracker tracker;

        UsageDelta d;
        d.prompt_tokens     = 100;
        d.completion_tokens = 50;
        d.total_tokens      = 150;
        d.cost_usd          = 0.0015;
        tracker.add(d);

        tracker.reset_turn();

        const UsageDelta s = tracker.session_total();
        const UsageDelta t = tracker.turn_total();

        // Session is preserved.
        CHECK(s.prompt_tokens     == 100);
        CHECK(s.completion_tokens == 50);
        CHECK(s.total_tokens      == 150);
        CHECK(s.cost_usd          == doctest::Approx(0.0015).epsilon(1e-5));

        // Turn is zeroed.
        CHECK(t.prompt_tokens     == 0);
        CHECK(t.completion_tokens == 0);
        CHECK(t.total_tokens      == 0);
        CHECK(t.cost_usd          == doctest::Approx(0.0));
    }

    TEST_CASE("turn accumulates independently after reset") {
        UsageTracker tracker;

        // Turn 1.
        UsageDelta d1;
        d1.prompt_tokens     = 100;
        d1.completion_tokens = 50;
        d1.total_tokens      = 150;
        d1.cost_usd          = 0.0010;
        tracker.add(d1);
        tracker.reset_turn();

        // Turn 2.
        UsageDelta d2;
        d2.prompt_tokens     = 200;
        d2.completion_tokens = 80;
        d2.total_tokens      = 280;
        d2.cost_usd          = 0.0020;
        tracker.add(d2);

        const UsageDelta s = tracker.session_total();
        const UsageDelta t = tracker.turn_total();

        // Session: sum of both turns.
        CHECK(s.prompt_tokens     == 300);
        CHECK(s.completion_tokens == 130);
        CHECK(s.total_tokens      == 430);
        CHECK(s.cost_usd          == doctest::Approx(0.0030).epsilon(1e-5));

        // Turn: only turn 2.
        CHECK(t.prompt_tokens     == 200);
        CHECK(t.completion_tokens == 80);
        CHECK(t.total_tokens      == 280);
        CHECK(t.cost_usd          == doctest::Approx(0.0020).epsilon(1e-5));
    }

    TEST_CASE("session_total and turn_total are distinct counters") {
        UsageTracker tracker;

        UsageDelta d;
        d.prompt_tokens = 500; d.completion_tokens = 200; d.total_tokens = 700;
        d.cost_usd = 0.005;
        tracker.add(d);
        tracker.reset_turn();
        tracker.add(d);

        // Session should have 2x the single delta.
        CHECK(tracker.session_total().prompt_tokens == 1000);
        // Turn should have only the last delta.
        CHECK(tracker.turn_total().prompt_tokens    == 500);
    }

    TEST_CASE("reset_all zeroes session and turn") {
        UsageTracker tracker;

        UsageDelta d;
        d.prompt_tokens = 300; d.completion_tokens = 100; d.total_tokens = 400;
        d.cost_usd = 0.003;
        tracker.add(d);
        tracker.add(d);

        tracker.reset_all();

        const UsageDelta s = tracker.session_total();
        const UsageDelta t = tracker.turn_total();

        CHECK(s.prompt_tokens == 0);
        CHECK(s.cost_usd      == doctest::Approx(0.0));
        CHECK(t.prompt_tokens == 0);
        CHECK(t.cost_usd      == doctest::Approx(0.0));
    }
}

// =============================================================================
// TEST SUITE 5: UsageTracker — thread safety
// =============================================================================
TEST_SUITE("UsageTracker — thread safety") {

    TEST_CASE("concurrent adds produce correct session total") {
        UsageTracker tracker;

        constexpr int kThreads = 8;
        constexpr int kAddsPerThread = 1000;

        UsageDelta d;
        d.prompt_tokens     = 1;
        d.completion_tokens = 1;
        d.total_tokens      = 2;
        d.cost_usd          = 0.000001;

        std::vector<std::thread> threads;
        threads.reserve(kThreads);
        for (int i = 0; i < kThreads; ++i) {
            threads.emplace_back([&]() {
                for (int j = 0; j < kAddsPerThread; ++j) {
                    tracker.add(d);
                }
            });
        }
        for (auto& t : threads) { t.join(); }

        const UsageDelta s = tracker.session_total();
        const int expected_tokens = kThreads * kAddsPerThread;
        CHECK(s.prompt_tokens     == expected_tokens);
        CHECK(s.completion_tokens == expected_tokens);
        CHECK(s.total_tokens      == expected_tokens * 2);
        // cost: kThreads * kAddsPerThread * 0.000001
        const double expected_cost = kThreads * kAddsPerThread * 0.000001;
        CHECK(s.cost_usd == doctest::Approx(expected_cost).epsilon(1e-4));
    }
}

// =============================================================================
// TEST SUITE 6: Integration — ModelPricing + UsageTracker together
// =============================================================================
TEST_SUITE("Integration — ModelPricing + UsageTracker") {

    TEST_CASE("compute cost via ModelPricing and accumulate in UsageTracker") {
        UsageTracker tracker;

        UsageDelta d;
        d.prompt_tokens     = 1000;
        d.completion_tokens = 500;
        d.total_tokens      = 1500;
        d.cost_usd          = ModelPricing::cost("gpt-4o", 1000, 500);

        tracker.add(d);

        const UsageDelta s = tracker.session_total();
        CHECK(s.prompt_tokens     == 1000);
        CHECK(s.completion_tokens == 500);
        CHECK(s.total_tokens      == 1500);

        // gpt-4o: (1000 * 2.50 + 500 * 10.00) / 1e6 = 0.0075
        CHECK(s.cost_usd == doctest::Approx(0.0075).epsilon(1e-4));
    }

    TEST_CASE("session accumulates cost across multiple model calls") {
        UsageTracker tracker;
        ModelPricing::reset_for_testing();  // Ensure clean built-in table.

        // Turn 1: gpt-4o
        UsageDelta d1;
        d1.prompt_tokens     = 1000;
        d1.completion_tokens = 500;
        d1.total_tokens      = 1500;
        d1.cost_usd          = ModelPricing::cost("gpt-4o", 1000, 500);
        tracker.add(d1);
        tracker.reset_turn();

        // Turn 2: gpt-4o-mini
        UsageDelta d2;
        d2.prompt_tokens     = 2000;
        d2.completion_tokens = 800;
        d2.total_tokens      = 2800;
        d2.cost_usd          = ModelPricing::cost("gpt-4o-mini", 2000, 800);
        tracker.add(d2);

        const UsageDelta session = tracker.session_total();
        CHECK(session.prompt_tokens     == 3000);
        CHECK(session.completion_tokens == 1300);
        CHECK(session.total_tokens      == 4300);

        const double expected_cost = d1.cost_usd + d2.cost_usd;
        CHECK(session.cost_usd == doctest::Approx(expected_cost).epsilon(1e-4));
    }

    TEST_CASE("unknown model incurs zero cost but tokens still accumulate") {
        UsageTracker tracker;

        UsageDelta d;
        d.prompt_tokens     = 500;
        d.completion_tokens = 200;
        d.total_tokens      = 700;
        d.cost_usd          = ModelPricing::cost("local-llama-3-8b", 500, 200);

        CHECK(d.cost_usd == doctest::Approx(0.0));

        tracker.add(d);
        const UsageDelta s = tracker.session_total();
        CHECK(s.prompt_tokens == 500);
        CHECK(s.cost_usd      == doctest::Approx(0.0));
    }
}
