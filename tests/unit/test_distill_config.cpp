// tests/unit/test_distill_config.cpp
// =============================================================================
// doctest suite for the S1+S4 DistillConfig fields (DIS-980, AC8).
//
// Covers: defaults, BATBOX_DISTILL_* + BATBOX_MAX_TOOL_RESPONSE_SIZE env-var
// loading, and validate() rules (threshold > 0, timeouts > 0, and the
// enabled-requires-endpoint constraints).
//
// Build standalone (no CMake, from repo root; x64-linux triplet). EnvLoader.cpp
// in this branch predates the DIS-969 <optional> fix, so the scoped build links
// the fixed copy from fix/linux-build-breakage-dis969 (NOT committed here):
//   git show fix/linux-build-breakage-dis969:src/config/EnvLoader.cpp > /tmp/EnvLoader_fixed.cpp
//   c++ -std=c++20 -I include -I build/vcpkg_installed/x64-linux/include \
//       tests/unit/test_distill_config.cpp \
//       src/config/Config.cpp /tmp/EnvLoader_fixed.cpp \
//       src/core/Json.cpp src/core/Paths.cpp src/core/Logging.cpp \
//       build/vcpkg_installed/x64-linux/lib/libsimdjson.a \
//       build/vcpkg_installed/x64-linux/lib/libspdlog.a \
//       build/vcpkg_installed/x64-linux/lib/libfmt.a \
//       -o /tmp/test_distill_config && /tmp/test_distill_config
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/config/Config.hpp>
#include <batbox/config/EnvLoader.hpp>

using batbox::config::Config;
using batbox::config::EnvMap;

// =============================================================================
// Defaults [AC8]
// =============================================================================

TEST_CASE("distill defaults are conservative (disabled, 200k threshold) [AC8]") {
    Config cfg = Config::load_default();
    CHECK_FALSE(cfg.distill.enabled);
    CHECK(cfg.distill.base_url.empty());
    CHECK(cfg.distill.model.empty());
    CHECK(cfg.distill.max_tool_response_size == 200'000);
    CHECK(cfg.distill.request_timeout_sec == 60);
    CHECK(cfg.distill.max_tokens == 512);
}

// =============================================================================
// Env-var loading [AC8]
// =============================================================================

TEST_CASE("BATBOX_DISTILL_* + threshold env overlay [AC8]") {
    EnvMap env{
        {"BATBOX_DISTILL_ENABLED", "true"},
        {"BATBOX_DISTILL_BASE_URL", "http://127.0.0.1:11434/v1"},
        {"BATBOX_DISTILL_API_KEY", "ollama"},
        {"BATBOX_DISTILL_MODEL", "qwen2.5:3b"},
        {"BATBOX_MAX_TOOL_RESPONSE_SIZE", "50000"},
        {"BATBOX_DISTILL_TIMEOUT_SEC", "30"},
        {"BATBOX_DISTILL_MAX_TOKENS", "256"},
    };
    auto r = Config::load_from_env(env);
    REQUIRE(r.has_value());
    const Config& c = r.value();
    CHECK(c.distill.enabled);
    CHECK(c.distill.base_url == "http://127.0.0.1:11434/v1");
    CHECK(c.distill.api_key == "ollama");
    CHECK(c.distill.model == "qwen2.5:3b");
    CHECK(c.distill.max_tool_response_size == 50'000);
    CHECK(c.distill.request_timeout_sec == 30);
    CHECK(c.distill.max_tokens == 256);
}

TEST_CASE("BATBOX_MAX_TOOL_RESPONSE_SIZE <= 0 is rejected at load [AC8]") {
    EnvMap env{{"BATBOX_MAX_TOOL_RESPONSE_SIZE", "0"}};
    auto r = Config::load_from_env(env);
    CHECK_FALSE(r.has_value());
}

TEST_CASE("BATBOX_MAX_TOOL_RESPONSE_SIZE non-integer is rejected [AC8]") {
    EnvMap env{{"BATBOX_MAX_TOOL_RESPONSE_SIZE", "huge"}};
    auto r = Config::load_from_env(env);
    CHECK_FALSE(r.has_value());
}

// =============================================================================
// validate() [AC8]
// =============================================================================

TEST_CASE("validate: enabled requires base_url [AC8]") {
    Config cfg = Config::load_default();
    cfg.distill.enabled = true;
    cfg.distill.model   = "m";
    // base_url empty
    CHECK_FALSE(cfg.validate().has_value());
}

TEST_CASE("validate: enabled requires a URL-shaped base_url [AC8]") {
    Config cfg = Config::load_default();
    cfg.distill.enabled  = true;
    cfg.distill.base_url = "127.0.0.1:11434";  // missing scheme
    cfg.distill.model    = "m";
    CHECK_FALSE(cfg.validate().has_value());
}

TEST_CASE("validate: enabled requires model [AC8]") {
    Config cfg = Config::load_default();
    cfg.distill.enabled  = true;
    cfg.distill.base_url = "http://127.0.0.1:11434/v1";
    cfg.distill.model    = "";
    CHECK_FALSE(cfg.validate().has_value());
}

TEST_CASE("validate: a fully-configured enabled distill passes [AC8]") {
    Config cfg = Config::load_default();
    cfg.distill.enabled  = true;
    cfg.distill.base_url = "http://127.0.0.1:11434/v1";
    cfg.distill.model    = "qwen2.5:3b";
    CHECK(cfg.validate().has_value());
}

TEST_CASE("validate: disabled distill needs no endpoint [AC8]") {
    Config cfg = Config::load_default();  // disabled, empty endpoint
    CHECK(cfg.validate().has_value());
}

TEST_CASE("validate: non-positive distill timeout / max_tokens rejected [AC8]") {
    {
        Config cfg = Config::load_default();
        cfg.distill.request_timeout_sec = 0;
        CHECK_FALSE(cfg.validate().has_value());
    }
    {
        Config cfg = Config::load_default();
        cfg.distill.max_tokens = 0;
        CHECK_FALSE(cfg.validate().has_value());
    }
}
