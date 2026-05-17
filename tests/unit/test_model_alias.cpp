// tests/unit/test_model_alias.cpp
// ---------------------------------------------------------------------------
// doctest suite for batbox::config::resolve_model_alias.
//
// Coverage:
//   1. "opus"   → cfg.api.opus_model
//   2. "sonnet" → cfg.api.sonnet_model
//   3. "haiku"  → cfg.api.haiku_model
//   4. Unknown alias string → returned verbatim
//   5. Empty string → returned verbatim (empty string pass-through)
//   6. Alias with empty target field falls back to cfg.api.default_model
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/config/Config.hpp>

using namespace batbox::config;
using namespace batbox;

// ---------------------------------------------------------------------------
// Helpers to build a Config with specific alias fields set.
// ---------------------------------------------------------------------------
static Config make_cfg(
        std::string default_model,
        std::string opus_model   = {},
        std::string sonnet_model = {},
        std::string haiku_model  = {}) {
    Config cfg = Config::load_default();
    cfg.api.default_model = std::move(default_model);
    cfg.api.opus_model    = std::move(opus_model);
    cfg.api.sonnet_model  = std::move(sonnet_model);
    cfg.api.haiku_model   = std::move(haiku_model);
    return cfg;
}

// ============================================================================
// SUITE — resolve_model_alias
// ============================================================================
TEST_SUITE("resolve_model_alias — alias dispatch") {

    TEST_CASE("'opus' resolves to cfg.api.opus_model") {
        const auto cfg = make_cfg("base", "big-model", "mid-model", "small-model");
        CHECK(resolve_model_alias("opus", cfg) == "big-model");
    }

    TEST_CASE("'sonnet' resolves to cfg.api.sonnet_model") {
        const auto cfg = make_cfg("base", "big-model", "mid-model", "small-model");
        CHECK(resolve_model_alias("sonnet", cfg) == "mid-model");
    }

    TEST_CASE("'haiku' resolves to cfg.api.haiku_model") {
        const auto cfg = make_cfg("base", "big-model", "mid-model", "small-model");
        CHECK(resolve_model_alias("haiku", cfg) == "small-model");
    }

    TEST_CASE("fully-qualified model name is returned verbatim") {
        const auto cfg = make_cfg("base", "big-model", "mid-model", "small-model");
        CHECK(resolve_model_alias("gpt-oss:120b-cloud", cfg) == "gpt-oss:120b-cloud");
    }

    TEST_CASE("empty string is returned verbatim (empty pass-through)") {
        const auto cfg = make_cfg("base", "big-model", "mid-model", "small-model");
        CHECK(resolve_model_alias("", cfg) == "");
    }

    TEST_CASE("unrecognised alias returns input unchanged") {
        const auto cfg = make_cfg("base");
        CHECK(resolve_model_alias("mistral-7b", cfg) == "mistral-7b");
        CHECK(resolve_model_alias("llama3.1:70b", cfg) == "llama3.1:70b");
        CHECK(resolve_model_alias("claude-opus-4-5", cfg) == "claude-opus-4-5");
    }

    TEST_CASE("'opus' with empty opus_model falls back to default_model") {
        // Empty alias field → fall back to default_model
        const auto cfg = make_cfg("fallback-model", /*opus=*/"", "mid", "small");
        CHECK(resolve_model_alias("opus", cfg) == "fallback-model");
    }

    TEST_CASE("'sonnet' with empty sonnet_model falls back to default_model") {
        const auto cfg = make_cfg("fallback-model", "big", /*sonnet=*/"", "small");
        CHECK(resolve_model_alias("sonnet", cfg) == "fallback-model");
    }

    TEST_CASE("'haiku' with empty haiku_model falls back to default_model") {
        const auto cfg = make_cfg("fallback-model", "big", "mid", /*haiku=*/"");
        CHECK(resolve_model_alias("haiku", cfg) == "fallback-model");
    }

    TEST_CASE("alias resolution via load_from_env round-trip") {
        // Verify the full config-loading path produces correct values for resolve_model_alias.
        EnvMap env{
            {"BATBOX_DEFAULT_MODEL", "kimi-k2.6:cloud"},
            {"BATBOX_OPUS_MODEL",    "kimi-k2.6:cloud"},
            {"BATBOX_SONNET_MODEL",  "kimi-k2.6:cloud"},
            {"BATBOX_HAIKU_MODEL",   "kimi-k2.6:cloud"},
        };
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(resolve_model_alias("opus",   *r) == "kimi-k2.6:cloud");
        CHECK(resolve_model_alias("sonnet", *r) == "kimi-k2.6:cloud");
        CHECK(resolve_model_alias("haiku",  *r) == "kimi-k2.6:cloud");
    }

    TEST_CASE("alias names are case-sensitive — 'Opus' is NOT an alias") {
        const auto cfg = make_cfg("base", "big-model", "mid-model", "small-model");
        // "Opus" (capital O) should pass through verbatim, not resolve to opus_model.
        CHECK(resolve_model_alias("Opus", cfg) == "Opus");
        CHECK(resolve_model_alias("OPUS", cfg) == "OPUS");
        CHECK(resolve_model_alias("Sonnet", cfg) == "Sonnet");
        CHECK(resolve_model_alias("Haiku", cfg) == "Haiku");
    }
}
