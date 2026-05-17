// include/batbox/tui/Changelog.hpp
// ---------------------------------------------------------------------------
// Changelog parser and loader for BatBox splash "What's new" panel.
//
// Parses a Markdown changelog file into a vector of ChangelogEntry structs.
// The loader tries agentic/changelog.md first, then CHANGELOG.md at the
// project root. Returns an empty vector if neither file is found; the caller
// falls back to the hardcoded kChangelog array in splash_taglines.hpp.
//
// Markdown format recognised:
//   ## v0.x.y - YYYY-MM-DD     (canonical)
//   ## [0.x.y] - YYYY-MM-DD    (bracket notation)
//   ## v0.x.y                   (date optional)
//
// Each version block's bullet lines (starting with "- " or "* ") are
// collected as the 'bullets' vector.  The parser is forgiving: lines that
// do not match any known pattern are silently skipped.
//
// Blueprint contract (TUI-FLOW-T10):
//   struct  batbox::tui::ChangelogEntry
//   function batbox::tui::parse_changelog
//   function batbox::tui::load_changelog
// ---------------------------------------------------------------------------
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace batbox::tui {

/// One parsed version block from the changelog.
struct ChangelogEntry {
    std::string version;              ///< e.g. "v0.1.0" or "0.2.3"
    std::string date;                 ///< e.g. "2026-05-16" (empty if absent)
    std::vector<std::string> bullets; ///< Bullet-point lines (stripped of "- "/"* " prefix)
};

/// Parse a Markdown changelog string into a vector of ChangelogEntry.
///
/// Entries are returned newest-first (the order they appear in the file,
/// which by convention is newest at the top).
///
/// The parser tolerates:
///   - Missing date fields ("## v0.1.0" without a date)
///   - Bracket version notation ("## [0.1.0]")
///   - Both "- " and "* " bullet markers
///   - Blank lines between blocks (ignored)
///   - Lines that don't match any pattern (silently skipped)
///
/// @param markdown   Full text of the changelog file.
/// @returns Vector of ChangelogEntry, newest-first. Empty if no entries found.
std::vector<ChangelogEntry> parse_changelog(std::string_view markdown);

/// Load a changelog from disk.
///
/// Search order:
///   1. project_root / "agentic" / "changelog.md"
///   2. project_root / "CHANGELOG.md"
///
/// Returns an empty vector if neither file exists or if both are unreadable.
/// The caller is responsible for falling back to the hardcoded kChangelog array.
///
/// @param project_root  Root directory to search (usually batbox::paths::project_root()).
/// @returns Parsed entries newest-first, or empty on error / missing file.
std::vector<ChangelogEntry> load_changelog(const std::filesystem::path& project_root);

} // namespace batbox::tui
