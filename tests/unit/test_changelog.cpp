// tests/unit/test_changelog.cpp
// ---------------------------------------------------------------------------
// doctest suite for batbox::tui::parse_changelog and load_changelog.
// (TUI-FLOW-T10)
//
// Coverage:
//   1.  parse_changelog: basic version + date + bullets
//   2.  parse_changelog: bracket notation "## [0.1.0]"
//   3.  parse_changelog: missing date is tolerated
//   4.  parse_changelog: multiple version blocks, newest-first order preserved
//   5.  parse_changelog: empty input returns empty vector
//   6.  parse_changelog: malformed entry (no bullets) still parses version
//   7.  parse_changelog: "* " bullet prefix works same as "- "
//   8.  parse_changelog: lines that don't match are skipped
//   9.  parse_changelog: 3-entry file → 3 entries returned
//   10. load_changelog: returns empty vector when neither file exists
//   11. load_changelog: prefers agentic/changelog.md over CHANGELOG.md
//   12. load_changelog: falls back to CHANGELOG.md when agentic/changelog.md absent
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "batbox/tui/Changelog.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

/// Write text to a file, creating parent dirs as needed.
void write_file(const fs::path& path, std::string_view content) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << content;
}

} // namespace

// ---------------------------------------------------------------------------
// parse_changelog tests
// ---------------------------------------------------------------------------

TEST_CASE("parse_changelog: basic version + date + bullets") {
    std::string md =
        "## v0.2.0 - 2026-05-16\n"
        "\n"
        "- Feature A\n"
        "- Feature B\n";

    auto entries = batbox::tui::parse_changelog(md);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].version == "0.2.0");
    CHECK(entries[0].date    == "2026-05-16");
    REQUIRE(entries[0].bullets.size() >= 1);
    CHECK(entries[0].bullets[0] == "Feature A");
}

TEST_CASE("parse_changelog: bracket notation ## [0.1.0]") {
    std::string md =
        "## [0.1.0] - 2026-01-01\n"
        "- Initial release\n";

    auto entries = batbox::tui::parse_changelog(md);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].version == "0.1.0");
    CHECK(entries[0].date    == "2026-01-01");
    REQUIRE(!entries[0].bullets.empty());
    CHECK(entries[0].bullets[0] == "Initial release");
}

TEST_CASE("parse_changelog: missing date is tolerated") {
    std::string md =
        "## v1.0.0\n"
        "- No date\n";

    auto entries = batbox::tui::parse_changelog(md);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].version == "1.0.0");
    CHECK(entries[0].date.empty());
    REQUIRE(!entries[0].bullets.empty());
    CHECK(entries[0].bullets[0] == "No date");
}

TEST_CASE("parse_changelog: multiple version blocks, order preserved") {
    std::string md =
        "## v0.3.0 - 2026-06-01\n"
        "- Third\n"
        "\n"
        "## v0.2.0 - 2026-05-01\n"
        "- Second\n"
        "\n"
        "## v0.1.0 - 2026-04-01\n"
        "- First\n";

    auto entries = batbox::tui::parse_changelog(md);
    REQUIRE(entries.size() == 3);
    CHECK(entries[0].version == "0.3.0");
    CHECK(entries[1].version == "0.2.0");
    CHECK(entries[2].version == "0.1.0");
}

TEST_CASE("parse_changelog: empty input returns empty vector") {
    auto entries = batbox::tui::parse_changelog("");
    CHECK(entries.empty());
}

TEST_CASE("parse_changelog: version with no bullets still parses") {
    std::string md =
        "## v0.5.0 - 2026-03-01\n"
        "\n"
        "Some prose paragraph without bullets.\n";

    auto entries = batbox::tui::parse_changelog(md);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].version == "0.5.0");
    CHECK(entries[0].bullets.empty());
}

TEST_CASE("parse_changelog: asterisk bullet prefix") {
    std::string md =
        "## v0.4.0\n"
        "* Star bullet one\n"
        "* Star bullet two\n";

    auto entries = batbox::tui::parse_changelog(md);
    REQUIRE(entries.size() == 1);
    REQUIRE(entries[0].bullets.size() >= 2);
    CHECK(entries[0].bullets[0] == "Star bullet one");
    CHECK(entries[0].bullets[1] == "Star bullet two");
}

TEST_CASE("parse_changelog: non-matching lines are skipped") {
    std::string md =
        "# Changelog\n"
        "\n"
        "All notable changes are listed below.\n"
        "\n"
        "## v0.1.0 - 2026-01-01\n"
        "- Good bullet\n"
        "### Sub-heading (skipped)\n"
        "  indented prose (skipped)\n";

    auto entries = batbox::tui::parse_changelog(md);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].version == "0.1.0");
    REQUIRE(entries[0].bullets.size() == 1);
    CHECK(entries[0].bullets[0] == "Good bullet");
}

TEST_CASE("parse_changelog: 3-entry file returns 3 entries") {
    std::string md =
        "## v0.3.0 - 2026-03-01\n"
        "- C\n"
        "## v0.2.0 - 2026-02-01\n"
        "- B\n"
        "## v0.1.0 - 2026-01-01\n"
        "- A\n";

    auto entries = batbox::tui::parse_changelog(md);
    CHECK(entries.size() == 3);
}

// ---------------------------------------------------------------------------
// load_changelog tests
// ---------------------------------------------------------------------------

TEST_CASE("load_changelog: empty vector when neither file exists") {
    // Use a temp directory that definitely has no changelog files.
    auto tmp = fs::temp_directory_path() / "batbox_test_load_cl_empty";
    fs::create_directories(tmp);

    auto entries = batbox::tui::load_changelog(tmp);
    CHECK(entries.empty());

    fs::remove_all(tmp);
}

TEST_CASE("load_changelog: finds agentic/changelog.md") {
    auto tmp = fs::temp_directory_path() / "batbox_test_load_cl_agentic";
    fs::create_directories(tmp);

    const std::string content =
        "## v1.2.3 - 2026-05-16\n"
        "- Agentic changelog\n";

    write_file(tmp / "agentic" / "changelog.md", content);

    auto entries = batbox::tui::load_changelog(tmp);
    REQUIRE(!entries.empty());
    CHECK(entries[0].version == "1.2.3");

    fs::remove_all(tmp);
}

TEST_CASE("load_changelog: prefers agentic/changelog.md over CHANGELOG.md") {
    auto tmp = fs::temp_directory_path() / "batbox_test_load_cl_prefer";
    fs::create_directories(tmp);

    write_file(tmp / "agentic" / "changelog.md",
        "## v9.9.9 - 2026-05-16\n- Preferred\n");
    write_file(tmp / "CHANGELOG.md",
        "## v0.0.1 - 2026-01-01\n- Fallback\n");

    auto entries = batbox::tui::load_changelog(tmp);
    REQUIRE(!entries.empty());
    CHECK(entries[0].version == "9.9.9");  // From agentic/changelog.md

    fs::remove_all(tmp);
}

TEST_CASE("load_changelog: falls back to CHANGELOG.md when agentic absent") {
    auto tmp = fs::temp_directory_path() / "batbox_test_load_cl_fallback";
    fs::create_directories(tmp);

    // Only CHANGELOG.md exists (no agentic/changelog.md)
    write_file(tmp / "CHANGELOG.md",
        "## v3.0.0 - 2026-05-16\n- Fallback entry\n");

    auto entries = batbox::tui::load_changelog(tmp);
    REQUIRE(!entries.empty());
    CHECK(entries[0].version == "3.0.0");

    fs::remove_all(tmp);
}
