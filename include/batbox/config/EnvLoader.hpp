// include/batbox/config/EnvLoader.hpp
// ---------------------------------------------------------------------------
// batbox::config::load_env_file — parse a .env file and return a key→value map.
//
// Supported syntax:
//   KEY=value               plain value (trailing whitespace stripped)
//   KEY="quoted value"      double-quoted: supports \n \t \\ \" escape seqs
//   KEY='literal value'     single-quoted: no escape processing
//   KEY=${VAR}              variable substitution from already-loaded process env
//   KEY=~/path              tilde expansion via batbox::paths::expand_tilde()
//   KEY=prefix${VAR}suffix  substitution anywhere in unquoted / double-quoted value
//   # comment               lines whose first non-whitespace char is '#' are skipped
//   export KEY=value        'export' keyword prefix is silently stripped
//
// BOM handling: a leading UTF-8 BOM (EF BB BF) on the first line is silently
// stripped so that Windows-authored .env files are accepted without error.
//
// Malformed lines (missing '=' separator, or invalid escape sequence) emit a
// warning to stderr and are skipped; parsing continues on the next line.
//
// Merge helpers:
//   merge_with_process_env(map, process_env_wins)
//     Merges the parsed map with the live process environment.
//     If process_env_wins == true  → process env values overwrite file values.
//     If process_env_wins == false → file values overwrite process env values.
//
//   get(map, key, default_value)
//     Typed lookup with a fallback default; returns std::string.
//
// Return value:
//   Result<EnvMap, std::string> — EnvMap on success, error message on failure
//   (the only hard error is a file that cannot be opened).
//
// ---------------------------------------------------------------------------
#pragma once

#include <batbox/core/Result.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

namespace batbox::config {

/// Alias for the key→value map produced by load_env_file().
using EnvMap = std::unordered_map<std::string, std::string>;

// ---------------------------------------------------------------------------
// load_env_file()
//
// Opens 'env_file', parses it line by line, and returns a populated EnvMap.
//
// Hard failure (Result is error):
//   - File cannot be opened (errno message is included in the error string).
//
// Soft failures (logged to stderr, line is skipped, parsing continues):
//   - Line has no '=' separator after the key token.
//   - Invalid escape sequence inside a double-quoted value.
//
// If 'env_file' does not exist the function returns an error Result; it is
// the caller's responsibility to decide whether a missing .env is fatal.
// ---------------------------------------------------------------------------
[[nodiscard]]
batbox::Result<EnvMap, std::string>
load_env_file(std::filesystem::path env_file);

// ---------------------------------------------------------------------------
// merge_with_process_env()
//
// Merges 'map' with the live process environment (environ / getenv).
//
// Parameters:
//   map               — the EnvMap to merge into (modified in place).
//   process_env_wins  — precedence flag:
//                         true  → existing process env entries WIN; any key
//                                 already present in the process env is NOT
//                                 overwritten by the file (use this for
//                                 ANTHROPIC_* compat where the shell must win).
//                         false → file entries WIN; the parsed value replaces
//                                 any same-named process env entry (use this
//                                 for BATBOX_* vars the user controls via .env).
//
// After this call 'map' contains the union of both sources, with precedence
// applied per 'process_env_wins'.
// ---------------------------------------------------------------------------
void merge_with_process_env(EnvMap& map, bool process_env_wins = false);

// ---------------------------------------------------------------------------
// get()
//
// Typed lookup helper: returns the value for 'key' from 'map', or
// 'default_value' if the key is absent.
// ---------------------------------------------------------------------------
[[nodiscard]]
std::string get(const EnvMap& map,
                std::string_view key,
                std::string_view default_value = "");

} // namespace batbox::config
