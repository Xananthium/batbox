// src/tui/Changelog.cpp
// ---------------------------------------------------------------------------
// batbox::tui changelog parser and disk loader (TUI-FLOW-T10).
//
// parse_changelog() — converts Markdown text to vector<ChangelogEntry>.
// load_changelog()  — tries agentic/changelog.md then CHANGELOG.md.
// ---------------------------------------------------------------------------

#include "batbox/tui/Changelog.hpp"

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace batbox::tui {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

/// Trim leading and trailing ASCII whitespace from a string_view.
std::string_view trim_sv(std::string_view sv) {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' ||
                           sv.front() == '\r' || sv.front() == '\n')) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' ||
                           sv.back() == '\r' || sv.back() == '\n')) {
        sv.remove_suffix(1);
    }
    return sv;
}

/// Try to read a file from disk into a string.
/// Returns empty string on any I/O error.
std::string read_file(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) return {};

    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {};

    std::ostringstream buf;
    buf << f.rdbuf();
    return buf.str();
}

/// Return true if the line is a level-2 Markdown heading that looks like a
/// version header.  Accepted patterns:
///   ## v0.x.y - YYYY-MM-DD
///   ## v0.x.y
///   ## [0.x.y] - YYYY-MM-DD
///   ## [0.x.y]
///
/// On match, fills *version and *date (date may be empty).
bool try_parse_version_header(std::string_view line,
                               std::string& out_version,
                               std::string& out_date) {
    // Must start with "## "
    if (line.size() < 4 || line[0] != '#' || line[1] != '#' || line[2] != ' ') {
        return false;
    }
    line.remove_prefix(3);
    line = trim_sv(line);

    // Regex: optional 'v', optional brackets, then semver-like, then optional date.
    // We use std::regex for robustness; compiled once per call (small file context).
    static const std::regex kVersionRe(
        R"((?:v|\[)?(\d+\.\d+[\.\d]*)(?:\])?(?:\s*[-–]\s*(\d{4}-\d{2}-\d{2}))?.*)",
        std::regex::ECMAScript | std::regex::optimize);

    std::string line_str(line);
    std::smatch m;
    if (!std::regex_match(line_str, m, kVersionRe)) return false;

    // m[1] = version digits, m[2] = date (may be empty)
    out_version = m[1].str();
    out_date    = m.size() > 2 ? m[2].str() : "";
    return true;
}

/// Return true if the line is a bullet (starts with "- " or "* ").
/// On match, fills *out_bullet with the stripped text.
bool try_parse_bullet(std::string_view line, std::string& out_bullet) {
    line = trim_sv(line);
    if (line.size() >= 2 &&
        (line[0] == '-' || line[0] == '*') &&
        line[1] == ' ') {
        out_bullet = std::string(trim_sv(line.substr(2)));
        return true;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// parse_changelog
// ---------------------------------------------------------------------------

std::vector<ChangelogEntry> parse_changelog(std::string_view markdown) {
    std::vector<ChangelogEntry> entries;
    ChangelogEntry current;
    bool in_entry = false;

    // Iterate line-by-line
    std::size_t pos = 0;
    while (pos <= markdown.size()) {
        std::size_t nl = markdown.find('\n', pos);
        std::string_view line;
        if (nl == std::string_view::npos) {
            line = markdown.substr(pos);
            pos  = markdown.size() + 1;
        } else {
            line = markdown.substr(pos, nl - pos);
            pos  = nl + 1;
        }

        // Strip \r if present (Windows line endings)
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        std::string ver, date;
        if (try_parse_version_header(line, ver, date)) {
            // Commit the previous entry (if any).
            if (in_entry && !current.version.empty()) {
                entries.push_back(std::move(current));
            }
            current = ChangelogEntry{};
            current.version = ver;
            current.date    = date;
            in_entry = true;
            continue;
        }

        if (in_entry) {
            std::string bullet;
            if (try_parse_bullet(line, bullet) && !bullet.empty()) {
                current.bullets.push_back(std::move(bullet));
            }
            // Other lines (sub-headings, blank lines, prose paragraphs) are skipped.
        }
    }

    // Commit the last entry.
    if (in_entry && !current.version.empty()) {
        entries.push_back(std::move(current));
    }

    return entries;
}

// ---------------------------------------------------------------------------
// load_changelog
// ---------------------------------------------------------------------------

std::vector<ChangelogEntry> load_changelog(const std::filesystem::path& project_root) {
    // Try agentic/changelog.md first, then CHANGELOG.md at project root.
    const std::filesystem::path candidates[] = {
        project_root / "agentic" / "changelog.md",
        project_root / "CHANGELOG.md",
    };

    for (const auto& candidate : candidates) {
        std::string text = read_file(candidate);
        if (!text.empty()) {
            auto entries = parse_changelog(text);
            if (!entries.empty()) {
                return entries;
            }
        }
    }

    return {};
}

} // namespace batbox::tui
