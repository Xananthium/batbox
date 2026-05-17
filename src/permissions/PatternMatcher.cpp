// src/permissions/PatternMatcher.cpp
// ---------------------------------------------------------------------------
// Implementation of PatternMatcher — fnmatch-style glob + tool-arg matching.
// See include/batbox/permissions/PatternMatcher.hpp for the full contract.
// ---------------------------------------------------------------------------

#include <batbox/permissions/PatternMatcher.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace batbox::permissions {

// ===========================================================================
// Internal helpers
// ===========================================================================

namespace {

/// ASCII-only char lowercasing (no locale dependency).
constexpr char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

/// Return a lower-cased copy of s (ASCII only — sufficient for tool names).
std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(ascii_lower(c));
    return out;
}

/// Match a single bracket expression "[…]" starting at pat[0] == '['.
/// Advances pat and text past the match.
/// Returns true if text[0] was in the set; false if not (or pattern broken).
/// On exit, `pat` points one past the closing ']', `text` one past the matched char.
/// If the bracket expression is malformed (no closing ']'), treats '[' literally.
bool match_bracket(std::string_view& pat, std::string_view& text) {
    if (pat.empty() || pat[0] != '[') return false;
    if (text.empty()) return false;

    const char ch = text[0];
    pat.remove_prefix(1);  // consume '['

    bool negate = false;
    if (!pat.empty() && (pat[0] == '!' || pat[0] == '^')) {
        negate = true;
        pat.remove_prefix(1);
    }

    // Allow ']' as first char in set (POSIX extension)
    bool matched = false;
    bool closed = false;
    std::string_view saved_pat = pat;  // for rollback on broken bracket

    while (!pat.empty()) {
        if (pat[0] == ']') {
            closed = true;
            pat.remove_prefix(1);  // consume ']'
            break;
        }
        // Range check: a-z
        if (pat.size() >= 3 && pat[1] == '-' && pat[2] != ']') {
            char lo = pat[0];
            char hi = pat[2];
            if (ch >= lo && ch <= hi) matched = true;
            pat.remove_prefix(3);
        } else {
            if (ch == pat[0]) matched = true;
            pat.remove_prefix(1);
        }
    }

    if (!closed) {
        // Malformed bracket — restore and treat '[' as literal
        pat = saved_pat;
        // undo negate prefix restore (we already removed it above)
        // Simplest: return false here; caller will fall through to literal '[' match
        return false;
    }

    if (negate) matched = !matched;
    if (matched) text.remove_prefix(1);
    return matched;
}

/// Recursive glob matcher core.
/// pattern and text are both passed by value (cheap string_view copies).
bool glob_match_impl(std::string_view pattern, std::string_view text) {
    while (!pattern.empty()) {
        // ------------------------------------------------------------------
        // ** — path-recursive wildcard (matches everything including '/')
        // ------------------------------------------------------------------
        if (pattern.size() >= 2 && pattern[0] == '*' && pattern[1] == '*') {
            // Consume all leading '**' sequences (/**/ etc.)
            std::string_view rest = pattern.substr(2);
            // Skip any trailing '/' after the '**'
            if (!rest.empty() && rest[0] == '/') rest.remove_prefix(1);

            // '**' with nothing left matches everything
            if (rest.empty()) return true;

            // Try matching `rest` at every position in text (greedy backtrack)
            // Position 0 is "consume nothing"
            for (std::size_t i = 0; i <= text.size(); ++i) {
                if (glob_match_impl(rest, text.substr(i))) return true;
            }
            return false;
        }

        // ------------------------------------------------------------------
        // * — single-component wildcard (does NOT cross '/')
        // ------------------------------------------------------------------
        if (pattern[0] == '*') {
            std::string_view rest = pattern.substr(1);

            // '*' at end of pattern: match rest of text that contains no '/'
            if (rest.empty()) {
                return text.find('/') == std::string_view::npos;
            }

            // Try every non-'/' prefix of text
            for (std::size_t i = 0; i <= text.size(); ++i) {
                if (i > 0 && text[i - 1] == '/') break;  // stop at '/'
                if (glob_match_impl(rest, text.substr(i))) return true;
            }
            return false;
        }

        // ------------------------------------------------------------------
        // ? — match exactly one character (any, does cross '/')
        // ------------------------------------------------------------------
        if (pattern[0] == '?') {
            if (text.empty()) return false;
            pattern.remove_prefix(1);
            text.remove_prefix(1);
            continue;
        }

        // ------------------------------------------------------------------
        // [abc] / [!abc] — bracket expression
        // ------------------------------------------------------------------
        if (pattern[0] == '[') {
            std::string_view pat_copy = pattern;
            std::string_view text_copy = text;
            if (match_bracket(pat_copy, text_copy)) {
                pattern = pat_copy;
                text = text_copy;
                continue;
            }
            // Bracket was malformed — fall through to literal '[' match below
        }

        // ------------------------------------------------------------------
        // Literal character match
        // ------------------------------------------------------------------
        if (text.empty() || pattern[0] != text[0]) return false;
        pattern.remove_prefix(1);
        text.remove_prefix(1);
    }

    return text.empty();
}

/// Extract the canonical argument string from a tool invocation.
/// See the header for the tool→field mapping.
std::string extract_arg(std::string_view tool_lower, const batbox::Json& args) {
    // Helper: get a string field from args, return "" if absent or not string
    auto get_str = [&](std::string_view key) -> std::string {
        try {
            auto it = args.find(key);
            if (it != args.end() && it->is_string()) {
                return it->get<std::string>();
            }
        } catch (...) {}
        return {};
    };

    if (tool_lower == "bash" || tool_lower == "bashbackground") {
        return get_str("command");
    }
    if (tool_lower == "read"
        || tool_lower == "write"
        || tool_lower == "edit"
        || tool_lower == "multiedit") {
        return get_str("file_path");
    }
    if (tool_lower == "webfetch") {
        return get_str("url");
    }
    if (tool_lower == "websearch") {
        return get_str("query");
    }

    // Fallback: JSON-stringify the entire args object
    try {
        return args.dump();
    } catch (...) {
        return {};
    }
}

} // anonymous namespace

// ===========================================================================
// ToolPattern::parse
// ===========================================================================

Result<ToolPattern, std::string> ToolPattern::parse(std::string_view rule) {
    // Find the first '('
    const auto paren_pos = rule.find('(');
    if (paren_pos == std::string_view::npos) {
        return Err(std::string("PatternMatcher: missing '(' in rule: '")
                   + std::string(rule) + "'");
    }

    const std::string_view tool_sv = rule.substr(0, paren_pos);
    if (tool_sv.empty()) {
        return Err(std::string("PatternMatcher: empty tool name in rule: '")
                   + std::string(rule) + "'");
    }

    // The body must end with ')'
    if (rule.back() != ')') {
        return Err(std::string("PatternMatcher: rule does not end with ')': '")
                   + std::string(rule) + "'");
    }

    // arg_glob is everything between '(' and the final ')'
    const std::string_view arg_sv = rule.substr(paren_pos + 1,
                                                 rule.size() - paren_pos - 2);

    return ToolPattern{to_lower(tool_sv), std::string(arg_sv)};
}

// ===========================================================================
// glob_match — public entry point
// ===========================================================================

bool glob_match(std::string_view pattern, std::string_view text) {
    return glob_match_impl(pattern, text);
}

// ===========================================================================
// matches — top-level predicate (blueprint contract)
// ===========================================================================

bool matches(std::string_view rule,
             std::string_view tool_name,
             const batbox::Json& args) {
    // Parse the rule; return false (not an error) on malformed input
    auto parsed = ToolPattern::parse(rule);
    if (!parsed) return false;

    // Tool-name comparison is case-insensitive
    const std::string tool_lower = to_lower(tool_name);
    if (parsed->tool_name != tool_lower) return false;

    // Extract the canonical argument string for this tool
    const std::string arg = extract_arg(tool_lower, args);

    // Apply the glob
    return glob_match(parsed->arg_glob, arg);
}

// ===========================================================================
// parse_pattern_list — bulk helper
// ===========================================================================

std::vector<ToolPattern>
parse_pattern_list(const std::vector<std::string>& raw_rules) {
    std::vector<ToolPattern> result;
    result.reserve(raw_rules.size());

    for (const auto& rule : raw_rules) {
        auto parsed = ToolPattern::parse(rule);
        if (parsed) {
            result.push_back(std::move(*parsed));
        }
        // Malformed entries are silently skipped
    }

    return result;
}

} // namespace batbox::permissions
