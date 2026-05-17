// src/plugins/FrontmatterParser.cpp
// =============================================================================
// Implementation of batbox::plugins::parse_frontmatter.
//
// Hand-rolled minimal YAML frontmatter parser (~120 LOC of logic).
// No yaml-cpp, no rapidyaml dependency.
// =============================================================================

#include <batbox/plugins/FrontmatterParser.hpp>

#include <cctype>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace batbox::plugins {

namespace {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/// Trim leading and trailing ASCII whitespace (space, tab, CR) from sv.
/// Does NOT trim newlines — callers strip those explicitly.
[[nodiscard]] constexpr std::string_view trim_space(std::string_view sv) noexcept {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r'))
        sv.remove_prefix(1);
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r'))
        sv.remove_suffix(1);
    return sv;
}

/// Try to parse sv as int64_t.  Returns true and sets out on success.
[[nodiscard]] bool try_parse_int(std::string_view sv, std::int64_t& out) noexcept {
    if (sv.empty()) return false;
    // std::from_chars requires a non-const char* range; string_view is fine.
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), out);
    return ec == std::errc{} && ptr == sv.data() + sv.size();
}

/// Try to coerce sv to a JSON boolean.
/// Returns true and sets out when sv matches true/false/yes/no (case-sensitive).
[[nodiscard]] bool try_parse_bool(std::string_view sv, bool& out) noexcept {
    if (sv == "true"  || sv == "yes") { out = true;  return true; }
    if (sv == "false" || sv == "no")  { out = false; return true; }
    return false;
}

/// Convert a raw scalar token (already trimmed, quotes stripped) to a Json value.
/// Precedence: boolean → integer → string.
[[nodiscard]] Json scalar_to_json(std::string_view tok) {
    bool   b{};
    std::int64_t i{};
    if (try_parse_bool(tok, b)) return Json(b);
    if (try_parse_int(tok, i))  return Json(i);
    return Json(std::string(tok));
}

/// Parse a double-quoted string starting just AFTER the opening '"'.
/// Handles "" escape for literal '"'.
/// Sets value to the unescaped content.
/// Returns the position just past the closing '"', or string_view::npos on error.
[[nodiscard]] std::size_t parse_quoted(std::string_view src, std::string& value) {
    value.clear();
    std::size_t i = 0;
    while (i < src.size()) {
        char c = src[i];
        if (c == '"') {
            // Peek ahead: "" → literal quote
            if (i + 1 < src.size() && src[i + 1] == '"') {
                value += '"';
                i += 2;
            } else {
                return i + 1; // success: return pos after closing '"'
            }
        } else {
            value += c;
            ++i;
        }
    }
    return std::string_view::npos; // unterminated string
}

/// Parse a YAML flow-style list:  [item1, item2, "item3", ...]
/// src starts just AFTER the '['.
/// Returns parsed items, or an empty vector + sets error on failure.
[[nodiscard]] bool parse_flow_list(std::string_view src,
                                   std::vector<std::string>& items,
                                   std::string& error) {
    items.clear();
    std::size_t i = 0;
    // Skip leading whitespace
    while (i < src.size() && (src[i] == ' ' || src[i] == '\t')) ++i;

    if (i < src.size() && src[i] == ']') return true; // empty list []

    while (i < src.size()) {
        // Skip whitespace before item
        while (i < src.size() && (src[i] == ' ' || src[i] == '\t')) ++i;
        if (i >= src.size()) { error = "unterminated flow list (missing ']')"; return false; }

        std::string item;
        if (src[i] == '"') {
            // Quoted item
            ++i; // skip opening '"'
            std::size_t consumed = parse_quoted(src.substr(i), item);
            if (consumed == std::string_view::npos) {
                error = "unterminated quoted string in flow list";
                return false;
            }
            i += consumed;
        } else {
            // Unquoted item: read until , or ]
            std::size_t start = i;
            while (i < src.size() && src[i] != ',' && src[i] != ']') ++i;
            item = std::string(trim_space(src.substr(start, i - start)));
        }

        items.push_back(std::move(item));

        // Skip whitespace after item
        while (i < src.size() && (src[i] == ' ' || src[i] == '\t')) ++i;
        if (i >= src.size()) { error = "unterminated flow list (missing ']')"; return false; }

        if (src[i] == ']') { ++i; return true; }      // done
        if (src[i] == ',') { ++i; continue; }         // next item
        error = "expected ',' or ']' in flow list";
        return false;
    }
    error = "unterminated flow list (missing ']')";
    return false;
}

/// Build a "line:col: message" error string.
[[nodiscard]] std::string make_error(int line, int col, std::string_view msg) {
    return std::to_string(line) + ':' + std::to_string(col) + ": " + std::string(msg);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// parse_frontmatter
// ---------------------------------------------------------------------------

Result<FrontmatterResult>
parse_frontmatter(std::string_view md_content) {
    // ---- Step 1: check for leading "---" delimiter -------------------------
    //
    // The frontmatter block must start at byte 0 with the line "---\n"
    // (or "---\r\n").  If not present, return empty map + whole content.

    constexpr std::string_view kDelim = "---";

    if (md_content.size() < 3 || md_content.substr(0, 3) != kDelim) {
        // No frontmatter — whole content is the body.
        return FrontmatterResult{ Frontmatter{}, std::string(md_content) };
    }

    // Skip optional \r after ---
    std::size_t pos = 3;
    if (pos < md_content.size() && md_content[pos] == '\r') ++pos;
    if (pos >= md_content.size() || md_content[pos] != '\n') {
        // "---" not followed by newline — not YAML frontmatter; treat as body.
        return FrontmatterResult{ Frontmatter{}, std::string(md_content) };
    }
    ++pos; // consume '\n'

    // ---- Step 2: find closing "---" ----------------------------------------
    //
    // Scan lines until we hit "---" (optionally followed by \r\n or \n).
    // Track line/col for error reporting (line 2 = first line after opening ---).

    Frontmatter meta;
    int  line_no = 2;
    bool in_block_list = false;     // true while collecting "  - item" lines
    std::string current_block_key;  // key being built as a block list
    std::vector<std::string> block_items;

    auto flush_block_list = [&]() {
        if (in_block_list) {
            Json arr = Json::array();
            for (auto& it : block_items) arr.push_back(it);
            meta[current_block_key] = std::move(arr);
            in_block_list = false;
            current_block_key.clear();
            block_items.clear();
        }
    };

    while (pos < md_content.size()) {
        // Find end of current line
        std::size_t eol = md_content.find('\n', pos);
        bool has_eol = (eol != std::string_view::npos);
        std::string_view raw_line = md_content.substr(pos, has_eol ? eol - pos : md_content.size() - pos);

        // Strip trailing \r
        std::string_view line = raw_line;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        pos = has_eol ? eol + 1 : md_content.size();

        // Check for closing delimiter
        if (line == kDelim) {
            flush_block_list();
            // Body is everything from pos onward
            std::string body = pos < md_content.size()
                ? std::string(md_content.substr(pos))
                : std::string{};
            return FrontmatterResult{ std::move(meta), std::move(body) };
        }

        // ---- Block list continuation: "  - item" or "- item" ---------------
        // A line that starts with optional whitespace then "- " (or just "-")
        // while in_block_list continues the current block list.
        {
            std::string_view stripped = line;
            while (!stripped.empty() && (stripped.front() == ' ' || stripped.front() == '\t'))
                stripped.remove_prefix(1);

            if (in_block_list) {
                if (!stripped.empty() && stripped.front() == '-') {
                    // "- item" continuation
                    std::string_view item_sv = stripped.substr(1);
                    item_sv = trim_space(item_sv);
                    // Strip quotes if present
                    std::string item_str;
                    if (!item_sv.empty() && item_sv.front() == '"') {
                        std::size_t consumed = parse_quoted(item_sv.substr(1), item_str);
                        if (consumed == std::string_view::npos) {
                            return Err(make_error(line_no, 1, "unterminated quoted string in block list"));
                        }
                    } else {
                        item_str = std::string(item_sv);
                    }
                    block_items.push_back(std::move(item_str));
                    ++line_no;
                    continue;
                } else {
                    // Non-list line: flush and fall through to key:value parsing
                    flush_block_list();
                }
            }
        }

        // ---- Empty / comment lines -----------------------------------------
        std::string_view trimmed = trim_space(line);
        if (trimmed.empty() || trimmed.front() == '#') {
            ++line_no;
            continue;
        }

        // ---- Key: value parsing --------------------------------------------
        std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            return Err(make_error(line_no, 1, "expected 'key: value' but found no ':'"));
        }

        std::string_view key_sv = trim_space(line.substr(0, colon));
        if (key_sv.empty()) {
            return Err(make_error(line_no, 1, "empty key"));
        }
        // Validate key characters
        for (char c : key_sv) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') {
                return Err(make_error(line_no, 1,
                    "key '" + std::string(key_sv) + "' contains invalid character '" + c + "'"));
            }
        }

        std::string key = std::string(key_sv);
        std::string_view val_sv = line.substr(colon + 1); // everything after ':'

        // Trim leading whitespace from value
        while (!val_sv.empty() && (val_sv.front() == ' ' || val_sv.front() == '\t'))
            val_sv.remove_prefix(1);
        // Strip trailing \r (already done on line, but be safe)
        while (!val_sv.empty() && val_sv.back() == '\r') val_sv.remove_suffix(1);

        // ---- Empty value: start a potential block list ---------------------
        if (val_sv.empty()) {
            // Could be the start of a block-style list; mark it and peek next line.
            in_block_list = true;
            current_block_key = key;
            block_items.clear();
            ++line_no;
            continue;
        }

        // ---- Flow-style list: [a, b, c] ------------------------------------
        if (val_sv.front() == '[') {
            std::string_view list_body = val_sv.substr(1); // after '['
            std::vector<std::string> items;
            std::string list_err;
            if (!parse_flow_list(list_body, items, list_err)) {
                return Err(make_error(line_no, static_cast<int>(colon + 2), list_err));
            }
            Json arr = Json::array();
            for (auto& it : items) arr.push_back(it);
            meta[key] = std::move(arr);
            ++line_no;
            continue;
        }

        // ---- Quoted string -------------------------------------------------
        if (val_sv.front() == '"') {
            std::string qval;
            std::size_t consumed = parse_quoted(val_sv.substr(1), qval);
            if (consumed == std::string_view::npos) {
                return Err(make_error(line_no, static_cast<int>(colon + 2),
                    "unterminated quoted string"));
            }
            meta[key] = Json(std::move(qval));
            ++line_no;
            continue;
        }

        // ---- Scalar (bool / int / string) ----------------------------------
        meta[key] = scalar_to_json(trim_space(val_sv));
        ++line_no;
    }

    // If we reach here, we consumed all content without finding a closing "---".
    return Err(make_error(line_no, 1, "frontmatter block not closed (missing closing '---')"));
}

} // namespace batbox::plugins
