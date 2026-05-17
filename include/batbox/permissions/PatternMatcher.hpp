// include/batbox/permissions/PatternMatcher.hpp
// ---------------------------------------------------------------------------
// batbox::permissions::PatternMatcher — fnmatch-style glob + tool-arg matching.
//
// Implements the pattern syntax used in settings.json permissions arrays:
//
//   "Bash(npm test:*)"             — matches Bash with command starting "npm test:"
//   "Read(./src/**)"               — matches Read with any file under ./src/
//   "Write(./build/*)"             — matches Write with file directly under ./build/
//   "WebFetch(https://*.github.com/**)" — matches WebFetch HTTPS subdomain URLs
//
// Pattern format:  ToolName(arg-glob)
//   - ToolName is matched case-insensitively against the tool name.
//   - arg-glob is matched against the canonical argument string for that tool:
//       Bash        → args["command"]
//       Read        → args["file_path"]
//       Write       → args["file_path"]
//       Edit        → args["file_path"]
//       MultiEdit   → args["file_path"]
//       WebFetch    → args["url"]
//       WebSearch   → args["query"]
//       TodoWrite   → JSON-stringified args (fallback)
//       (unrecognised tools) → JSON-stringified args (fallback)
//
// Glob metacharacters:
//   *      match any characters except '/'
//   **     match any characters including '/'  (path-recursive)
//   ?      match exactly one character (any, including '/')
//   [abc]  match one character in the bracket set
//   [!abc] match one character NOT in the bracket set
//
// All other characters are matched literally.
//
// Free functions:
//   matches(rule, tool_name, args)     parse rule + evaluate in one call
//   parse_pattern_list(strings)        bulk-parse settings.json arrays
//   glob_match(pattern, text)          low-level glob predicate (exposed for tests)
//
// Struct:
//   ToolPattern                        parsed {tool_name, arg_glob} pair
//
// Build (standalone):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_pattern_matcher.cpp \
//       src/permissions/PatternMatcher.cpp \
//       -o /tmp/test_pm && /tmp/test_pm
// ---------------------------------------------------------------------------

#pragma once

#include <batbox/core/Json.hpp>
#include <batbox/core/Result.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace batbox::permissions {

// ===========================================================================
// ToolPattern — parsed representation of a single rule string
// ===========================================================================

/// A parsed permission pattern.  Constructed by splitting "Tool(arg-glob)" on
/// the first '('.  Both fields are stored normalised (tool_name lowercased).
struct ToolPattern {
    std::string tool_name;  ///< lower-cased, e.g. "bash", "read", "webfetch"
    std::string arg_glob;   ///< glob pattern applied to the canonical argument

    /// Parse a raw rule string such as "Bash(npm test:*)".
    ///
    /// Returns Err if:
    ///   - the string contains no '(' at all
    ///   - the string does not end with ')'
    ///   - the tool-name part is empty
    ///
    /// Blueprint contract name: ToolPattern constructor
    [[nodiscard]] static Result<ToolPattern, std::string>
    parse(std::string_view rule);
};

// ===========================================================================
// glob_match — low-level glob predicate
// ===========================================================================

/// Match `text` against `pattern` using the glob dialect described in the
/// file header:
///   '*'    any chars except '/'
///   '**'   any chars including '/'
///   '?'    exactly one char (any)
///   '[…]'  character class; '[!…]' negated class
///   other  literal match
///
/// Returns true iff the entire text is consumed by the pattern.
/// This function is exposed publicly so unit tests can exercise it directly.
///
/// Blueprint contract name: glob_match
[[nodiscard]] bool glob_match(std::string_view pattern, std::string_view text);

// ===========================================================================
// matches — top-level predicate (blueprint contract)
// ===========================================================================

/// Parse `rule` (e.g. "Bash(npm test:*)") and check whether it matches
/// the given tool invocation.
///
/// Returns false (not an error) when:
///   - the rule is malformed (tool name empty / missing parens)
///   - the tool name does not match
///   - the arg glob does not match the canonical argument
///
/// Returns true only when both the tool name and the arg glob match.
///
/// Canonical argument extraction per tool:
///   Bash / BashBackground  → args["command"]
///   Read                   → args["file_path"]
///   Write                  → args["file_path"]
///   Edit                   → args["file_path"]
///   MultiEdit              → args["file_path"]
///   WebFetch               → args["url"]
///   WebSearch              → args["query"]
///   (anything else)        → args.dump()  (JSON-stringified fallback)
///
/// Blueprint contract name: matches
[[nodiscard]] bool matches(std::string_view rule,
                           std::string_view tool_name,
                           const batbox::Json& args);

// ===========================================================================
// parse_pattern_list — bulk helper for settings.json arrays
// ===========================================================================

/// Parse an array of raw rule strings (as loaded from settings.json
/// "permissions.allow" / "permissions.deny" / "permissions.ask") into a
/// vector of ToolPattern objects.
///
/// Malformed entries are silently skipped (the vector simply omits them).
/// Callers that need to report bad entries should call ToolPattern::parse()
/// individually.
///
/// Blueprint contract name: parse_pattern_list
[[nodiscard]] std::vector<ToolPattern>
parse_pattern_list(const std::vector<std::string>& raw_rules);

} // namespace batbox::permissions
