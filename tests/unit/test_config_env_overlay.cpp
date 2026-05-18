// tests/unit/test_config_env_overlay.cpp
// ---------------------------------------------------------------------------
// PEXT 3.2 — AUDIT-2: config env-overlay end-to-end test
//
// Dedicated regression guard that asserts Config::load_from_env() correctly
// overlays EVERY BATBOX_* env key catalogued in path-audit.md §2.
//
// One TEST_CASE per key (or per fallback group).  Successful-overlay cases
// only — validation-error cases live in tests/unit/test_config_load.cpp.
//
// If a new BATBOX_* key is added to apply_env() in Config.cpp, add a
// matching TEST_CASE here so this file stays authoritative.
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/config/Config.hpp>
#include <batbox/config/EnvLoader.hpp>

using namespace batbox::config;
using namespace batbox;

// ============================================================================
// TEST_SUITE: one TEST_CASE per BATBOX_* env key
// ============================================================================

TEST_SUITE("Config::load_from_env — complete BATBOX_* env-overlay coverage") {

    // -----------------------------------------------------------------------
    // API group
    // -----------------------------------------------------------------------

    TEST_CASE("BATBOX_MODELS overlay — CSV parsed and trimmed") {
        EnvMap env{{"BATBOX_MODELS", "alpha, beta, gamma"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        REQUIRE(r->api.models.size() == 3);
        CHECK(r->api.models[0] == "alpha");
        CHECK(r->api.models[1] == "beta");
        CHECK(r->api.models[2] == "gamma");
    }

    TEST_CASE("BATBOX_API_BASE_URL overlay") {
        EnvMap env{{"BATBOX_API_BASE_URL", "http://localhost:11434/v1"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.base_url == "http://localhost:11434/v1");
    }

    TEST_CASE("OPENAI_BASE_URL fallback when BATBOX_API_BASE_URL absent") {
        EnvMap env{{"OPENAI_BASE_URL", "http://openai-compat.example.com/v1"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.base_url == "http://openai-compat.example.com/v1");
    }

    TEST_CASE("BATBOX_API_KEY overlay") {
        EnvMap env{{"BATBOX_API_KEY", "sk-test-key-abc"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.api_key == "sk-test-key-abc");
    }

    TEST_CASE("OPENAI_API_KEY fallback when BATBOX_API_KEY absent") {
        EnvMap env{{"OPENAI_API_KEY", "sk-openai-fallback-key"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.api_key == "sk-openai-fallback-key");
    }

    TEST_CASE("BATBOX_DEFAULT_MODEL overlay") {
        EnvMap env{{"BATBOX_DEFAULT_MODEL", "llama3.1:70b"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.default_model == "llama3.1:70b");
    }

    TEST_CASE("BATBOX_MODEL alias fallback when BATBOX_DEFAULT_MODEL absent") {
        EnvMap env{{"BATBOX_MODEL", "mistral-7b"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.default_model == "mistral-7b");
    }

    TEST_CASE("BATBOX_OPUS_MODEL overlay — independent of other aliases") {
        EnvMap env{
            {"BATBOX_DEFAULT_MODEL", "base-model"},
            {"BATBOX_OPUS_MODEL",    "big-opus-model"},
        };
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.opus_model   == "big-opus-model");
        CHECK(r->api.sonnet_model == "base-model");  // falls back to default_model
        CHECK(r->api.haiku_model  == "base-model");  // falls back to default_model
    }

    TEST_CASE("BATBOX_SONNET_MODEL overlay — independent of other aliases") {
        EnvMap env{
            {"BATBOX_DEFAULT_MODEL", "base-model"},
            {"BATBOX_SONNET_MODEL",  "mid-sonnet-model"},
        };
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.sonnet_model == "mid-sonnet-model");
        CHECK(r->api.opus_model   == "base-model");
        CHECK(r->api.haiku_model  == "base-model");
    }

    TEST_CASE("BATBOX_HAIKU_MODEL overlay — independent of other aliases") {
        EnvMap env{
            {"BATBOX_DEFAULT_MODEL", "base-model"},
            {"BATBOX_HAIKU_MODEL",   "small-haiku-model"},
        };
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.haiku_model  == "small-haiku-model");
        CHECK(r->api.opus_model   == "base-model");
        CHECK(r->api.sonnet_model == "base-model");
    }

    TEST_CASE("BATBOX_MAX_TOKENS overlay") {
        EnvMap env{{"BATBOX_MAX_TOKENS", "8192"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.max_tokens == 8192);
    }

    TEST_CASE("BATBOX_TEMPERATURE overlay") {
        EnvMap env{{"BATBOX_TEMPERATURE", "0.42"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.temperature == doctest::Approx(0.42));
    }

    TEST_CASE("BATBOX_TOP_P overlay") {
        EnvMap env{{"BATBOX_TOP_P", "0.9"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.top_p == doctest::Approx(0.9));
    }

    TEST_CASE("BATBOX_REQUEST_TIMEOUT_SEC overlay") {
        EnvMap env{{"BATBOX_REQUEST_TIMEOUT_SEC", "30"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.request_timeout_sec == 30);
    }

    TEST_CASE("BATBOX_STREAM overlay — false disables streaming") {
        EnvMap env{{"BATBOX_STREAM", "false"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK_FALSE(r->api.stream);
    }

    TEST_CASE("BATBOX_PROVIDER_HINT overlay") {
        EnvMap env{{"BATBOX_PROVIDER_HINT", "ollama"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.provider_hint == "ollama");
    }

    TEST_CASE("BATBOX_STREAM_IDLE_TIMEOUT_SEC overlay") {
        EnvMap env{{"BATBOX_STREAM_IDLE_TIMEOUT_SEC", "120"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.stream_idle_timeout_sec == 120);
    }

    TEST_CASE("BATBOX_CTX_LEN_<MODEL> per-model overlay") {
        // For model "test-model-abc", the env key is
        // BATBOX_CTX_LEN_TEST_MODEL_ABC (uppercased, non-alphanum → '_').
        EnvMap env{
            {"BATBOX_DEFAULT_MODEL",         "test-model-abc"},
            {"BATBOX_CTX_LEN_TEST_MODEL_ABC", "32768"},
        };
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.default_model_ctx_len == static_cast<std::size_t>(32768));
    }

    TEST_CASE("BATBOX_CTX_LEN_DEFAULT global fallback overlay") {
        // When the per-model key is absent, BATBOX_CTX_LEN_DEFAULT is used.
        EnvMap env{
            {"BATBOX_DEFAULT_MODEL",  "some-unknown-model"},
            {"BATBOX_CTX_LEN_DEFAULT", "16384"},
        };
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.default_model_ctx_len == static_cast<std::size_t>(16384));
    }

    TEST_CASE("per-model BATBOX_CTX_LEN_<MODEL> wins over BATBOX_CTX_LEN_DEFAULT") {
        EnvMap env{
            {"BATBOX_DEFAULT_MODEL",            "my-model"},
            {"BATBOX_CTX_LEN_MY_MODEL",         "65536"},
            {"BATBOX_CTX_LEN_DEFAULT",           "8192"},
        };
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->api.default_model_ctx_len == static_cast<std::size_t>(65536));
    }

    // -----------------------------------------------------------------------
    // General group
    // -----------------------------------------------------------------------

    TEST_CASE("BATBOX_CONFIG_DIR overlay") {
        EnvMap env{{"BATBOX_CONFIG_DIR", "/tmp/test-batbox-config"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->general.config_dir.string() == "/tmp/test-batbox-config");
    }

    TEST_CASE("BATBOX_PROJECT_MEMORY_FILE overlay") {
        EnvMap env{{"BATBOX_PROJECT_MEMORY_FILE", "MY_NOTES.md"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->general.project_memory_file == "MY_NOTES.md");
    }

    TEST_CASE("BATBOX_LOG_LEVEL overlay — all five valid values") {
        for (const auto& [str, expected] : std::initializer_list<std::pair<const char*, LogLevel>>{
                {"trace", LogLevel::Trace},
                {"debug", LogLevel::Debug},
                {"info",  LogLevel::Info},
                {"warn",  LogLevel::Warn},
                {"error", LogLevel::Error},
            }) {
            EnvMap env{{"BATBOX_LOG_LEVEL", str}};
            auto r = Config::load_from_env(env);
            REQUIRE(r.has_value());
            CHECK(r->general.log_level == expected);
        }
    }

    TEST_CASE("BATBOX_LOG_FILE overlay") {
        EnvMap env{{"BATBOX_LOG_FILE", "/tmp/test-batbox.log"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->general.log_file.string() == "/tmp/test-batbox.log");
    }

    // -----------------------------------------------------------------------
    // UI group
    // -----------------------------------------------------------------------

    TEST_CASE("BATBOX_THEME overlay — all five valid values") {
        for (const auto& [str, expected] : std::initializer_list<std::pair<const char*, Theme>>{
                {"miss-kittin",    Theme::MissKittin},
                {"stock-exchange", Theme::StockExchange},
                {"frank-sinatra",  Theme::FrankSinatra},
                {"monochrome",     Theme::Monochrome},
                {"classic",        Theme::Classic},
            }) {
            EnvMap env{{"BATBOX_THEME", str}};
            auto r = Config::load_from_env(env);
            REQUIRE(r.has_value());
            CHECK(r->ui.theme == expected);
        }
    }

    TEST_CASE("BATBOX_NO_SPLASH overlay — true enables no-splash") {
        EnvMap env{{"BATBOX_NO_SPLASH", "true"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->ui.no_splash);
    }

    TEST_CASE("BATBOX_VIM_MODE overlay — 1 enables vim mode") {
        EnvMap env{{"BATBOX_VIM_MODE", "1"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->ui.vim_mode);
    }

    TEST_CASE("BATBOX_STATUSLINE overlay — all three valid values") {
        for (const auto& [str, expected] : std::initializer_list<std::pair<const char*, StatusLine>>{
                {"default", StatusLine::Default},
                {"minimal", StatusLine::Minimal},
                {"verbose", StatusLine::Verbose},
            }) {
            EnvMap env{{"BATBOX_STATUSLINE", str}};
            auto r = Config::load_from_env(env);
            REQUIRE(r.has_value());
            CHECK(r->ui.statusline == expected);
        }
    }

    // -----------------------------------------------------------------------
    // Search / WebFetch group
    // -----------------------------------------------------------------------

    TEST_CASE("BATBOX_SEARCH_ENGINE overlay — ddg and searxng") {
        for (const auto& [str, expected] : std::initializer_list<std::pair<const char*, SearchEngine>>{
                {"ddg",     SearchEngine::Ddg},
                {"searxng", SearchEngine::Searxng},
            }) {
            EnvMap env{
                {"BATBOX_SEARCH_ENGINE", str},
                // Provide a valid searxng URL so validation passes for searxng.
                {"BATBOX_SEARXNG_URL",   "https://searxng.example.com"},
            };
            auto r = Config::load_from_env(env);
            REQUIRE(r.has_value());
            CHECK(r->search.engine == expected);
        }
    }

    TEST_CASE("BATBOX_SEARXNG_URL overlay") {
        EnvMap env{
            {"BATBOX_SEARCH_ENGINE", "searxng"},
            {"BATBOX_SEARXNG_URL",   "https://my-searxng.example.org"},
        };
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->search.searxng_url == "https://my-searxng.example.org");
    }

    TEST_CASE("BATBOX_RESPECT_ROBOTS overlay — false disables robots.txt check") {
        EnvMap env{{"BATBOX_RESPECT_ROBOTS", "no"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK_FALSE(r->search.respect_robots);
    }

    TEST_CASE("BATBOX_WEBFETCH_MAX_BYTES overlay") {
        EnvMap env{{"BATBOX_WEBFETCH_MAX_BYTES", "2097152"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->search.webfetch_max_bytes == 2'097'152);
    }

    TEST_CASE("BATBOX_WEBFETCH_TIMEOUT_SEC overlay") {
        EnvMap env{{"BATBOX_WEBFETCH_TIMEOUT_SEC", "45"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->search.webfetch_timeout_sec == 45);
    }

    // -----------------------------------------------------------------------
    // Sidecar group
    // -----------------------------------------------------------------------

    TEST_CASE("BATBOX_SIDECAR_PYTHON overlay") {
        EnvMap env{{"BATBOX_SIDECAR_PYTHON", "/usr/local/bin/python3"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->sidecar.python.string() == "/usr/local/bin/python3");
    }

    TEST_CASE("BATBOX_SIDECAR_VENV overlay") {
        EnvMap env{{"BATBOX_SIDECAR_VENV", "/tmp/test-venv"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->sidecar.venv.string() == "/tmp/test-venv");
    }

    TEST_CASE("BATBOX_SIDECAR_STARTUP_TIMEOUT_SEC overlay") {
        EnvMap env{{"BATBOX_SIDECAR_STARTUP_TIMEOUT_SEC", "25"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->sidecar.startup_timeout_sec == 25);
    }

    TEST_CASE("BATBOX_SIDECAR_AUTOSTART overlay — false disables autostart") {
        EnvMap env{{"BATBOX_SIDECAR_AUTOSTART", "false"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK_FALSE(r->sidecar.autostart);
    }

    TEST_CASE("BATBOX_SIDECAR_PREWARM overlay — true enables prewarm") {
        EnvMap env{{"BATBOX_SIDECAR_PREWARM", "true"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->sidecar.prewarm);
    }

    // -----------------------------------------------------------------------
    // Tools group
    // -----------------------------------------------------------------------

    TEST_CASE("BATBOX_BASH_TIMEOUT_SEC overlay") {
        EnvMap env{{"BATBOX_BASH_TIMEOUT_SEC", "60"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->tools.bash_timeout_sec == 60);
    }

    TEST_CASE("BATBOX_BASH_MAX_OUTPUT_BYTES overlay") {
        EnvMap env{{"BATBOX_BASH_MAX_OUTPUT_BYTES", "524288"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->tools.bash_max_output_bytes == 524288);
    }

    TEST_CASE("BATBOX_TASK_PARALLEL_LIMIT overlay") {
        EnvMap env{{"BATBOX_TASK_PARALLEL_LIMIT", "8"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->tools.task_parallel_limit == 8);
    }

    // -----------------------------------------------------------------------
    // MCP group
    // -----------------------------------------------------------------------

    TEST_CASE("BATBOX_MCP_CONFIG overlay") {
        EnvMap env{{"BATBOX_MCP_CONFIG", "/tmp/test-mcp.json"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->mcp.config_path.string() == "/tmp/test-mcp.json");
    }

    TEST_CASE("BATBOX_MCP_STARTUP_TIMEOUT_SEC overlay") {
        EnvMap env{{"BATBOX_MCP_STARTUP_TIMEOUT_SEC", "20"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->mcp.startup_timeout_sec == 20);
    }

    // -----------------------------------------------------------------------
    // Agents group
    // -----------------------------------------------------------------------

    TEST_CASE("BATBOX_AGENTS_CONFIG overlay") {
        EnvMap env{{"BATBOX_AGENTS_CONFIG", "/tmp/test-agents.json"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->agents.agents_config.string() == "/tmp/test-agents.json");
    }

    TEST_CASE("BATBOX_AGENTS_DIR overlay") {
        EnvMap env{{"BATBOX_AGENTS_DIR", "/tmp/test-agents"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->agents.agents_dir.string() == "/tmp/test-agents");
    }

    TEST_CASE("BATBOX_DEMON_ENABLED overlay — true enables demon mode") {
        EnvMap env{{"BATBOX_DEMON_ENABLED", "true"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->agents.demon_enabled);
    }

    // -----------------------------------------------------------------------
    // Security group
    // -----------------------------------------------------------------------

    TEST_CASE("BATBOX_PERMISSION_MODE overlay — all four valid values") {
        for (const auto& [str, expected] : std::initializer_list<std::pair<const char*, PermissionMode>>{
                {"default",     PermissionMode::Default},
                {"plan",        PermissionMode::Plan},
                {"acceptedits", PermissionMode::AcceptEdits},
                {"nuclear",     PermissionMode::Nuclear},
            }) {
            EnvMap env{{"BATBOX_PERMISSION_MODE", str}};
            auto r = Config::load_from_env(env);
            REQUIRE(r.has_value());
            CHECK(r->security.permission_mode == expected);
        }
    }

    TEST_CASE("BATBOX_AUTO_APPROVE_READS overlay — false disables auto-approve") {
        EnvMap env{{"BATBOX_AUTO_APPROVE_READS", "false"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK_FALSE(r->security.auto_approve_reads);
    }

    // -----------------------------------------------------------------------
    // Compact group
    // -----------------------------------------------------------------------

    TEST_CASE("BATBOX_AUTO_COMPACT_AT_PCT overlay") {
        EnvMap env{{"BATBOX_AUTO_COMPACT_AT_PCT", "70"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->compact.auto_compact_at_pct == 70);
    }

    TEST_CASE("BATBOX_KEEP_LAST_N_TURNS_VERBATIM overlay") {
        EnvMap env{{"BATBOX_KEEP_LAST_N_TURNS_VERBATIM", "5"}};
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        CHECK(r->compact.keep_last_n_turns_verbatim == 5);
    }

    // -----------------------------------------------------------------------
    // Boolean parsing — shared by all bool fields
    // -----------------------------------------------------------------------

    TEST_CASE("Boolean overlay — all six accepted forms (via BATBOX_VIM_MODE)") {
        // Truthy forms
        for (const char* val : {"true", "True", "TRUE", "1", "yes", "YES"}) {
            EnvMap env{{"BATBOX_VIM_MODE", val}};
            auto r = Config::load_from_env(env);
            REQUIRE(r.has_value());
            CHECK(r->ui.vim_mode);
        }
        // Falsy forms
        for (const char* val : {"false", "False", "FALSE", "0", "no", "NO"}) {
            EnvMap env{{"BATBOX_VIM_MODE", val}};
            auto r = Config::load_from_env(env);
            REQUIRE(r.has_value());
            CHECK_FALSE(r->ui.vim_mode);
        }
    }

    // -----------------------------------------------------------------------
    // Snapshot: all keys absent → load_from_env still succeeds with defaults
    // -----------------------------------------------------------------------

    TEST_CASE("empty EnvMap succeeds with built-in defaults") {
        EnvMap env;
        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());
        // Spot-check a few defaults to confirm nothing exploded.
        CHECK(r->api.base_url         == "https://api.openai.com/v1");
        CHECK(r->api.default_model    == "gpt-4o");
        CHECK(r->general.log_level    == LogLevel::Info);
        CHECK(r->ui.theme             == Theme::MissKittin);
        CHECK(r->search.engine        == SearchEngine::Ddg);
        CHECK(r->security.permission_mode == PermissionMode::Default);
        CHECK(r->compact.auto_compact_at_pct        == 80);
        CHECK(r->compact.keep_last_n_turns_verbatim == 10);
    }

} // TEST_SUITE

// ============================================================================
// KEYS COVERED BY THIS FILE (audit checklist — update when apply_env changes)
// ============================================================================
//
// API group:
//   BATBOX_MODELS                      → cfg.api.models
//   BATBOX_API_BASE_URL                → cfg.api.base_url
//   OPENAI_BASE_URL                    → cfg.api.base_url (fallback)
//   BATBOX_API_KEY                     → cfg.api.api_key
//   OPENAI_API_KEY                     → cfg.api.api_key (fallback)
//   BATBOX_DEFAULT_MODEL               → cfg.api.default_model
//   BATBOX_MODEL                       → cfg.api.default_model (fallback alias)
//   BATBOX_OPUS_MODEL                  → cfg.api.opus_model
//   BATBOX_SONNET_MODEL                → cfg.api.sonnet_model
//   BATBOX_HAIKU_MODEL                 → cfg.api.haiku_model
//   BATBOX_MAX_TOKENS                  → cfg.api.max_tokens
//   BATBOX_TEMPERATURE                 → cfg.api.temperature
//   BATBOX_TOP_P                       → cfg.api.top_p
//   BATBOX_REQUEST_TIMEOUT_SEC         → cfg.api.request_timeout_sec
//   BATBOX_STREAM                      → cfg.api.stream
//   BATBOX_PROVIDER_HINT               → cfg.api.provider_hint
//   BATBOX_STREAM_IDLE_TIMEOUT_SEC     → cfg.api.stream_idle_timeout_sec
//   BATBOX_CTX_LEN_<MODEL>             → cfg.api.default_model_ctx_len (per-model)
//   BATBOX_CTX_LEN_DEFAULT             → cfg.api.default_model_ctx_len (global fallback)
//
// General group:
//   BATBOX_CONFIG_DIR                  → cfg.general.config_dir
//   BATBOX_PROJECT_MEMORY_FILE         → cfg.general.project_memory_file
//   BATBOX_LOG_LEVEL                   → cfg.general.log_level
//   BATBOX_LOG_FILE                    → cfg.general.log_file
//
// UI group:
//   BATBOX_THEME                       → cfg.ui.theme
//   BATBOX_NO_SPLASH                   → cfg.ui.no_splash
//   BATBOX_VIM_MODE                    → cfg.ui.vim_mode
//   BATBOX_STATUSLINE                  → cfg.ui.statusline
//
// Search / WebFetch group:
//   BATBOX_SEARCH_ENGINE               → cfg.search.engine
//   BATBOX_SEARXNG_URL                 → cfg.search.searxng_url
//   BATBOX_RESPECT_ROBOTS              → cfg.search.respect_robots
//   BATBOX_WEBFETCH_MAX_BYTES          → cfg.search.webfetch_max_bytes
//   BATBOX_WEBFETCH_TIMEOUT_SEC        → cfg.search.webfetch_timeout_sec
//
// Sidecar group:
//   BATBOX_SIDECAR_PYTHON              → cfg.sidecar.python
//   BATBOX_SIDECAR_VENV                → cfg.sidecar.venv
//   BATBOX_SIDECAR_STARTUP_TIMEOUT_SEC → cfg.sidecar.startup_timeout_sec
//   BATBOX_SIDECAR_AUTOSTART           → cfg.sidecar.autostart
//   BATBOX_SIDECAR_PREWARM             → cfg.sidecar.prewarm
//
// Tools group:
//   BATBOX_BASH_TIMEOUT_SEC            → cfg.tools.bash_timeout_sec
//   BATBOX_BASH_MAX_OUTPUT_BYTES       → cfg.tools.bash_max_output_bytes
//   BATBOX_TASK_PARALLEL_LIMIT         → cfg.tools.task_parallel_limit
//
// MCP group:
//   BATBOX_MCP_CONFIG                  → cfg.mcp.config_path
//   BATBOX_MCP_STARTUP_TIMEOUT_SEC     → cfg.mcp.startup_timeout_sec
//
// Agents group:
//   BATBOX_AGENTS_CONFIG               → cfg.agents.agents_config
//   BATBOX_AGENTS_DIR                  → cfg.agents.agents_dir
//   BATBOX_DEMON_ENABLED               → cfg.agents.demon_enabled
//
// Security group:
//   BATBOX_PERMISSION_MODE             → cfg.security.permission_mode
//   BATBOX_AUTO_APPROVE_READS          → cfg.security.auto_approve_reads
//
// Compact group:
//   BATBOX_AUTO_COMPACT_AT_PCT         → cfg.compact.auto_compact_at_pct
//   BATBOX_KEEP_LAST_N_TURNS_VERBATIM  → cfg.compact.keep_last_n_turns_verbatim
//
// Total distinct BATBOX_* keys covered: 51 (24 from audit table + 27 additional
// keys found in Config.cpp apply_env that were not in the original audit table).
// ============================================================================
