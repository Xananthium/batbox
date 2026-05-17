// include/batbox/plugins/FrontmatterParser.hpp
// =============================================================================
// batbox::plugins::parse_frontmatter — minimal hand-rolled YAML frontmatter
// parser for skill/agent/command .md files.
//
// Design rationale (per ned-cpp.md §2.C11):
//   The subset of YAML actually consumed by claude-code skills/agents/commands
//   is tiny: key:value scalar pairs, simple flow-style lists [a, b, c],
//   block-style lists (- item), quoted and unquoted strings, integers, and
//   booleans.  No anchors, no aliases, no multi-line scalars, no flow maps.
//   A ~120-LOC hand-rolled parser covers the full subset and avoids pulling
//   yaml-cpp or rapidyaml as vcpkg deps (saves build time + binary size).
//
// Return type:
//   Result<std::pair<Frontmatter, std::string>>
//
//   where:
//     Frontmatter = std::unordered_map<std::string, Json>
//     std::string = the markdown body after the closing "---" delimiter
//
//   Values in the map follow nlohmann::json semantics:
//     - Unquoted booleans (true/false/yes/no)    → Json boolean
//     - Integer literals                          → Json integer (int64_t)
//     - Flow-style lists  [a, b, c]               → Json array of strings
//     - Block-style lists  (- item per line)      → Json array of strings
//     - Everything else                           → Json string (quotes stripped)
//
// Behaviour when no leading "---\n" is present:
//   Returns Ok with an empty Frontmatter map and the whole content as the body.
//
// Behaviour on malformed frontmatter:
//   Returns Err with a human-readable "line:col: <message>" error string.
//   Callers (PluginLoader) should log the error, skip the file, and continue.
//
// Build standalone (from repo root):
//   c++ -std=c++20 -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_frontmatter_parser.cpp \
//       src/plugins/FrontmatterParser.cpp \
//       -o /tmp/test_frontmatter_parser && /tmp/test_frontmatter_parser
// =============================================================================

#pragma once

#include <batbox/core/Json.hpp>
#include <batbox/core/Result.hpp>

#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace batbox::plugins {

// ---------------------------------------------------------------------------
// Type aliases
// ---------------------------------------------------------------------------

/// A parsed frontmatter block: map from key to JSON-typed value.
/// Values are one of: string, integer (int64_t), boolean, or array of strings.
using Frontmatter = std::unordered_map<std::string, Json>;

/// The return type of parse_frontmatter.
/// .first  = frontmatter key-value map (empty when no --- block present)
/// .second = markdown body text (everything after the closing ---, or the
///           entire content when no frontmatter block exists)
using FrontmatterResult = std::pair<Frontmatter, std::string>;

// ---------------------------------------------------------------------------
// Primary API
// ---------------------------------------------------------------------------

/// Parse YAML frontmatter from a markdown file's content.
///
/// @param md_content  Full text of the .md file (may be a string_view into
///                    a memory-mapped or in-memory buffer).
/// @returns           Ok(FrontmatterResult) on success;
///                    Err(std::string) with "line:col: message" on malformed
///                    frontmatter.
///
/// The function recognises exactly the YAML subset used by claude-code:
///   key: value            — unquoted scalar (string, int64, bool)
///   key: "quoted value"   — double-quoted string (inner "" escape supported)
///   key: [a, b, c]        — flow-style list of string items
///   key:                  — followed by "  - item" lines → block-style list
///     - item
///     - item
///
/// Boolean coercion:  "true" "yes"  → true;  "false" "no"  → false.
/// Integer coercion:  any token matching [-+]?[0-9]+ → int64_t.
/// All other tokens are treated as strings.
///
/// Keys must match [A-Za-z0-9_-]+ (no whitespace, no colons).
/// Duplicate keys: last value wins (matches standard YAML merge semantics).
[[nodiscard]] Result<FrontmatterResult>
parse_frontmatter(std::string_view md_content);

} // namespace batbox::plugins
