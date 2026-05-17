// include/batbox/config/StateStore.hpp
// ---------------------------------------------------------------------------
// Lightweight key-value state persistence in ~/.batbox/state.json.
//
// Provides a minimal interface for reading and writing simple string values
// that need to persist across BatBox sessions.  Uses nlohmann::json for
// reading and writing.  The state file is created on first write if it does
// not exist; its parent directory is created if needed.
//
// Thread safety: NOT thread-safe.  All reads and writes must occur from the
// same thread (typically the main thread before and after the FTXUI loop).
//
// Current keys:
//   "last_seen_changelog_version"  — newest version string shown to the user.
//
// Blueprint contract (TUI-FLOW-T10):
//   function  batbox::config::read_last_seen_changelog_version
//   function  batbox::config::write_last_seen_changelog_version
// ---------------------------------------------------------------------------
#pragma once

#include <optional>
#include <string>

namespace batbox::config {

/// Read the "last_seen_changelog_version" key from ~/.batbox/state.json.
///
/// Returns std::nullopt if the file does not exist, the key is absent, or
/// any I/O / parse error occurs.
std::optional<std::string> read_last_seen_changelog_version();

/// Write the "last_seen_changelog_version" key to ~/.batbox/state.json.
///
/// Creates the file (and its parent directory ~/.batbox/) if they do not
/// exist.  If the file already contains other keys they are preserved.
/// Errors are silently ignored (this is best-effort state persistence).
///
/// @param version  The version string to persist, e.g. "0.1.0".
void write_last_seen_changelog_version(std::string version);

} // namespace batbox::config
