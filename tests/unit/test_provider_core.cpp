// tests/unit/test_provider_core.cpp
// ---------------------------------------------------------------------------
// Unit tests for S8 + S9 — Provider abstraction core.
//
// Coverage:
//   1. ProviderRegistry::create returns a non-null OpenAiCompatibleProvider for
//      representative configs (openai, ollama, groq, kimi, deepseek).
//   2. manages_own_context() is false by default and true when the opt-out flag
//      is set (on both OpenAiCompatibleProvider and ProviderRegistry::create).
//   3. metadata()/name() report the resolved provider key + base_url.
//   4. map_to_canonical_model normalises representative ids deterministically
//      and is idempotent: f(f(x)) == f(x).
//   5. should_use_responses_api returns the documented value (always false —
//      batbox speaks only Chat Completions today).
//   6. map_to_canonical_model is wired into ModelPricing::cost as a fallback:
//      prefixed/tagged/mixed-case ids resolve to the priced entry, raw hits
//      and genuine unknowns are unchanged (scope-#4).
//
// Hermetic: NO network calls.  Only construction / metadata / pure-function
// normalisation are exercised — never a live chat()/stream_chat().
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/inference/Provider.hpp>
#include <batbox/inference/ModelPricing.hpp>
#include <batbox/config/Config.hpp>

#include <memory>
#include <string>

namespace {

/// Build a minimal Config for a given hint + base_url.  No network is ever
/// touched: the Config only parameterises provider resolution.
batbox::config::Config make_cfg(std::string hint, std::string base_url) {
    batbox::config::Config cfg;
    cfg.api.provider_hint = std::move(hint);
    cfg.api.base_url      = std::move(base_url);
    cfg.api.api_key       = "test-key";
    return cfg;
}

} // anonymous namespace

// ===========================================================================
// TEST SUITE: ProviderRegistry
// ===========================================================================

TEST_SUITE("ProviderRegistry") {

    TEST_CASE("create returns a non-null OpenAiCompatibleProvider") {
        auto cfg = make_cfg("openai", "https://api.openai.com/v1");
        auto provider = batbox::inference::ProviderRegistry::create(cfg);
        REQUIRE(provider != nullptr);
        // The concrete type is OpenAiCompatibleProvider.
        auto* concrete =
            dynamic_cast<batbox::inference::OpenAiCompatibleProvider*>(provider.get());
        CHECK(concrete != nullptr);
    }

    TEST_CASE("create resolves the provider identity for representative configs") {
        struct Case { std::string hint; std::string url; std::string expect; };
        const Case cases[] = {
            {"openai",   "https://api.openai.com/v1",     "openai"},
            {"",         "http://localhost:11434/v1",     "ollama"},   // auto-detect
            {"auto",     "https://api.groq.com/openai/v1","groq"},     // auto-detect
            {"kimi",     "https://api.moonshot.cn/v1",    "kimi"},     // verbatim identity
            {"deepseek", "https://api.deepseek.com/v1",   "deepseek"}, // verbatim identity
        };
        for (const auto& c : cases) {
            auto cfg      = make_cfg(c.hint, c.url);
            auto provider = batbox::inference::ProviderRegistry::create(cfg);
            REQUIRE(provider != nullptr);
            CHECK(provider->name() == c.expect);
            CHECK(provider->metadata().name == c.expect);
            CHECK(provider->metadata().base_url == c.url);
        }
    }

} // TEST_SUITE ProviderRegistry

// ===========================================================================
// TEST SUITE: manages_own_context (S9)
// ===========================================================================

TEST_SUITE("manages_own_context") {

    TEST_CASE("default is false on OpenAiCompatibleProvider") {
        auto cfg = make_cfg("openai", "https://api.openai.com/v1");
        batbox::inference::OpenAiCompatibleProvider provider{cfg};
        CHECK(provider.manages_own_context() == false);
    }

    TEST_CASE("default is false via ProviderRegistry::create") {
        auto cfg      = make_cfg("openai", "https://api.openai.com/v1");
        auto provider = batbox::inference::ProviderRegistry::create(cfg);
        REQUIRE(provider != nullptr);
        CHECK(provider->manages_own_context() == false);
    }

    TEST_CASE("opt-out flag flips it true on OpenAiCompatibleProvider") {
        auto cfg = make_cfg("openai", "https://api.openai.com/v1");
        batbox::inference::OpenAiCompatibleProvider provider{cfg, /*manages_own_context=*/true};
        CHECK(provider.manages_own_context() == true);
    }

    TEST_CASE("opt-out flag flips it true via ProviderRegistry::create") {
        auto cfg      = make_cfg("openai", "https://api.openai.com/v1");
        auto provider = batbox::inference::ProviderRegistry::create(cfg, /*manages_own_context=*/true);
        REQUIRE(provider != nullptr);
        CHECK(provider->manages_own_context() == true);
    }

    TEST_CASE("the hook is reachable through the Provider base interface") {
        auto cfg = make_cfg("openai", "https://api.openai.com/v1");
        std::unique_ptr<batbox::inference::Provider> base =
            std::make_unique<batbox::inference::OpenAiCompatibleProvider>(cfg, true);
        CHECK(base->manages_own_context() == true);
    }

} // TEST_SUITE manages_own_context

// ===========================================================================
// TEST SUITE: map_to_canonical_model
// ===========================================================================

TEST_SUITE("map_to_canonical_model") {

    using batbox::inference::map_to_canonical_model;

    TEST_CASE("strips a single leading vendor prefix") {
        CHECK(map_to_canonical_model("openai/gpt-4o")            == "gpt-4o");
        CHECK(map_to_canonical_model("mistralai/magistral-small")== "magistral-small");
        CHECK(map_to_canonical_model("together/llama-3")         == "llama-3");
        // Multi-segment paths collapse to the final segment (keeps idempotence).
        CHECK(map_to_canonical_model("a/b/c")                    == "c");
    }

    TEST_CASE("strips a trailing :tag deployment suffix") {
        CHECK(map_to_canonical_model("llama3.2:3b-cloud") == "llama3.2");
        CHECK(map_to_canonical_model("qwen2.5:latest")    == "qwen2.5");
    }

    TEST_CASE("lowercases and trims whitespace") {
        CHECK(map_to_canonical_model("  GPT-4O  ") == "gpt-4o");
        CHECK(map_to_canonical_model("Claude-3-5-Sonnet") == "claude-3-5-sonnet");
    }

    TEST_CASE("prefix + suffix combined") {
        CHECK(map_to_canonical_model("ollama/llama3.2:latest") == "llama3.2");
    }

    TEST_CASE("plain ids pass through (lowercased)") {
        CHECK(map_to_canonical_model("gpt-4o")     == "gpt-4o");
        CHECK(map_to_canonical_model("gpt-4o-mini")== "gpt-4o-mini");
    }

    TEST_CASE("empty / whitespace-only input maps to empty") {
        CHECK(map_to_canonical_model("").empty());
        CHECK(map_to_canonical_model("   ").empty());
    }

    TEST_CASE("is idempotent: f(f(x)) == f(x)") {
        const char* inputs[] = {
            "openai/gpt-4o", "mistralai/magistral-small", "llama3.2:3b-cloud",
            "ollama/llama3.2:latest", "  GPT-4O  ", "a/b/c", "gpt-4o-mini",
            "qwen2.5:latest", "Claude-3-5-Sonnet", "", "   ",
        };
        for (const char* in : inputs) {
            const std::string once  = map_to_canonical_model(in);
            const std::string twice = map_to_canonical_model(once);
            CHECK(twice == once);
        }
    }

} // TEST_SUITE map_to_canonical_model

// ===========================================================================
// TEST SUITE: should_use_responses_api
// ===========================================================================

TEST_SUITE("should_use_responses_api") {

    using batbox::inference::should_use_responses_api;

    TEST_CASE("returns false for every current endpoint (Chat Completions only)") {
        CHECK(should_use_responses_api("openai",   "gpt-4o")        == false);
        CHECK(should_use_responses_api("openai",   "o1-preview")    == false);
        CHECK(should_use_responses_api("ollama",   "llama3.2")      == false);
        CHECK(should_use_responses_api("groq",     "mixtral-8x7b")  == false);
        CHECK(should_use_responses_api("deepseek", "deepseek-chat") == false);
        CHECK(should_use_responses_api("",         "")              == false);
    }

} // TEST_SUITE should_use_responses_api

// ===========================================================================
// TEST SUITE: canonical-name wiring into ModelPricing (scope-#4)
// ---------------------------------------------------------------------------
// map_to_canonical_model is wired into ModelPricing::cost as a fallback: the
// raw model id is looked up first (existing behaviour, no regression), and on
// a miss the canonicalised id is tried.  These cases prove the seam is
// load-bearing — provider-prefixed / tag-suffixed / mixed-case ids now resolve
// to the same priced entry, while raw hits and genuine unknowns are unchanged.
// ===========================================================================

TEST_SUITE("ModelPricing canonical wiring") {

    using batbox::inference::ModelPricing;

    // gpt-4o: 2.50 / 10.00 per million → 1000 prompt + 500 completion
    //   = (1000*2.50 + 500*10.00)/1e6 = (2500 + 5000)/1e6 = 0.0075
    static constexpr double kGpt4oCost = 0.0075;

    TEST_CASE("raw id still resolves unchanged (AC6 no-regression)") {
        ModelPricing::reset_for_testing();
        CHECK(ModelPricing::cost("gpt-4o", 1000, 500) == doctest::Approx(kGpt4oCost));
    }

    TEST_CASE("provider-prefixed id resolves via canonical fallback") {
        ModelPricing::reset_for_testing();
        CHECK(ModelPricing::cost("openai/gpt-4o", 1000, 500)
              == doctest::Approx(kGpt4oCost));
    }

    TEST_CASE("tag-suffixed + mixed-case ids resolve via canonical fallback") {
        ModelPricing::reset_for_testing();
        CHECK(ModelPricing::cost("gpt-4o:latest", 1000, 500)
              == doctest::Approx(kGpt4oCost));
        CHECK(ModelPricing::cost("OpenAI/GPT-4o", 1000, 500)
              == doctest::Approx(kGpt4oCost));
    }

    TEST_CASE("genuinely unknown model still returns 0.0") {
        ModelPricing::reset_for_testing();
        CHECK(ModelPricing::cost("no-such-model-xyz", 1000, 500) == 0.0);
        // Canonicalises to a still-unknown id — must not spuriously price.
        CHECK(ModelPricing::cost("acme/no-such-model-xyz:v2", 1000, 500) == 0.0);
    }

} // TEST_SUITE ModelPricing canonical wiring
