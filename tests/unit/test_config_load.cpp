// tests/unit/test_config_load.cpp
// ---------------------------------------------------------------------------
// doctest suite for batbox::config::Config.
//
// Coverage:
//   1. Default values sane (all ~40 fields at least implicitly via load_default)
//   2. load_from_env() overrides beat defaults for every field
//   3. load() — settings.json overrides beat defaults but lose to env
//   4. validation rejects malformed values with clear error strings
//   5. redacted_for_display() masks api_key
//   6. to_json() / round-trip enum helpers
//   7. Boolean parsing: true/false/1/0/yes/no (case-insensitive)
//   8. Enum fields reject unknown values with field name in error
//   9. version_seq copy semantics
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/config/Config.hpp>
#include <batbox/core/Json.hpp>

using namespace batbox::config;
using namespace batbox;

// ============================================================================
// SUITE 1 — Default values
// ============================================================================
TEST_SUITE("Config::load_default — field sanity") {

    TEST_CASE("API defaults are sane") {
        const auto cfg = Config::load_default();
        CHECK(cfg.api.base_url     == "https://api.openai.com/v1");
        CHECK(cfg.api.api_key.empty());
        CHECK(cfg.api.default_model == "gpt-4o");
        CHECK_FALSE(cfg.api.models.empty());
        CHECK(cfg.api.max_tokens        == 4096);
        CHECK(cfg.api.temperature       == doctest::Approx(0.7));
        CHECK(cfg.api.top_p             == doctest::Approx(1.0));
        CHECK(cfg.api.request_timeout_sec == 120);
        CHECK(cfg.api.stream);
    }

    TEST_CASE("general defaults are sane") {
        const auto cfg = Config::load_default();
        CHECK_FALSE(cfg.general.config_dir.empty());
        CHECK(cfg.general.project_memory_file == "BATBOX.md");
        CHECK(cfg.general.log_level == LogLevel::Info);
        CHECK_FALSE(cfg.general.log_file.empty());
    }

    TEST_CASE("UI defaults are sane") {
        const auto cfg = Config::load_default();
        CHECK(cfg.ui.theme      == Theme::MissKittin);
        CHECK_FALSE(cfg.ui.no_splash);
        CHECK_FALSE(cfg.ui.vim_mode);
        CHECK(cfg.ui.statusline == StatusLine::Default);
    }

    TEST_CASE("search defaults are sane") {
        const auto cfg = Config::load_default();
        CHECK(cfg.search.engine              == SearchEngine::Ddg);
        CHECK(cfg.search.searxng_url.empty());
        CHECK(cfg.search.respect_robots);
        CHECK(cfg.search.webfetch_max_bytes   == 5'242'880);
        CHECK(cfg.search.webfetch_timeout_sec == 30);
    }

    TEST_CASE("sidecar defaults are sane") {
        const auto cfg = Config::load_default();
        CHECK(cfg.sidecar.python.string().find("python") != std::string::npos);
        CHECK_FALSE(cfg.sidecar.venv.empty());
        CHECK(cfg.sidecar.startup_timeout_sec == 15);
        CHECK(cfg.sidecar.autostart);
        CHECK_FALSE(cfg.sidecar.prewarm);
    }

    TEST_CASE("tools defaults are sane") {
        const auto cfg = Config::load_default();
        CHECK(cfg.tools.bash_timeout_sec     == 120);
        CHECK(cfg.tools.bash_max_output_bytes == 1'048'576);
        CHECK(cfg.tools.task_parallel_limit  == 4);
    }

    TEST_CASE("MCP defaults are sane") {
        const auto cfg = Config::load_default();
        CHECK_FALSE(cfg.mcp.config_path.empty());
        CHECK(cfg.mcp.startup_timeout_sec == 10);
    }

    TEST_CASE("agents defaults are sane") {
        const auto cfg = Config::load_default();
        CHECK_FALSE(cfg.agents.agents_config.empty());
        CHECK_FALSE(cfg.agents.agents_dir.empty());
        CHECK_FALSE(cfg.agents.demon_enabled);
    }

    TEST_CASE("security defaults are sane") {
        const auto cfg = Config::load_default();
        CHECK(cfg.security.permission_mode   == PermissionMode::Default);
        CHECK(cfg.security.auto_approve_reads);
    }

    TEST_CASE("compact defaults are sane") {
        const auto cfg = Config::load_default();
        CHECK(cfg.compact.auto_compact_at_pct        == 80);
        CHECK(cfg.compact.keep_last_n_turns_verbatim == 10);
    }
}

// ============================================================================
// SUITE 2 — load_from_env overrides (env beats defaults)
// ============================================================================
TEST_SUITE("Config::load_from_env — env overrides defaults") {

    TEST_CASE("BATBOX_API_BASE_URL override") {
        EnvMap env{{"BATBOX_API_BASE_URL", "http://localhost:11434/v1"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.base_url == "http://localhost:11434/v1");
    }

    TEST_CASE("BATBOX_API_KEY override") {
        EnvMap env{{"BATBOX_API_KEY", "sk-test-abc123"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.api_key == "sk-test-abc123");
    }

    TEST_CASE("BATBOX_DEFAULT_MODEL override") {
        EnvMap env{{"BATBOX_DEFAULT_MODEL", "llama3.1"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.default_model == "llama3.1");
    }

    TEST_CASE("BATBOX_MODELS comma-separated override") {
        EnvMap env{{"BATBOX_MODELS", "gpt-4o, gpt-4o-mini, o1-preview"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        REQUIRE(r->api.models.size() == 3);
        CHECK(r->api.models[0] == "gpt-4o");
        CHECK(r->api.models[1] == "gpt-4o-mini");
        CHECK(r->api.models[2] == "o1-preview");
    }

    TEST_CASE("BATBOX_MAX_TOKENS override") {
        EnvMap env{{"BATBOX_MAX_TOKENS", "8192"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.max_tokens == 8192);
    }

    TEST_CASE("BATBOX_TEMPERATURE override") {
        EnvMap env{{"BATBOX_TEMPERATURE", "0.3"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.temperature == doctest::Approx(0.3));
    }

    TEST_CASE("BATBOX_TOP_P override") {
        EnvMap env{{"BATBOX_TOP_P", "0.95"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.top_p == doctest::Approx(0.95));
    }

    TEST_CASE("BATBOX_REQUEST_TIMEOUT_SEC override") {
        EnvMap env{{"BATBOX_REQUEST_TIMEOUT_SEC", "60"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.request_timeout_sec == 60);
    }

    TEST_CASE("BATBOX_STREAM false override") {
        EnvMap env{{"BATBOX_STREAM", "false"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK_FALSE(r->api.stream);
    }

    TEST_CASE("BATBOX_LOG_LEVEL override to debug") {
        EnvMap env{{"BATBOX_LOG_LEVEL", "debug"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->general.log_level == LogLevel::Debug);
    }

    TEST_CASE("BATBOX_LOG_LEVEL override to trace (case-insensitive)") {
        EnvMap env{{"BATBOX_LOG_LEVEL", "TRACE"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->general.log_level == LogLevel::Trace);
    }

    TEST_CASE("BATBOX_THEME override to stock-exchange") {
        EnvMap env{{"BATBOX_THEME", "stock-exchange"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->ui.theme == Theme::StockExchange);
    }

    TEST_CASE("BATBOX_THEME override to frank-sinatra") {
        EnvMap env{{"BATBOX_THEME", "frank-sinatra"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->ui.theme == Theme::FrankSinatra);
    }

    TEST_CASE("BATBOX_THEME override to monochrome") {
        EnvMap env{{"BATBOX_THEME", "monochrome"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->ui.theme == Theme::Monochrome);
    }

    TEST_CASE("BATBOX_THEME override to classic") {
        EnvMap env{{"BATBOX_THEME", "classic"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->ui.theme == Theme::Classic);
    }

    TEST_CASE("BATBOX_NO_SPLASH override") {
        EnvMap env{{"BATBOX_NO_SPLASH", "true"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->ui.no_splash);
    }

    TEST_CASE("BATBOX_VIM_MODE override") {
        EnvMap env{{"BATBOX_VIM_MODE", "1"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->ui.vim_mode);
    }

    TEST_CASE("BATBOX_STATUSLINE override to minimal") {
        EnvMap env{{"BATBOX_STATUSLINE", "minimal"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->ui.statusline == StatusLine::Minimal);
    }

    TEST_CASE("BATBOX_SEARCH_ENGINE override to searxng") {
        EnvMap env{
            {"BATBOX_SEARCH_ENGINE", "searxng"},
            {"BATBOX_SEARXNG_URL",   "https://searxng.example.org"},
        };
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->search.engine == SearchEngine::Searxng);
        CHECK(r->search.searxng_url == "https://searxng.example.org");
    }

    TEST_CASE("BATBOX_RESPECT_ROBOTS false override") {
        EnvMap env{{"BATBOX_RESPECT_ROBOTS", "no"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK_FALSE(r->search.respect_robots);
    }

    TEST_CASE("BATBOX_WEBFETCH_MAX_BYTES override") {
        EnvMap env{{"BATBOX_WEBFETCH_MAX_BYTES", "1048576"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->search.webfetch_max_bytes == 1'048'576);
    }

    TEST_CASE("BATBOX_WEBFETCH_TIMEOUT_SEC override") {
        EnvMap env{{"BATBOX_WEBFETCH_TIMEOUT_SEC", "60"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->search.webfetch_timeout_sec == 60);
    }

    TEST_CASE("BATBOX_SIDECAR_STARTUP_TIMEOUT_SEC override") {
        EnvMap env{{"BATBOX_SIDECAR_STARTUP_TIMEOUT_SEC", "30"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->sidecar.startup_timeout_sec == 30);
    }

    TEST_CASE("BATBOX_SIDECAR_AUTOSTART false override") {
        EnvMap env{{"BATBOX_SIDECAR_AUTOSTART", "false"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK_FALSE(r->sidecar.autostart);
    }

    TEST_CASE("BATBOX_SIDECAR_PREWARM true override") {
        EnvMap env{{"BATBOX_SIDECAR_PREWARM", "true"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->sidecar.prewarm);
    }

    TEST_CASE("BATBOX_BASH_TIMEOUT_SEC override") {
        EnvMap env{{"BATBOX_BASH_TIMEOUT_SEC", "60"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->tools.bash_timeout_sec == 60);
    }

    TEST_CASE("BATBOX_BASH_MAX_OUTPUT_BYTES override") {
        EnvMap env{{"BATBOX_BASH_MAX_OUTPUT_BYTES", "524288"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->tools.bash_max_output_bytes == 524288);
    }

    TEST_CASE("BATBOX_TASK_PARALLEL_LIMIT override") {
        EnvMap env{{"BATBOX_TASK_PARALLEL_LIMIT", "8"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->tools.task_parallel_limit == 8);
    }

    TEST_CASE("BATBOX_MCP_STARTUP_TIMEOUT_SEC override") {
        EnvMap env{{"BATBOX_MCP_STARTUP_TIMEOUT_SEC", "20"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->mcp.startup_timeout_sec == 20);
    }

    TEST_CASE("BATBOX_DEMON_ENABLED override") {
        EnvMap env{{"BATBOX_DEMON_ENABLED", "true"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->agents.demon_enabled);
    }

    TEST_CASE("BATBOX_PERMISSION_MODE override to plan") {
        EnvMap env{{"BATBOX_PERMISSION_MODE", "plan"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->security.permission_mode == PermissionMode::Plan);
    }

    TEST_CASE("BATBOX_PERMISSION_MODE override to acceptEdits (case-insensitive)") {
        EnvMap env{{"BATBOX_PERMISSION_MODE", "acceptedits"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->security.permission_mode == PermissionMode::AcceptEdits);
    }

    TEST_CASE("BATBOX_PERMISSION_MODE override to nuclear") {
        EnvMap env{{"BATBOX_PERMISSION_MODE", "nuclear"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->security.permission_mode == PermissionMode::Nuclear);
    }

    TEST_CASE("BATBOX_AUTO_APPROVE_READS false override") {
        EnvMap env{{"BATBOX_AUTO_APPROVE_READS", "false"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK_FALSE(r->security.auto_approve_reads);
    }

    TEST_CASE("BATBOX_AUTO_COMPACT_AT_PCT override") {
        EnvMap env{{"BATBOX_AUTO_COMPACT_AT_PCT", "90"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->compact.auto_compact_at_pct == 90);
    }

    TEST_CASE("BATBOX_KEEP_LAST_N_TURNS_VERBATIM override") {
        EnvMap env{{"BATBOX_KEEP_LAST_N_TURNS_VERBATIM", "5"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->compact.keep_last_n_turns_verbatim == 5);
    }
}

// ============================================================================
// SUITE 3 — load() precedence: env > settings_json > defaults
// ============================================================================
TEST_SUITE("Config::load — precedence rules") {

    TEST_CASE("settings_json overrides defaults") {
        Json settings;
        settings["api"]["max_tokens"] = 2048;
        settings["ui"]["theme"] = "classic";

        EnvMap env;  // no env override
        auto r = Config::load(env, settings);
        REQUIRE(r.has_value());
        CHECK(r->api.max_tokens == 2048);
        CHECK(r->ui.theme == Theme::Classic);
        // untouched default
        CHECK(r->api.base_url == "https://api.openai.com/v1");
    }

    TEST_CASE("env overrides settings_json (env wins)") {
        // settings_json says max_tokens=2048; env says 1024 — env must win.
        Json settings;
        settings["api"]["max_tokens"] = 2048;
        settings["api"]["default_model"] = "gpt-3.5-turbo";

        EnvMap env{
            {"BATBOX_MAX_TOKENS",    "1024"},
            {"BATBOX_DEFAULT_MODEL", "o1-preview"},
        };
        auto r = Config::load(env, settings);
        REQUIRE(r.has_value());
        CHECK(r->api.max_tokens    == 1024);          // env wins
        CHECK(r->api.default_model == "o1-preview");  // env wins
    }

    TEST_CASE("settings_json does not clobber env-set fields") {
        // env sets permission_mode=nuclear; settings says default — env wins.
        Json settings;
        settings["security"]["permission_mode"] = "default";

        EnvMap env{{"BATBOX_PERMISSION_MODE", "nuclear"}};
        auto r = Config::load(env, settings);
        REQUIRE(r.has_value());
        CHECK(r->security.permission_mode == PermissionMode::Nuclear);
    }

    TEST_CASE("settings_json beats defaults but not env — three-way check") {
        // default theme = miss-kittin
        // settings says monochrome
        // env says stock-exchange
        // expected: stock-exchange (env wins)
        Json settings;
        settings["ui"]["theme"] = "monochrome";

        EnvMap env{{"BATBOX_THEME", "stock-exchange"}};
        auto r = Config::load(env, settings);
        REQUIRE(r.has_value());
        CHECK(r->ui.theme == Theme::StockExchange);
    }

    TEST_CASE("settings_json only beats defaults when env is absent") {
        // no env for theme → settings should win
        Json settings;
        settings["ui"]["theme"] = "monochrome";

        EnvMap env;
        auto r = Config::load(env, settings);
        REQUIRE(r.has_value());
        CHECK(r->ui.theme == Theme::Monochrome);
    }

    TEST_CASE("null / empty settings_json is silently skipped") {
        Json empty_j;  // default-constructed → JSON null / object
        EnvMap env{{"BATBOX_MAX_TOKENS", "512"}};
        auto r = Config::load(env, empty_j);
        REQUIRE(r.has_value());
        CHECK(r->api.max_tokens == 512);
    }
}

// ============================================================================
// SUITE 4 — Validation errors
// ============================================================================
TEST_SUITE("Config::validate — malformed values") {

    TEST_CASE("invalid BATBOX_API_BASE_URL (not a URL) returns error with field name") {
        EnvMap env{{"BATBOX_API_BASE_URL", "ftp://not-http.example.com"}};
        auto r = Config::load_from_env(env);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().find("BATBOX_API_BASE_URL") != std::string::npos);
    }

    TEST_CASE("BATBOX_MAX_TOKENS = 0 fails validation") {
        EnvMap env{{"BATBOX_MAX_TOKENS", "0"}};
        auto r = Config::load_from_env(env);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().find("BATBOX_MAX_TOKENS") != std::string::npos);
    }

    TEST_CASE("BATBOX_MAX_TOKENS = negative fails validation") {
        EnvMap env{{"BATBOX_MAX_TOKENS", "-1"}};
        auto r = Config::load_from_env(env);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().find("BATBOX_MAX_TOKENS") != std::string::npos);
    }

    TEST_CASE("BATBOX_MAX_TOKENS = junk string returns error") {
        EnvMap env{{"BATBOX_MAX_TOKENS", "notanumber"}};
        auto r = Config::load_from_env(env);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().find("BATBOX_MAX_TOKENS") != std::string::npos);
    }

    TEST_CASE("BATBOX_TEMPERATURE out of range (> 2.0)") {
        EnvMap env{
            {"BATBOX_API_BASE_URL", "https://api.openai.com/v1"},
            {"BATBOX_TEMPERATURE",  "3.5"},
        };
        auto r = Config::load_from_env(env);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().find("BATBOX_TEMPERATURE") != std::string::npos);
    }

    TEST_CASE("BATBOX_TEMPERATURE negative fails") {
        EnvMap env{{"BATBOX_TEMPERATURE", "-0.1"}};
        auto r = Config::load_from_env(env);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().find("BATBOX_TEMPERATURE") != std::string::npos);
    }

    TEST_CASE("BATBOX_TOP_P = 0 fails validation: zero is outside the valid half-open interval") {
        EnvMap env{{"BATBOX_TOP_P", "0"}};
        auto r = Config::load_from_env(env);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().find("BATBOX_TOP_P") != std::string::npos);
    }

    TEST_CASE("BATBOX_TOP_P > 1 fails") {
        EnvMap env{{"BATBOX_TOP_P", "1.5"}};
        auto r = Config::load_from_env(env);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().find("BATBOX_TOP_P") != std::string::npos);
    }

    TEST_CASE("BATBOX_TASK_PARALLEL_LIMIT = 0 fails") {
        EnvMap env{{"BATBOX_TASK_PARALLEL_LIMIT", "0"}};
        auto r = Config::load_from_env(env);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().find("BATBOX_TASK_PARALLEL_LIMIT") != std::string::npos);
    }

    TEST_CASE("BATBOX_TASK_PARALLEL_LIMIT > 64 fails") {
        EnvMap env{{"BATBOX_TASK_PARALLEL_LIMIT", "65"}};
        auto r = Config::load_from_env(env);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().find("BATBOX_TASK_PARALLEL_LIMIT") != std::string::npos);
    }

    TEST_CASE("BATBOX_AUTO_COMPACT_AT_PCT = 0 fails") {
        EnvMap env{{"BATBOX_AUTO_COMPACT_AT_PCT", "0"}};
        auto r = Config::load_from_env(env);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().find("BATBOX_AUTO_COMPACT_AT_PCT") != std::string::npos);
    }

    TEST_CASE("BATBOX_AUTO_COMPACT_AT_PCT = 101 fails") {
        EnvMap env{{"BATBOX_AUTO_COMPACT_AT_PCT", "101"}};
        auto r = Config::load_from_env(env);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().find("BATBOX_AUTO_COMPACT_AT_PCT") != std::string::npos);
    }

    TEST_CASE("BATBOX_LOG_LEVEL unknown value returns error") {
        EnvMap env{{"BATBOX_LOG_LEVEL", "verbose"}};
        auto r = Config::load_from_env(env);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().find("BATBOX_LOG_LEVEL") != std::string::npos);
    }

    TEST_CASE("BATBOX_THEME unknown value returns error") {
        EnvMap env{{"BATBOX_THEME", "neon-jungle"}};
        auto r = Config::load_from_env(env);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().find("BATBOX_THEME") != std::string::npos);
    }

    TEST_CASE("BATBOX_SEARCH_ENGINE unknown value returns error") {
        EnvMap env{{"BATBOX_SEARCH_ENGINE", "bing"}};
        auto r = Config::load_from_env(env);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().find("BATBOX_SEARCH_ENGINE") != std::string::npos);
    }

    TEST_CASE("BATBOX_PERMISSION_MODE unknown value returns error") {
        EnvMap env{{"BATBOX_PERMISSION_MODE", "yolo"}};
        auto r = Config::load_from_env(env);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().find("BATBOX_PERMISSION_MODE") != std::string::npos);
    }

    TEST_CASE("BATBOX_STREAM invalid boolean returns error") {
        EnvMap env{{"BATBOX_STREAM", "maybe"}};
        auto r = Config::load_from_env(env);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().find("BATBOX_STREAM") != std::string::npos);
    }

    TEST_CASE("BATBOX_SEARXNG_URL malformed (not http) when engine=searxng fails") {
        EnvMap env{
            {"BATBOX_SEARCH_ENGINE", "searxng"},
            {"BATBOX_SEARXNG_URL",   "ftp://bad.example.com"},
        };
        auto r = Config::load_from_env(env);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().find("BATBOX_SEARXNG_URL") != std::string::npos);
    }

    TEST_CASE("BATBOX_SIDECAR_STARTUP_TIMEOUT_SEC = 0 fails") {
        EnvMap env{{"BATBOX_SIDECAR_STARTUP_TIMEOUT_SEC", "0"}};
        auto r = Config::load_from_env(env);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().find("BATBOX_SIDECAR_STARTUP_TIMEOUT_SEC") != std::string::npos);
    }
}

// ============================================================================
// SUITE 5 — redacted_for_display masks api_key
// ============================================================================
TEST_SUITE("Config::redacted_for_display") {

    TEST_CASE("api_key is masked with ****") {
        EnvMap env{{"BATBOX_API_KEY", "sk-supersecret-do-not-share"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        const auto j = r->redacted_for_display();
        REQUIRE(j.contains("api"));
        REQUIRE(j["api"].contains("api_key"));
        CHECK(j["api"]["api_key"].get<std::string>() == "****");
    }

    TEST_CASE("to_json exposes api_key in full") {
        EnvMap env{{"BATBOX_API_KEY", "sk-realkey"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        const auto j = r->to_json();
        CHECK(j["api"]["api_key"].get<std::string>() == "sk-realkey");
    }

    TEST_CASE("non-secret fields are unchanged in redacted view") {
        EnvMap env{
            {"BATBOX_DEFAULT_MODEL", "gpt-4o"},
            {"BATBOX_API_KEY",       "sk-secret"},
        };
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        const auto j = r->redacted_for_display();
        CHECK(j["api"]["default_model"].get<std::string>() == "gpt-4o");
    }
}

// ============================================================================
// SUITE 6 — Boolean parsing (all six accepted forms)
// ============================================================================
TEST_SUITE("Config — boolean field parsing") {

    TEST_CASE("'yes' is accepted as true") {
        EnvMap env{{"BATBOX_VIM_MODE", "yes"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->ui.vim_mode);
    }

    TEST_CASE("'YES' is accepted as true (case-insensitive)") {
        EnvMap env{{"BATBOX_VIM_MODE", "YES"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->ui.vim_mode);
    }

    TEST_CASE("'no' is accepted as false") {
        EnvMap env{{"BATBOX_SIDECAR_AUTOSTART", "no"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK_FALSE(r->sidecar.autostart);
    }

    TEST_CASE("'0' is accepted as false") {
        EnvMap env{{"BATBOX_SIDECAR_PREWARM", "0"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK_FALSE(r->sidecar.prewarm);
    }

    TEST_CASE("'1' is accepted as true") {
        EnvMap env{{"BATBOX_SIDECAR_PREWARM", "1"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->sidecar.prewarm);
    }

    TEST_CASE("'True' mixed-case accepted") {
        EnvMap env{{"BATBOX_NO_SPLASH", "True"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->ui.no_splash);
    }
}

// ============================================================================
// SUITE 7 — to_json round-trip for enum fields
// ============================================================================
TEST_SUITE("Config::to_json — enum round-trips") {

    TEST_CASE("permission_mode round-trip: nuclear") {
        EnvMap env{{"BATBOX_PERMISSION_MODE", "nuclear"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        const auto j = r->to_json();
        CHECK(j["security"]["permission_mode"].get<std::string>() == "nuclear");
    }

    TEST_CASE("theme round-trip: frank-sinatra") {
        EnvMap env{{"BATBOX_THEME", "frank-sinatra"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        const auto j = r->to_json();
        CHECK(j["ui"]["theme"].get<std::string>() == "frank-sinatra");
    }

    TEST_CASE("log_level round-trip: warn") {
        EnvMap env{{"BATBOX_LOG_LEVEL", "warn"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        const auto j = r->to_json();
        CHECK(j["general"]["log_level"].get<std::string>() == "warn");
    }

    TEST_CASE("search engine round-trip: searxng") {
        EnvMap env{
            {"BATBOX_SEARCH_ENGINE", "searxng"},
            {"BATBOX_SEARXNG_URL",   "https://sx.example.com"},
        };
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        const auto j = r->to_json();
        CHECK(j["search"]["engine"].get<std::string>() == "searxng");
    }
}

// ============================================================================
// SUITE 8 — version_seq copy semantics
// ============================================================================
TEST_SUITE("Config::version_seq — copy and move semantics") {

    TEST_CASE("default version_seq is 0") {
        const auto cfg = Config::load_default();
        CHECK(cfg.version_seq.load() == 0u);
    }

    TEST_CASE("copy constructor preserves version_seq") {
        Config a = Config::load_default();
        a.version_seq.store(42u);
        const Config b = a;
        CHECK(b.version_seq.load() == 42u);
    }

    TEST_CASE("copy assignment preserves version_seq") {
        Config a = Config::load_default();
        a.version_seq.store(77u);
        Config b;
        b = a;
        CHECK(b.version_seq.load() == 77u);
    }

    TEST_CASE("move constructor preserves version_seq") {
        Config a = Config::load_default();
        a.version_seq.store(55u);
        const Config b = std::move(a);
        CHECK(b.version_seq.load() == 55u);
    }

    TEST_CASE("version_seq can be incremented independently after copy") {
        Config a = Config::load_default();
        a.version_seq.store(10u);
        Config b = a;
        b.version_seq.fetch_add(1u);
        CHECK(a.version_seq.load() == 10u);
        CHECK(b.version_seq.load() == 11u);
    }
}

// ============================================================================
// SUITE 9 — OpenAI-convention env-var fallbacks (Fix #18)
// ============================================================================
TEST_SUITE("Config — OpenAI-convention env-var fallbacks") {

    // -------------------------------------------------------------------------
    // BATBOX_API_BASE_URL / OPENAI_BASE_URL
    // -------------------------------------------------------------------------

    TEST_CASE("canonical BATBOX_API_BASE_URL wins over OPENAI_BASE_URL") {
        EnvMap env{
            {"BATBOX_API_BASE_URL", "http://localhost:11434/v1"},
            {"OPENAI_BASE_URL",     "http://other.example.com/v1"},
        };
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.base_url == "http://localhost:11434/v1");
    }

    TEST_CASE("OPENAI_BASE_URL used when BATBOX_API_BASE_URL absent") {
        EnvMap env{
            {"OPENAI_BASE_URL", "http://openai-compat.example.com/v1"},
        };
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.base_url == "http://openai-compat.example.com/v1");
    }

    TEST_CASE("both BATBOX_API_BASE_URL and OPENAI_BASE_URL absent yields built-in default") {
        EnvMap env{};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.base_url == "https://api.openai.com/v1");
    }

    // -------------------------------------------------------------------------
    // BATBOX_API_KEY / OPENAI_API_KEY
    // -------------------------------------------------------------------------

    TEST_CASE("canonical BATBOX_API_KEY wins over OPENAI_API_KEY") {
        EnvMap env{
            {"BATBOX_API_KEY", "sk-batbox-canonical"},
            {"OPENAI_API_KEY", "sk-openai-fallback"},
        };
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.api_key == "sk-batbox-canonical");
    }

    TEST_CASE("OPENAI_API_KEY used when BATBOX_API_KEY absent") {
        EnvMap env{
            {"OPENAI_API_KEY", "sk-openai-key-123"},
        };
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.api_key == "sk-openai-key-123");
    }

    TEST_CASE("both BATBOX_API_KEY and OPENAI_API_KEY absent yields empty api_key") {
        EnvMap env{};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.api_key.empty());
    }

    // -------------------------------------------------------------------------
    // BATBOX_DEFAULT_MODEL / BATBOX_MODEL alias
    // -------------------------------------------------------------------------

    TEST_CASE("canonical BATBOX_DEFAULT_MODEL wins over BATBOX_MODEL alias") {
        EnvMap env{
            {"BATBOX_DEFAULT_MODEL", "gpt-4o"},
            {"BATBOX_MODEL",         "llama3.1"},
        };
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.default_model == "gpt-4o");
    }

    TEST_CASE("BATBOX_MODEL alias used when BATBOX_DEFAULT_MODEL absent") {
        EnvMap env{
            {"BATBOX_MODEL", "llama3.1"},
        };
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.default_model == "llama3.1");
    }

    TEST_CASE("both BATBOX_DEFAULT_MODEL and BATBOX_MODEL absent yields built-in default") {
        EnvMap env{};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        // Default model is gpt-4o (matches ApiConfig::default_model initializer)
        CHECK(r->api.default_model == "gpt-4o");
    }

    TEST_CASE("BATBOX_MODEL with BATBOX_DEFAULT_MODEL unset resolves correctly") {
        // Regression: BATBOX_MODEL should be treated as the alias for the model field.
        EnvMap env{
            {"BATBOX_MODEL", "mistral-7b"},
        };
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.default_model == "mistral-7b");
    }

    TEST_CASE("BATBOX_MODELS set but no model name env var: default_model falls back to first of BATBOX_MODELS") {
        // When neither BATBOX_DEFAULT_MODEL nor BATBOX_MODEL is set,
        // default_model should use the first entry of BATBOX_MODELS if available.
        EnvMap env{
            {"BATBOX_MODELS", "mixtral-8x7b, gpt-4o-mini"},
        };
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.default_model == "mixtral-8x7b");
    }

    TEST_CASE("BATBOX_DEFAULT_MODEL beats BATBOX_MODELS first-entry fallback") {
        EnvMap env{
            {"BATBOX_DEFAULT_MODEL", "gpt-4o"},
            {"BATBOX_MODELS",        "mixtral-8x7b, gpt-4o-mini"},
        };
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.default_model == "gpt-4o");
    }
}
// ============================================================================
// SUITE 10 — Model alias fields: defaults + env overrides
// ============================================================================
TEST_SUITE("Config — model alias fields (opus/sonnet/haiku)") {

    TEST_CASE("opus_model defaults to default_model when BATBOX_OPUS_MODEL unset") {
        EnvMap env{{"BATBOX_DEFAULT_MODEL", "kimi-k2.6:cloud"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.opus_model == r->api.default_model);
    }

    TEST_CASE("sonnet_model defaults to default_model when BATBOX_SONNET_MODEL unset") {
        EnvMap env{{"BATBOX_DEFAULT_MODEL", "kimi-k2.6:cloud"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.sonnet_model == r->api.default_model);
    }

    TEST_CASE("haiku_model defaults to default_model when BATBOX_HAIKU_MODEL unset") {
        EnvMap env{{"BATBOX_DEFAULT_MODEL", "kimi-k2.6:cloud"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.haiku_model == r->api.default_model);
    }

    TEST_CASE("BATBOX_OPUS_MODEL set overrides opus_model") {
        EnvMap env{
            {"BATBOX_DEFAULT_MODEL", "kimi-k2.6:cloud"},
            {"BATBOX_OPUS_MODEL",    "llama3.3:70b-cloud"},
        };
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.opus_model   == "llama3.3:70b-cloud");
        CHECK(r->api.sonnet_model == "kimi-k2.6:cloud");  // still falls back to default
    }

    TEST_CASE("BATBOX_SONNET_MODEL set overrides sonnet_model") {
        EnvMap env{
            {"BATBOX_DEFAULT_MODEL",  "kimi-k2.6:cloud"},
            {"BATBOX_SONNET_MODEL",   "llama3.2:3b-cloud"},
        };
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.sonnet_model == "llama3.2:3b-cloud");
        CHECK(r->api.opus_model   == "kimi-k2.6:cloud");  // still falls back to default
    }

    TEST_CASE("BATBOX_HAIKU_MODEL set overrides haiku_model") {
        EnvMap env{
            {"BATBOX_DEFAULT_MODEL", "kimi-k2.6:cloud"},
            {"BATBOX_HAIKU_MODEL",   "llama3.2:1b-cloud"},
        };
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.haiku_model  == "llama3.2:1b-cloud");
        CHECK(r->api.opus_model   == "kimi-k2.6:cloud");  // still falls back to default
        CHECK(r->api.sonnet_model == "kimi-k2.6:cloud");  // still falls back to default
    }

    TEST_CASE("all three alias env vars set independently") {
        EnvMap env{
            {"BATBOX_DEFAULT_MODEL", "base-model"},
            {"BATBOX_OPUS_MODEL",    "big-model"},
            {"BATBOX_SONNET_MODEL",  "mid-model"},
            {"BATBOX_HAIKU_MODEL",   "small-model"},
        };
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.default_model == "base-model");
        CHECK(r->api.opus_model    == "big-model");
        CHECK(r->api.sonnet_model  == "mid-model");
        CHECK(r->api.haiku_model   == "small-model");
    }
}
