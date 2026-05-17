// tests/integration/test_migrate.cpp
// =============================================================================
// Integration tests for batbox::cmd::run_migrate
//
// Strategy:
//   - Build a temporary source tree under /tmp/batbox_test_migrate_src_<pid>/
//     with a subset of the claude-code config layout:
//       settings.json
//       CLAUDE.md
//       keybindings.json
//       mcp.json
//       history
//       .env
//       sessions/ (empty dir)
//       plugins/myplugin.json
//       skills/ (empty dir)
//       agents/ (empty dir)
//   - Use a temporary destination under /tmp/batbox_test_migrate_dst_<pid>/
//   - Run dry-run first: verify destination is unchanged.
//   - Run --apply: verify files appear in the correct destination paths.
//   - Run --apply again without --force: verify existing files are not
//     overwritten (size/content preserved).
//   - Run --apply --force: verify overwrite succeeds.
//   - Verify CLAUDE.md was copied as BATBOX.md (rename mapping).
//   - Verify plugins/ directory was recursively copied.
//   - Cleanup both temp dirs after each test.
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "../../src/migrate.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string pid_suffix() {
    return std::to_string(static_cast<long>(::getpid()));
}

/// Write content to a file, creating parent dirs as needed.
static void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::out | std::ios::trunc);
    f << content;
}

/// Read entire file to string.
static std::string read_file(const fs::path& p) {
    std::ifstream f(p);
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

/// RAII guard that removes a directory on destruction.
struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& suffix)
        : path(fs::temp_directory_path() / ("batbox_migrate_test_" + suffix))
    {
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec); // ignore errors on cleanup
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

/// Build a representative source tree inside dir.
static void populate_source(const fs::path& dir) {
    write_file(dir / "settings.json",    R"({"theme":"miss-kittin"})");
    write_file(dir / "CLAUDE.md",        "# Claude memory\nsome notes\n");
    write_file(dir / "keybindings.json", R"({"ctrl-r":"history"})");
    write_file(dir / "mcp.json",         R"({"mcpServers":{}})");
    write_file(dir / "history",          "previous command\n");
    write_file(dir / ".env",             "BATBOX_API_KEY=test\n");
    // sessions/ directory (empty)
    fs::create_directories(dir / "sessions");
    // plugins/ directory with a file
    write_file(dir / "plugins" / "myplugin.json", R"({"name":"myplugin"})");
    // skills/ directory (empty)
    fs::create_directories(dir / "skills");
    // agents/ directory (empty)
    fs::create_directories(dir / "agents");
}

// ---------------------------------------------------------------------------
// TEST CASES
// ---------------------------------------------------------------------------

TEST_CASE("dry-run does not write any files") {
    const std::string suffix = "dry_" + pid_suffix();
    TempDir src("src_" + suffix);
    TempDir dst("dst_" + suffix);

    populate_source(src.path);

    batbox::cmd::MigrateArgs args;
    args.apply    = false; // dry-run (default)
    args.force    = false;
    args.from_dir = src.path.string();
    args.to_dir   = dst.path.string();

    int rc = batbox::cmd::run_migrate(args);
    CHECK(rc == 0);

    // Destination should be empty (only the dir itself exists).
    int file_count = 0;
    for ([[maybe_unused]] const auto& e : fs::recursive_directory_iterator(dst.path)) {
        ++file_count;
    }
    CHECK(file_count == 0);
}

TEST_CASE("--apply copies all mapped files and dirs") {
    const std::string suffix = "apply_" + pid_suffix();
    TempDir src("src_" + suffix);
    TempDir dst("dst_" + suffix);

    populate_source(src.path);

    batbox::cmd::MigrateArgs args;
    args.apply    = true;
    args.force    = false;
    args.from_dir = src.path.string();
    args.to_dir   = dst.path.string();

    int rc = batbox::cmd::run_migrate(args);
    CHECK(rc == 0);

    // Plain file copies.
    CHECK(fs::exists(dst.path / "settings.json"));
    CHECK(fs::exists(dst.path / "keybindings.json"));
    CHECK(fs::exists(dst.path / "mcp.json"));
    CHECK(fs::exists(dst.path / "history"));
    CHECK(fs::exists(dst.path / ".env"));

    // CLAUDE.md must be renamed to BATBOX.md.
    CHECK_FALSE(fs::exists(dst.path / "CLAUDE.md"));
    CHECK(fs::exists(dst.path / "BATBOX.md"));

    // Content preserved for rename.
    CHECK(read_file(dst.path / "BATBOX.md") == "# Claude memory\nsome notes\n");

    // plugins/ recursively copied.
    CHECK(fs::exists(dst.path / "plugins" / "myplugin.json"));
    CHECK(read_file(dst.path / "plugins" / "myplugin.json") == R"({"name":"myplugin"})");

    // sessions/ and agents/ dirs created (even if empty).
    CHECK(fs::is_directory(dst.path / "sessions"));
    CHECK(fs::is_directory(dst.path / "agents"));
}

TEST_CASE("--apply without --force does not overwrite existing files") {
    const std::string suffix = "noforce_" + pid_suffix();
    TempDir src("src_" + suffix);
    TempDir dst("dst_" + suffix);

    populate_source(src.path);

    // Pre-write a different settings.json in the destination.
    write_file(dst.path / "settings.json", R"({"existing":"value"})");

    batbox::cmd::MigrateArgs args;
    args.apply    = true;
    args.force    = false;
    args.from_dir = src.path.string();
    args.to_dir   = dst.path.string();

    int rc = batbox::cmd::run_migrate(args);
    CHECK(rc == 0);

    // Original destination content must be preserved.
    CHECK(read_file(dst.path / "settings.json") == R"({"existing":"value"})");
}

TEST_CASE("--apply --force overwrites existing files") {
    const std::string suffix = "force_" + pid_suffix();
    TempDir src("src_" + suffix);
    TempDir dst("dst_" + suffix);

    populate_source(src.path);

    // Pre-write stale settings.json in the destination.
    write_file(dst.path / "settings.json", R"({"stale":"data"})");

    batbox::cmd::MigrateArgs args;
    args.apply    = true;
    args.force    = true;
    args.from_dir = src.path.string();
    args.to_dir   = dst.path.string();

    int rc = batbox::cmd::run_migrate(args);
    CHECK(rc == 0);

    // Destination settings.json must now have the source content.
    CHECK(read_file(dst.path / "settings.json") == R"({"theme":"miss-kittin"})");
}

TEST_CASE("absent source directory is handled gracefully") {
    const std::string suffix = "absent_" + pid_suffix();
    TempDir dst("dst_" + suffix);

    batbox::cmd::MigrateArgs args;
    args.apply    = true;
    args.force    = false;
    args.from_dir = "/tmp/batbox_definitely_does_not_exist_" + pid_suffix();
    args.to_dir   = dst.path.string();

    int rc = batbox::cmd::run_migrate(args);
    CHECK(rc == 0); // graceful: nothing to migrate is not an error
}

TEST_CASE("source files absent from claude dir are reported but not an error") {
    const std::string suffix = "partial_" + pid_suffix();
    TempDir src("src_" + suffix);
    TempDir dst("dst_" + suffix);

    // Only write a subset of the expected files.
    write_file(src.path / "settings.json", R"({"partial":"true"})");
    // CLAUDE.md, keybindings.json, mcp.json, etc. are intentionally absent.

    batbox::cmd::MigrateArgs args;
    args.apply    = true;
    args.force    = false;
    args.from_dir = src.path.string();
    args.to_dir   = dst.path.string();

    int rc = batbox::cmd::run_migrate(args);
    CHECK(rc == 0);

    CHECK(fs::exists(dst.path / "settings.json"));
    // BATBOX.md should NOT exist since CLAUDE.md was absent in source.
    CHECK_FALSE(fs::exists(dst.path / "BATBOX.md"));
}
