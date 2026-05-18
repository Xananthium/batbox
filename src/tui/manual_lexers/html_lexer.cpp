// src/tui/manual_lexers/html_lexer.cpp
// ---------------------------------------------------------------------------
// Manual HTML lexer.
//
// Recognises:
//   - <!DOCTYPE ...>                 → Comment (preprocessor-style color)
//   - <!-- ... -->                   → Comment (multi-line safe)
//   - <tag> </tag> <tag />           → tag name as Keyword
//   - attr="value" attr='value'      → attr name as Plain, value as String
//   - bare boolean attrs (disabled)  → Plain
//   - &entity; and &#123;            → String (escape color)
//   - Text between tags              → Plain
//   - <script>...</script>           → contents kept as Plain (no JS recursion)
//   - <style>...</style>             → contents kept as Plain (no CSS recursion)
//   - Angle brackets / punctuation   → Operator
// ---------------------------------------------------------------------------
#include "manual_lexers.hpp"

#include <cctype>
#include <cstring>
#include <string_view>
#include <vector>

namespace batbox::tui::detail {

namespace {

bool is_tag_name_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) ||
           c == '-' || c == '_' || c == '.' || c == ':';
}

bool is_attr_name_char(char c) {
    // XML/HTML attribute names: letters, digits, -, _, :, .
    return std::isalnum(static_cast<unsigned char>(c)) ||
           c == '-' || c == '_' || c == ':' || c == '.';
}

bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Case-insensitive comparison for tag name detection (script / style).
bool tag_name_eq_ci(std::string_view a, const char* b) {
    std::size_t n = std::strlen(b);
    if (a.size() != n) return false;
    for (std::size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

} // namespace

std::vector<Token> lex_html(std::string_view src) {
    std::vector<Token> tokens;
    tokens.reserve(src.size() / 6);

    const char* p   = src.data();
    const char* end = src.data() + src.size();

    auto push = [&](Token::Kind k, const char* begin, const char* finish) {
        if (finish > begin)
            tokens.push_back({k, std::string_view(begin, static_cast<std::size_t>(finish - begin))});
    };

    while (p < end) {
        // ----------------------------------------------------------------
        // Outside a tag: text content, comments, doctype, or opening '<'
        // ----------------------------------------------------------------

        if (*p != '<') {
            // ---- HTML entity: &name; or &#digits; or &#xHex; ----------
            if (*p == '&') {
                const char* s = p++;
                // consume until ';' or whitespace or '<' or EOF
                while (p < end && *p != ';' && *p != '<' && !is_ws(*p)) ++p;
                if (p < end && *p == ';') ++p; // consume the semicolon
                push(Token::Kind::String, s, p);
                continue;
            }

            // ---- Plain text between tags --------------------------------
            const char* s = p;
            while (p < end && *p != '<' && *p != '&') ++p;
            push(Token::Kind::Plain, s, p);
            continue;
        }

        // p points at '<'

        // ---- Comment: <!-- ... --> -------------------------------------
        if (p + 3 < end && p[1] == '!' && p[2] == '-' && p[3] == '-') {
            const char* s = p;
            p += 4;
            while (p + 2 < end && !(p[0] == '-' && p[1] == '-' && p[2] == '>')) ++p;
            if (p + 2 < end) p += 3; // consume -->
            push(Token::Kind::Comment, s, p);
            continue;
        }

        // ---- DOCTYPE: <!DOCTYPE ...> -----------------------------------
        if (p + 1 < end && p[1] == '!') {
            const char* s = p;
            // Scan to closing '>'
            while (p < end && *p != '>') ++p;
            if (p < end) ++p; // consume '>'
            push(Token::Kind::Comment, s, p);
            continue;
        }

        // ---- Tag: <tagname ... > or </tagname> or <tagname ... /> -----
        {
            const char* tag_open = p;
            p++; // consume '<'

            bool is_close = false;
            if (p < end && *p == '/') {
                is_close = true;
                ++p;
            }

            // Emit the '<' (and '/' if closing) as operator
            push(Token::Kind::Operator, tag_open, p);

            // Collect tag name
            const char* name_start = p;
            while (p < end && is_tag_name_char(*p)) ++p;
            const char* name_end = p;

            std::string_view tag_name(name_start, static_cast<std::size_t>(name_end - name_start));
            bool is_script = tag_name_eq_ci(tag_name, "script");
            bool is_style  = tag_name_eq_ci(tag_name, "style");

            // Tag name → Keyword (even empty/malformed tag leaves the text
            // alone; lex_html never crashes on malformed input).
            push(Token::Kind::Keyword, name_start, name_end);

            // ---- Parse attributes inside the tag ----------------------
            while (p < end && *p != '>') {
                // Check for self-closing '/>'
                if (p + 1 < end && p[0] == '/' && p[1] == '>') {
                    push(Token::Kind::Operator, p, p + 2);
                    p += 2;
                    goto after_tag; // NOLINT(cppcoreguidelines-avoid-goto)
                }

                // Whitespace inside tag
                if (is_ws(*p)) {
                    push(Token::Kind::Plain, p, p + 1);
                    ++p;
                    continue;
                }

                // Attribute name
                if (is_attr_name_char(*p)) {
                    const char* attr_start = p;
                    while (p < end && is_attr_name_char(*p)) ++p;
                    push(Token::Kind::Plain, attr_start, p);

                    // Skip whitespace around '='
                    const char* ws_start = p;
                    while (p < end && is_ws(*p)) ++p;

                    if (p < end && *p == '=') {
                        // Emit any whitespace before '='
                        push(Token::Kind::Plain, ws_start, p);
                        push(Token::Kind::Operator, p, p + 1);
                        ++p; // consume '='

                        // Skip whitespace after '='
                        while (p < end && is_ws(*p)) { push(Token::Kind::Plain, p, p+1); ++p; }

                        // Attribute value: quoted or unquoted
                        if (p < end && (*p == '"' || *p == '\'')) {
                            char delim = *p;
                            const char* val_start = p++;
                            while (p < end && *p != delim) ++p;
                            if (p < end) ++p; // closing quote
                            push(Token::Kind::String, val_start, p);
                        } else {
                            // Unquoted value: read until whitespace or '>'
                            const char* val_start = p;
                            while (p < end && !is_ws(*p) && *p != '>' && *p != '/') ++p;
                            push(Token::Kind::String, val_start, p);
                        }
                    } else {
                        // Boolean attr (no '=') — emit the whitespace we already skipped as plain
                        push(Token::Kind::Plain, ws_start, p);
                    }
                    continue;
                }

                // Anything else inside tag (stray chars, malformed) → plain
                push(Token::Kind::Plain, p, p + 1);
                ++p;
            }

            // Consume closing '>'
            if (p < end && *p == '>') {
                push(Token::Kind::Operator, p, p + 1);
                ++p;
            }

            after_tag:

            // For <script> and <style> (opening tags only) capture the
            // raw text content up to the matching close tag as Plain so we
            // don't accidentally recurse into embedded JS/CSS lexers.
            if (!is_close && (is_script || is_style)) {
                const char* raw_start = p;
                const char* close_tag = is_script ? "</script" : "</style";
                std::size_t close_len = std::strlen(close_tag);

                while (p < end) {
                    // Look for the opening '<' of the close tag
                    if (*p == '<' &&
                        static_cast<std::size_t>(end - p) > close_len &&
                        // case-insensitive check for first 8 chars
                        [&]() -> bool {
                            for (std::size_t i = 0; i < close_len; ++i) {
                                if (std::tolower(static_cast<unsigned char>(p[i])) !=
                                    static_cast<unsigned char>(close_tag[i]))
                                    return false;
                            }
                            return true;
                        }()) {
                        break;
                    }
                    ++p;
                }
                push(Token::Kind::Plain, raw_start, p);
                // The </script> or </style> tag itself will be lexed in the
                // next loop iteration as a normal closing tag.
            }
        }
    }

    return tokens;
}

} // namespace batbox::tui::detail
