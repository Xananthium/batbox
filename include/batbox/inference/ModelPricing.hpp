// include/batbox/inference/ModelPricing.hpp
// =============================================================================
// batbox::inference::ModelPricing — static model pricing table.
//
// The pricing table is loaded from the embedded data/models.json (compiled into
// the binary as a constexpr string) and parsed once at startup via a
// function-local static.  The user may override per-model prices by setting:
//
//   BATBOX_MODEL_PRICING_OVERRIDE=/path/to/custom.json
//
// The override file uses the same shape as models.json.  Entries in the
// override file replace matching model IDs in the built-in table; models not
// present in the override are left at their built-in prices.
//
// Usage:
//   double usd = ModelPricing::cost("gpt-4o", 1000, 500);
//
// Thread-safety:
//   The table is initialised once (C++11 magic static guarantee) and is
//   read-only thereafter.  Concurrent calls to cost() are safe.
//
// Build standalone (from repo root):
//   c++ -std=c++20 -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_usage_tracker.cpp \
//       src/inference/ModelPricing.cpp src/inference/UsageTracker.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       -o /tmp/test_usage && /tmp/test_usage
// =============================================================================

#pragma once

#include <string_view>

namespace batbox::inference {

// =============================================================================
// ModelPricing — lookup cost for a model given token counts
// =============================================================================

/// Provides per-model USD cost computation based on a static pricing table.
///
/// The table is loaded from the embedded models.json at first call.  An
/// optional user override path (BATBOX_MODEL_PRICING_OVERRIDE env var) merges
/// on top, replacing entries for matched model IDs.
///
/// Models not found in the table return 0.0 (unknown / zero-cost models such
/// as local inference servers or un-priced Anthropic placeholders).
class ModelPricing {
public:
    /// Compute cost in USD for one API call.
    ///
    /// @param model            Model identifier (e.g. "gpt-4o").
    /// @param prompt_tokens    Number of prompt / input tokens.
    /// @param completion_tokens Number of completion / output tokens.
    /// @returns                Estimated cost in USD, rounded to 4 decimal places.
    ///                         Returns 0.0 for unknown models.
    [[nodiscard]] static double cost(std::string_view model,
                                     int prompt_tokens,
                                     int completion_tokens);

    /// Force re-initialisation of the pricing table (for testing only).
    /// Not thread-safe; call only from single-threaded test setup.
    static void reset_for_testing();
};

} // namespace batbox::inference
