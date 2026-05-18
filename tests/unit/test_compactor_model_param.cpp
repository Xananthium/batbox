// tests/unit/test_compactor_model_param.cpp
// =============================================================================
// Doctest suite for PEXT 5.1 — K-5: Compactor takes model parameter from Config.
//
// Acceptance criteria:
//   1. Compactor constructor accepts a model string parameter.
//   2. model() accessor returns the exact string passed to the constructor.
//   3. The string "gpt-4o" does NOT appear as a hardcoded fallback in Compactor.cpp
//      (verified at source level; the test below is the runtime complement).
//   4. Constructing with cfg.api.default_model ensures BATBOX_DEFAULT_MODEL is
//      honoured — any value other than the old hardcoded "gpt-4o" round-trips.
//
// Build standalone (no CMake, from repo root):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_compactor_model_param.cpp \
//       src/conversation/Compactor.cpp \
//       src/conversation/Message.cpp \
//       src/core/Uuid.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       -o /tmp/test_compactor_model_param && /tmp/test_compactor_model_param
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/conversation/Compactor.hpp>
#include <batbox/config/Config.hpp>

#include <string>

using batbox::conversation::Compactor;
using batbox::config::Config;

// =============================================================================
// Helper: build a minimal Config with a specific default_model
// =============================================================================

namespace {

Config make_config(const char* model) {
    Config cfg        = Config::load_default();
    cfg.api.default_model = model;
    return cfg;
}

} // anonymous namespace

// =============================================================================
// 1. Constructor stores keep_last_n correctly (regression: two-param ctor)
// =============================================================================

TEST_CASE("Compactor: keep_last_n stored correctly with model parameter") {
    Compactor c{5, "some-model"};
    CHECK(c.keep_last_n() == 5);
}

// =============================================================================
// 2. model() accessor returns the exact string passed to the constructor
// =============================================================================

TEST_CASE("Compactor: model() returns the string passed to the constructor") {
    {
        Compactor c{3, "my-custom-model"};
        CHECK(c.model() == "my-custom-model");
    }
    {
        Compactor c{0, "claude-3-5-sonnet-20241022"};
        CHECK(c.model() == "claude-3-5-sonnet-20241022");
    }
    {
        // Empty model string is accepted (degenerate but should not crash).
        Compactor c{1, ""};
        CHECK(c.model() == "");
    }
}

// =============================================================================
// 3. model() is NOT "gpt-4o" when a different string is supplied
//    — runtime complement of the grep-based acceptance criterion
// =============================================================================

TEST_CASE("Compactor: model is NOT hardcoded gpt-4o") {
    Compactor c{4, "magistral-small-2506"};
    CHECK(c.model() != "gpt-4o");
    CHECK(c.model() == "magistral-small-2506");
}

// =============================================================================
// 4. Constructed from Config::api.default_model — value round-trips
// =============================================================================

TEST_CASE("Compactor: constructed from cfg.api.default_model preserves value") {
    const Config cfg = make_config("qwen3-235b-a22b");
    Compactor c{cfg.compact.keep_last_n_turns_verbatim, cfg.api.default_model};
    CHECK(c.model() == cfg.api.default_model);
    CHECK(c.model() == "qwen3-235b-a22b");
}

// =============================================================================
// 5. StatusCallback variant: model still correct when callback is provided
// =============================================================================

TEST_CASE("Compactor: model is correct when optional status callback is set") {
    bool callback_fired = false;
    Compactor c{2, "deepseek-r2",
                [&callback_fired](const std::string&) { callback_fired = true; }};
    CHECK(c.model() == "deepseek-r2");
    // Callback should not have fired at construction time.
    CHECK_FALSE(callback_fired);
}
