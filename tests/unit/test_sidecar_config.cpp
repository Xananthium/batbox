// tests/unit/test_sidecar_config.cpp
// ---------------------------------------------------------------------------
// doctest suite for Task 24 — sidecar python_bin auto-resolution from venv.
//
// Tests the Approach A fix in Config.cpp apply_env():
//   - Default "python3" + venv exists  → resolved to absolute venv path
//   - Default "python3" + venv absent  → stays "python3" (no false override)
//   - Explicit BATBOX_SIDECAR_PYTHON   → respected regardless of venv
//
// All tests use BATBOX_CONFIG_DIR to redirect the config_dir() lookup to a
// temporary directory.  No test depends on the real ~/.batbox installation.
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/config/Config.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using namespace batbox::config;

namespace {

// ---------------------------------------------------------------------------
// TmpDir — RAII temporary directory helper.
// Creates a unique directory under std::filesystem::temp_directory_path()
// and removes it (recursively) on destruction.
// ---------------------------------------------------------------------------
struct TmpDir {
    std::filesystem::path path;

    TmpDir() {
        // Build a unique path using the pid + a counter.
        static int counter = 0;
        path = std::filesystem::temp_directory_path() /
               ("batbox_sidecar_cfg_test_" +
                std::to_string(static_cast<int>(::getpid())) + "_" +
                std::to_string(++counter));
        std::filesystem::create_directories(path);
    }

    ~TmpDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        // Intentionally ignore errors in destructor — best effort cleanup.
    }

    // Disallow copy/move to keep ownership clear.
    TmpDir(const TmpDir&)            = delete;
    TmpDir& operator=(const TmpDir&) = delete;
};

// Create an empty regular file (and all parent directories) at the given path.
void touch_file(const std::filesystem::path& p) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p);
    // file closes on destruction — empty file is sufficient for exists() test
}

} // anonymous namespace

// ============================================================================
// SUITE: Sidecar python_bin auto-resolution
// ============================================================================
TEST_SUITE("Config sidecar python_bin — venv auto-resolution (Task 24)") {

    // -------------------------------------------------------------------------
    // Case 1: default "python3" + venv python exists → resolved to absolute path
    // -------------------------------------------------------------------------
    TEST_CASE("venv_exists: default python3 is resolved to absolute venv path") {
        TmpDir tmp;

        // Create the fake venv python file so std::filesystem::exists() returns true.
        const std::filesystem::path venv_python =
            tmp.path / "sidecar" / ".venv" / "bin" / "python3";
        touch_file(venv_python);

        // Point BATBOX_CONFIG_DIR at our temp dir so config_dir() returns it.
        EnvMap env{
            {"BATBOX_CONFIG_DIR", tmp.path.string()},
        };

        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());

        const std::string resolved = r->sidecar.python.string();

        // Must be the absolute venv path, not the bare "python3" token.
        CHECK(resolved == venv_python.string());
        // Must be absolute.
        CHECK(r->sidecar.python.is_absolute());
    }

    // -------------------------------------------------------------------------
    // Case 2: default "python3" + venv absent → python stays "python3"
    // -------------------------------------------------------------------------
    TEST_CASE("venv_missing: default python3 is kept when venv does not exist") {
        TmpDir tmp;
        // Do NOT create the venv directory — it must be absent.

        EnvMap env{
            {"BATBOX_CONFIG_DIR", tmp.path.string()},
        };

        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());

        // Should remain the bare "python3" default.
        CHECK(r->sidecar.python.string() == "python3");
    }

    // -------------------------------------------------------------------------
    // Case 3: explicit BATBOX_SIDECAR_PYTHON → not overridden by venv auto-resolve
    // -------------------------------------------------------------------------
    TEST_CASE("explicit_env: BATBOX_SIDECAR_PYTHON beats venv auto-resolution") {
        TmpDir tmp;

        // Create the venv python so the auto-resolution would fire IF it weren't
        // for the explicit env var.
        const std::filesystem::path venv_python =
            tmp.path / "sidecar" / ".venv" / "bin" / "python3";
        touch_file(venv_python);

        EnvMap env{
            {"BATBOX_CONFIG_DIR",      tmp.path.string()},
            {"BATBOX_SIDECAR_PYTHON",  "/usr/bin/python3.11"},
        };

        auto r = Config::load_from_env(env);
        REQUIRE(r.has_value());

        // The explicit override must win — not the auto-resolved venv path.
        CHECK(r->sidecar.python.string() == "/usr/bin/python3.11");
    }
}
