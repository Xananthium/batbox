// tests/unit/test_paths.cpp
// ---------------------------------------------------------------------------
// Unit tests for batbox::paths helpers (Paths.hpp / Paths.cpp).
//
// Build + run (standalone, no CMake needed):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_paths.cpp src/core/Paths.cpp \
//       -o /tmp/test_paths && /tmp/test_paths
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "batbox/core/Paths.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <fstream>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
namespace {

// RAII guard: set an environment variable for the duration of a test, then
// restore (or unset) it when the guard goes out of scope.
struct EnvGuard {
    std::string key;
    bool had_previous{false};
    std::string previous_value;

    explicit EnvGuard(const char* k, const char* v) : key{k} {
        const char* existing = std::getenv(k);
        if (existing != nullptr) {
            had_previous = true;
            previous_value = existing;
        }
        ::setenv(k, v, /*overwrite=*/1);
    }

    ~EnvGuard() {
        if (had_previous) {
            ::setenv(key.c_str(), previous_value.c_str(), 1);
        } else {
            ::unsetenv(key.c_str());
        }
    }

    // Non-copyable, non-movable.
    EnvGuard(const EnvGuard&) = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// home_dir()
// ---------------------------------------------------------------------------
TEST_SUITE("home_dir") {
    TEST_CASE("returns $HOME when set") {
        EnvGuard guard{"HOME", "/tmp/fake_home_for_test"};
        const auto p = batbox::paths::home_dir();
        CHECK(p == std::filesystem::path{"/tmp/fake_home_for_test"});
    }

    TEST_CASE("returns non-empty path when $HOME is unset (getpwuid fallback)") {
        // Unset HOME; getpwuid_r must still yield a directory.
        const char* original = std::getenv("HOME");
        std::string saved = original ? original : "";
        ::unsetenv("HOME");

        std::filesystem::path p;
        CHECK_NOTHROW(p = batbox::paths::home_dir());
        CHECK_FALSE(p.empty());
        CHECK(p.is_absolute());

        // Restore
        if (!saved.empty()) {
            ::setenv("HOME", saved.c_str(), 1);
        }
    }

    TEST_CASE("returned path is absolute") {
        const auto p = batbox::paths::home_dir();
        CHECK(p.is_absolute());
    }
}

// ---------------------------------------------------------------------------
// config_dir()
// ---------------------------------------------------------------------------
TEST_SUITE("config_dir") {
    TEST_CASE("honours BATBOX_CONFIG_DIR override") {
        EnvGuard cfg_guard{"BATBOX_CONFIG_DIR", "/tmp/my_batbox_cfg"};
        const auto p = batbox::paths::config_dir();
        CHECK(p == std::filesystem::path{"/tmp/my_batbox_cfg"});
    }

    TEST_CASE("defaults to ~/.batbox when BATBOX_CONFIG_DIR is not set") {
        // Make sure BATBOX_CONFIG_DIR is absent.
        const char* saved_cfg = std::getenv("BATBOX_CONFIG_DIR");
        std::string saved_cfg_str = saved_cfg ? saved_cfg : "";
        ::unsetenv("BATBOX_CONFIG_DIR");

        // Pin HOME so the expected path is deterministic.
        EnvGuard home_guard{"HOME", "/tmp/fake_home_default_cfg"};

        const auto p = batbox::paths::config_dir();
        CHECK(p == std::filesystem::path{"/tmp/fake_home_default_cfg/.batbox"});

        // Restore BATBOX_CONFIG_DIR if it existed before this test.
        if (!saved_cfg_str.empty()) {
            ::setenv("BATBOX_CONFIG_DIR", saved_cfg_str.c_str(), 1);
        }
    }

    TEST_CASE("returned path ends with .batbox when using default") {
        ::unsetenv("BATBOX_CONFIG_DIR");
        EnvGuard home_guard{"HOME", "/tmp/pathtest_home"};
        const auto p = batbox::paths::config_dir();
        CHECK(p.filename() == ".batbox");
    }
}

// ---------------------------------------------------------------------------
// expand_tilde()
// ---------------------------------------------------------------------------
TEST_SUITE("expand_tilde") {
    TEST_CASE("expands ~/foo to <home>/foo") {
        EnvGuard home_guard{"HOME", "/home/testuser"};
        const auto p = batbox::paths::expand_tilde("~/projects/batbox");
        CHECK(p == std::filesystem::path{"/home/testuser/projects/batbox"});
    }

    TEST_CASE("expands bare ~ to home_dir()") {
        EnvGuard home_guard{"HOME", "/home/testuser"};
        const auto p = batbox::paths::expand_tilde("~");
        CHECK(p == std::filesystem::path{"/home/testuser"});
    }

    TEST_CASE("leaves absolute paths unchanged") {
        const auto p = batbox::paths::expand_tilde("/usr/local/bin");
        CHECK(p == std::filesystem::path{"/usr/local/bin"});
    }

    TEST_CASE("leaves relative paths unchanged") {
        const auto p = batbox::paths::expand_tilde("relative/path");
        CHECK(p == std::filesystem::path{"relative/path"});
    }

    TEST_CASE("leaves empty string unchanged") {
        const auto p = batbox::paths::expand_tilde("");
        CHECK(p == std::filesystem::path{""});
    }

    TEST_CASE("does not expand ~username (other-user tilde)") {
        // We do not implement ~username expansion; return as-is.
        const auto p = batbox::paths::expand_tilde("~alice/docs");
        CHECK(p == std::filesystem::path{"~alice/docs"});
    }
}

// ---------------------------------------------------------------------------
// project_root()
// ---------------------------------------------------------------------------
TEST_SUITE("project_root") {
    TEST_CASE("returns cwd when called from the repo root (contains .git)") {
        // The test binary is built from the repo root which has a .git dir.
        // We check that project_root() returns a path that is an ancestor of
        // (or equal to) cwd and contains .git.
        const auto root = batbox::paths::project_root();
        CHECK(std::filesystem::exists(root / ".git"));
    }

    TEST_CASE("returned path is absolute") {
        const auto root = batbox::paths::project_root();
        CHECK(root.is_absolute());
    }

    TEST_CASE("returns cwd when no .git or BATBOX.md marker found") {
        // Create a temporary directory tree with no markers, chdir there,
        // run project_root(), then chdir back.
        namespace fs = std::filesystem;

        const fs::path tmp_dir = fs::temp_directory_path() / "batbox_test_no_marker";
        const fs::path nested  = tmp_dir / "a" / "b" / "c";
        fs::create_directories(nested);

        // Save and change cwd.
        const fs::path original_cwd = fs::current_path();
        fs::current_path(nested);

        const auto root = batbox::paths::project_root();

        // Restore cwd.
        fs::current_path(original_cwd);

        // Without any marker, project_root() must fall back to the cwd at the
        // time it was called — which was `nested`.
        // Use canonical() to resolve symlinks (e.g. /var -> /private/var on macOS).
        CHECK(root == fs::canonical(nested));

        // Cleanup.
        fs::remove_all(tmp_dir);
    }

    TEST_CASE("finds BATBOX.md marker when present") {
        namespace fs = std::filesystem;

        // Create a temp tree: <tmp>/marker_root/ (has BATBOX.md)
        //                                        subdir/
        const fs::path tmp_dir    = fs::temp_directory_path() / "batbox_test_marker";
        const fs::path marker_dir = tmp_dir / "marker_root";
        const fs::path sub_dir    = marker_dir / "subdir";
        fs::create_directories(sub_dir);

        // Place the BATBOX.md marker in marker_root.
        {
            std::ofstream marker_file{(marker_dir / "BATBOX.md").string()};
            marker_file << "# BatBox project\n";
        }

        const fs::path original_cwd = fs::current_path();
        fs::current_path(sub_dir);

        const auto root = batbox::paths::project_root();

        fs::current_path(original_cwd);

        // Use canonical() to resolve symlinks (e.g. /var -> /private/var on macOS).
        CHECK(root == fs::canonical(marker_dir));

        // Cleanup.
        fs::remove_all(tmp_dir);
    }
}
