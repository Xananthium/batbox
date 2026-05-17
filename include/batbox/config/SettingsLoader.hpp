// include/batbox/config/SettingsLoader.hpp
// ---------------------------------------------------------------------------
// batbox::config::Settings + load_settings() / write_settings()
//
// Settings is a lightweight struct that represents the user-editable subset of
// ~/.batbox/settings.json.  It deliberately does NOT duplicate the full Config
// aggregate — its job is:
//
//   1. Be the sole read/write interface for the on-disk file.
//   2. Expose the permission arrays that the PermissionStore (CPP 12.1) needs
//      to persist allow/deny/ask rules without touching Config.
//   3. Expose theme, plugins.disabled, and output_style so those commands can
//      persist user choices without re-parsing the entire env stack.
//
// JSON shape of ~/.batbox/settings.json (all keys optional):
// {
//   "permissions": {
//     "allow": ["Bash(git *)", "Read(./src/**)"],
//     "deny":  ["Bash(rm -rf *)"],
//     "ask":   []
//   },
//   "theme": "miss-kittin",
//   "plugins": {
//     "disabled": ["plugin-name-1"]
//   },
//   "output_style": "default"
// }
//
// Loading pipeline (Config::load integrates this layer):
//   load_settings(path)  — returns Settings with defaults when file is missing.
//   write_settings(path, s) — atomic write: write to path.tmp then rename.
//
// Precedence: env vars always override settings.json values at the Config
// aggregate level.  Settings itself is just the file layer.
//
// Error handling:
//   load_settings:
//     - Missing file        → ok, returns Settings{} defaults (not an error)
//     - Cannot open file    → Err(message)
//     - Malformed JSON      → Err(nlohmann parse_error::what() — includes byte offset)
//   write_settings:
//     - Cannot open tmp     → Err(message)
//     - Rename fails        → Err(message with errno)
// ---------------------------------------------------------------------------
#pragma once

#include <batbox/core/Result.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace batbox::config {

// ============================================================================
// Settings struct
// ============================================================================

/// User-editable settings persisted in ~/.batbox/settings.json.
///
/// All fields default to empty / neutral values.  load_settings() returns
/// this struct even when the file does not exist so callers never need to
/// special-case a missing file.
struct Settings {
    // ---- permissions --------------------------------------------------------
    /// Patterns in permissions.allow — auto-approved without prompt.
    /// Example: "Bash(git *)", "Read(./src/**)"
    std::vector<std::string> permissions_allow;

    /// Patterns in permissions.deny — always blocked.
    std::vector<std::string> permissions_deny;

    /// Patterns in permissions.ask — always prompt (overrides allow).
    std::vector<std::string> permissions_ask;

    // ---- theme --------------------------------------------------------------
    /// UI theme name as a raw string (e.g. "miss-kittin").
    /// Empty string → use built-in default ("miss-kittin").
    std::string theme;

    // ---- plugins ------------------------------------------------------------
    /// Names of disabled plugins (plugins.disabled array in JSON).
    std::vector<std::string> plugins_disabled;

    // ---- output_style -------------------------------------------------------
    /// Output-style hint for the renderer.
    /// Valid values: "default", "compact", "verbose".
    /// Empty string → "default".
    std::string output_style;
};

// ============================================================================
// load_settings()
// ============================================================================

/// Read and parse ~/.batbox/settings.json (or any path the caller supplies).
///
/// Return:
///   Ok(Settings{defaults})   — file is missing (this is NOT an error).
///   Ok(Settings{...})        — file was found and parsed successfully.
///   Err(std::string)         — file exists but cannot be opened, or the JSON
///                              is malformed (includes nlohmann byte-offset).
[[nodiscard]]
batbox::Result<Settings, std::string>
load_settings(std::filesystem::path path);

// ============================================================================
// write_settings()
// ============================================================================

/// Atomically write 's' to 'path' as JSON.
///
/// The write sequence is:
///   1. Open path + ".tmp" for writing (truncate).
///   2. Serialise Settings to pretty-printed JSON.
///   3. Flush and close the tmp file.
///   4. std::filesystem::rename(tmp, path)  — POSIX atomic on same filesystem.
///
/// Return:
///   Ok()        — success.
///   Err(msg)    — tmp file could not be created, or rename failed.
[[nodiscard]]
batbox::Result<void, std::string>
write_settings(std::filesystem::path path, const Settings& s);

} // namespace batbox::config
