// tests/integration/test_config_reload.cpp
// ---------------------------------------------------------------------------
// Integration tests for batbox::config::reload_config and ConfigReloadBus.
//
// Exercises:
//   AC1 — reload() returns Ok(ReloadReport) with correct changed_fields
//   AC2 — version_seq increments on every successful reload
//   AC3 — registered subscribers are called after each reload
//   AC4 — errors during reload do NOT corrupt the previous config
//         (transactional guarantee)
//   AC5 — restart_required_fields lists sidecar.python and general.log_file
//   AC6 — unchanged config produces an empty changed_fields list
//   AC7 — multiple subscribers all fire; unregistered subscriber does not
//   AC8 — reload on empty config_dir (no .env, no settings.json) succeeds
//         and matches Config::load_default() fields
//
// Build standalone (adapt paths for your platform):
//   ROOT=/path/to/batbox
//   c++ -std=c++20 \
//       -I $ROOT/include \
//       -I $ROOT/build/vcpkg_installed/arm64-osx/include \
//       $ROOT/tests/integration/test_config_reload.cpp \
//       $ROOT/src/config/Config.cpp \
//       $ROOT/src/config/ConfigReload.cpp \
//       $ROOT/src/config/EnvLoader.cpp \
//       $ROOT/src/config/SettingsLoader.cpp \
//       $ROOT/src/core/Logging.cpp \
//       $ROOT/src/core/Paths.cpp \
//       $ROOT/src/core/Json.cpp \
//       $ROOT/build/vcpkg_installed/arm64-osx/lib/libfmt.a \
//       $ROOT/build/vcpkg_installed/arm64-osx/lib/libspdlog.a \
//       $ROOT/build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       -o /tmp/test_config_reload && /tmp/test_config_reload
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/config/Config.hpp>
#include <batbox/config/ConfigReload.hpp>
#include <batbox/config/EnvLoader.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace batbox;
using namespace batbox::config;

// ============================================================================
// Test fixture helpers
// ============================================================================
namespace {

/// RAII temp directory: created in the system tmp dir, removed on destruction.
struct TempDir {
    fs::path path;

    TempDir() {
        const auto unique_id =
            static_cast<unsigned long>(::getpid()) * 1000000UL
            + static_cast<unsigned long>(
                std::chrono::steady_clock::now().time_since_epoch().count() % 1000000);
        path = fs::temp_directory_path() /
               ("batbox_test_config_reload_" + std::to_string(unique_id));
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;
};

/// Write text to a file, creating parent directories as needed.
void write_file(const fs::path& p, std::string_view content) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::trunc);
    REQUIRE(f.is_open());
    f << content;
}

/// Return true when 'field' appears in 'vec'.
bool contains(const std::vector<std::string>& vec, const std::string& field) {
    return std::find(vec.begin(), vec.end(), field) != vec.end();
}

} // anonymous namespace

// ============================================================================
// AC1 — changed_fields reports the correct set of modified fields
// ============================================================================
TEST_CASE("AC1 — changed_fields contains modified fields") {
    TempDir d;

    // Start with a default config pointing at our temp dir.
    Config cfg = Config::load_default();
    cfg.general.config_dir = d.path;

    // Write a .env that changes api.max_tokens and ui.vim_mode.
    write_file(d.path / ".env",
               "BATBOX_MAX_TOKENS=8192\n"
               "BATBOX_VIM_MODE=true\n");

    auto result = reload_config(cfg, d.path);
    REQUIRE(result.has_value());

    const ReloadReport& rep = *result;
    CHECK(contains(rep.changed_fields, "api.max_tokens"));
    CHECK(contains(rep.changed_fields, "ui.vim_mode"));

    // Fields that were NOT in the .env should not appear.
    CHECK_FALSE(contains(rep.changed_fields, "api.base_url"));
    CHECK_FALSE(contains(rep.changed_fields, "api.api_key"));

    // Values should have been applied.
    CHECK(cfg.api.max_tokens == 8192);
    CHECK(cfg.ui.vim_mode);
}

// ============================================================================
// AC2 — version_seq increments on every successful reload
// ============================================================================
TEST_CASE("AC2 — version_seq increments on each successful reload") {
    TempDir d;
    Config cfg = Config::load_default();
    cfg.general.config_dir = d.path;

    const uint64_t seq0 = cfg.version_seq.load();

    auto r1 = reload_config(cfg, d.path);
    REQUIRE(r1.has_value());
    const uint64_t seq1 = cfg.version_seq.load();
    CHECK(seq1 == seq0 + 1);

    auto r2 = reload_config(cfg, d.path);
    REQUIRE(r2.has_value());
    const uint64_t seq2 = cfg.version_seq.load();
    CHECK(seq2 == seq1 + 1);

    auto r3 = reload_config(cfg, d.path);
    REQUIRE(r3.has_value());
    CHECK(cfg.version_seq.load() == seq2 + 1);
}

// ============================================================================
// AC3 — registered subscribers are called after each reload
// ============================================================================
TEST_CASE("AC3 — subscribers fire after successful reload") {
    TempDir d;
    Config cfg = Config::load_default();
    cfg.general.config_dir = d.path;

    // Write something so the reload has a change to report.
    write_file(d.path / ".env", "BATBOX_MAX_TOKENS=999\n");

    int fire_count = 0;
    uint64_t last_seq = 0;

    auto handle = ConfigReloadBus::instance().subscribe(
        [&](const Config& new_cfg, const ReloadReport& /*rep*/) {
            ++fire_count;
            last_seq = new_cfg.version_seq.load();
        });

    auto r = reload_config(cfg, d.path);
    REQUIRE(r.has_value());
    CHECK(fire_count == 1);
    CHECK(last_seq == cfg.version_seq.load());

    // Second reload — subscriber fires again.
    auto r2 = reload_config(cfg, d.path);
    REQUIRE(r2.has_value());
    CHECK(fire_count == 2);
}

// ============================================================================
// AC4 — errors during reload do NOT corrupt the previous config (transactional)
// ============================================================================
TEST_CASE("AC4 — failed reload leaves cfg unchanged") {
    TempDir d;
    Config cfg = Config::load_default();
    cfg.general.config_dir = d.path;

    // Set a recognisable value so we can verify it is preserved.
    cfg.api.max_tokens = 1234;
    const uint64_t seq_before = cfg.version_seq.load();

    // Write an invalid .env: max_tokens must be > 0; use "not_a_number" to
    // trigger a parse error (Config::load returns Err).
    write_file(d.path / ".env", "BATBOX_MAX_TOKENS=not_a_number\n");

    auto r = reload_config(cfg, d.path);
    CHECK_FALSE(r.has_value());
    CHECK_FALSE(r.error().empty());

    // cfg must be completely unchanged.
    CHECK(cfg.api.max_tokens == 1234);
    CHECK(cfg.version_seq.load() == seq_before);
}

// ============================================================================
// AC5 — restart_required_fields lists sidecar.python and general.log_file
// ============================================================================
TEST_CASE("AC5 — restart_required_fields for sidecar.python and log_file") {
    TempDir d;
    Config cfg = Config::load_default();
    cfg.general.config_dir = d.path;

    // Change both restart-required fields via .env.
    write_file(d.path / ".env",
               "BATBOX_SIDECAR_PYTHON=/opt/homebrew/bin/python3\n"
               "BATBOX_LOG_FILE=/tmp/batbox_test.log\n");

    auto r = reload_config(cfg, d.path);
    REQUIRE(r.has_value());

    CHECK(contains(r->changed_fields,          "sidecar.python"));
    CHECK(contains(r->restart_required_fields, "sidecar.python"));
    CHECK(contains(r->changed_fields,          "general.log_file"));
    CHECK(contains(r->restart_required_fields, "general.log_file"));

    // restart_required is a subset of changed.
    for (const auto& f : r->restart_required_fields) {
        CHECK(contains(r->changed_fields, f));
    }
}

// ============================================================================
// AC6 — unchanged config produces an empty changed_fields list
// ============================================================================
TEST_CASE("AC6 — no .env means empty changed_fields (identity reload)") {
    TempDir d;

    // Build the baseline cfg the same way reload_config() will build it:
    // load from the process environment with no .env file and no settings.json.
    // This ensures the starting point already reflects any BATBOX_* process env
    // vars, so a subsequent reload with the same inputs is a true identity.
    //
    // Key: we do NOT override cfg.general.config_dir — we pass d.path explicitly
    // as the config_dir parameter to reload_config() so the diff function never
    // sees a config_dir field change.
    EnvMap process_env;
    merge_with_process_env(process_env, /*process_env_wins=*/false);
    auto baseline_r = Config::load(process_env, Json::object());
    REQUIRE(baseline_r.has_value());
    Config cfg = std::move(*baseline_r);

    // Snapshot the baseline max_tokens for post-reload confirmation.
    const int prev_max_tokens = cfg.api.max_tokens;

    // Reload from an empty temp dir (no .env, no settings.json).
    // The explicit config_dir parameter is used only for file loading,
    // not reflected back into the new Config's general.config_dir.
    auto r = reload_config(cfg, d.path);
    REQUIRE(r.has_value());

    // Nothing changed — changed_fields should be empty.
    CHECK(r->is_unchanged());
    CHECK(r->changed_fields.empty());
    CHECK(r->restart_required_fields.empty());

    // Values should be stable across the identity reload.
    CHECK(cfg.api.max_tokens == prev_max_tokens);
}

// ============================================================================
// AC7 — multiple subscribers all fire; unregistered subscriber does not
// ============================================================================
TEST_CASE("AC7 — multiple subscribers / unregister mid-run") {
    TempDir d;
    Config cfg = Config::load_default();
    cfg.general.config_dir = d.path;

    int count_a = 0;
    int count_b = 0;
    int count_c = 0;

    auto ha = ConfigReloadBus::instance().subscribe(
        [&](const Config&, const ReloadReport&) { ++count_a; });
    auto hb = ConfigReloadBus::instance().subscribe(
        [&](const Config&, const ReloadReport&) { ++count_b; });
    {
        // hc lives only inside this scope — unregistered before the second reload.
        auto hc = ConfigReloadBus::instance().subscribe(
            [&](const Config&, const ReloadReport&) { ++count_c; });

        // First reload — all three subscribers should fire.
        auto r1 = reload_config(cfg, d.path);
        REQUIRE(r1.has_value());
        CHECK(count_a == 1);
        CHECK(count_b == 1);
        CHECK(count_c == 1);
    } // hc destroyed here — unregistered

    // Second reload — only a and b should fire.
    auto r2 = reload_config(cfg, d.path);
    REQUIRE(r2.has_value());
    CHECK(count_a == 2);
    CHECK(count_b == 2);
    CHECK(count_c == 1); // did NOT increment
}

// ============================================================================
// AC8 — empty config_dir (no files) succeeds and matches load_default()
// ============================================================================
TEST_CASE("AC8 — reload from empty directory succeeds") {
    TempDir d;
    Config cfg = Config::load_default();
    cfg.general.config_dir = d.path;

    // Verify no config files exist in d.path.
    CHECK_FALSE(fs::exists(d.path / ".env"));
    CHECK_FALSE(fs::exists(d.path / "settings.json"));

    auto r = reload_config(cfg, d.path);
    REQUIRE(r.has_value());

    // Config should match defaults for all unaffected fields.
    CHECK(cfg.api.base_url     == "https://api.openai.com/v1");
    CHECK(cfg.api.max_tokens   == 4096);
    CHECK(cfg.ui.theme         == Theme::MissKittin);
    CHECK(cfg.version_seq.load() == 1u); // incremented exactly once
}

// ============================================================================
// AC9 — subscriber receives the ReloadReport with accurate changed_fields
// ============================================================================
TEST_CASE("AC9 — subscriber receives accurate ReloadReport") {
    TempDir d;
    Config cfg = Config::load_default();
    cfg.general.config_dir = d.path;

    write_file(d.path / ".env",
               "BATBOX_DEFAULT_MODEL=gpt-4-turbo\n"
               "BATBOX_TEMPERATURE=0.5\n");

    std::vector<std::string> subscriber_changed;
    auto handle = ConfigReloadBus::instance().subscribe(
        [&](const Config& /*cfg*/, const ReloadReport& rep) {
            subscriber_changed = rep.changed_fields;
        });

    auto r = reload_config(cfg, d.path);
    REQUIRE(r.has_value());

    CHECK(contains(subscriber_changed, "api.default_model"));
    CHECK(contains(subscriber_changed, "api.temperature"));
    CHECK_FALSE(contains(subscriber_changed, "api.base_url"));
}
