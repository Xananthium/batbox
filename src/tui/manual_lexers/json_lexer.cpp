// src/tui/manual_lexers/json_lexer.cpp
// ---------------------------------------------------------------------------
// Manual JSON / JSON5 lexer.
//
// Token mapping:
//   Keyword   — true, false, null                      → AccentMagenta
//   String    — "…" value strings (escape-aware)       → AccentCyan
//   Plain     — "…" object keys (string before ':')    → Fg  (identifier colour)
//   Number    — integer, decimal, scientific            → Success
//   Operator  — { } [ ] , :                            → Fg
//   Comment   — // line and /* block */ (JSON5)        → Muted
//
// Key-vs-value detection:
//   After emitting a string token the lexer skips whitespace/comments and
//   peeks at the next non-whitespace character.  If it is ':' the string was
//   an object key and its Kind is retroactively changed to Plain so that keys
//   render in a distinguishable colour.
//
// JSON5 extensions (recognised, not required):
//   // line comments, /* block comments */, single-quoted strings,
//   unquoted identifer keys, trailing commas.
//
// Malformed input: best-effort — the lexer never crashes on bad UTF-8 or
// truncated sequences.  An unterminated string is closed at EOF.
// ---------------------------------------------------------------------------
#include "manual_lexers.hpp"

#include <cctype>
#include <string_view>
#include <vector>

namespace batbox::tui::detail {

namespace {

bool is_json_ws(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool is_ident_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$';
}
bool is_ident_cont(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
}

// Peek past whitespace and comments (JSON5) and return the next char or '\0'.
char peek_next_non_ws(const char* p, const char* end) {
    while (p < end) {
        if (is_json_ws(*p)) { ++p; continue; }
        // JSON5 line comment
        if (p + 1 < end && p[0] == '/' && p[1] == '/') {
            p += 2;
            while (p < end && *p != '\n') ++p;
            continue;
        }
        // JSON5 block comment
        if (p + 1 < end && p[0] == '/' && p[1] == '*') {
            p += 2;
            while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) ++p;
            if (p + 1 < end) p += 2;
            continue;
        }
        return *p;
    }
    return '\0';
}

} // namespace

std::vector<Token> lex_json(std::string_view src) {
    std::vector<Token> tokens;
    tokens.reserve(src.size() / 4 + 8);

    const char* p   = src.data();
    const char* end = src.data() + src.size();

    auto push = [&](Token::Kind k, const char* begin, const char* finish) {
        if (finish > begin)
            tokens.push_back({k, std::string_view(begin, static_cast<size_t>(finish - begin))});
    };

    while (p < end) {
        // ---- Whitespace (silent) ----------------------------------------
        if (is_json_ws(*p)) { ++p; continue; }

        // ---- JSON5 line comment -----------------------------------------
        if (p + 1 < end && p[0] == '/' && p[1] == '/') {
            const char* s = p;
            p += 2;
            while (p < end && *p != '\n') ++p;
            push(Token::Kind::Comment, s, p);
            continue;
        }

        // ---- JSON5 block comment ----------------------------------------
        if (p + 1 < end && p[0] == '/' && p[1] == '*') {
            const char* s = p;
            p += 2;
            while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) ++p;
            if (p + 1 < end) p += 2;
            push(Token::Kind::Comment, s, p);
            continue;
        }

        // ---- String (double or single-quoted; JSON5 allows single) ------
        if (*p == '"' || *p == '\'') {
            char delim = *p;
            const char* s = p++;
            while (p < end && *p != delim) {
                if (*p == '\\') {
                    ++p; // skip escape prefix
                    if (p < end) {
                        if (*p == 'u' && p + 4 < end) {
                            p += 4; // skip \uXXXX
                        }
                        ++p;
                    }
                } else {
                    ++p;
                }
            }
            if (p < end) ++p; // closing quote

            // Key detection: peek past whitespace at next char.
            Token::Kind kind = Token::Kind::String;
            if (peek_next_non_ws(p, end) == ':') {
                kind = Token::Kind::Plain; // object key → identifier colour
            }
            push(kind, s, p);
            continue;
        }

        // ---- Number: [-] (int) [.frac] [e/E [+-] exp] ------------------
        if (is_digit(*p) || (*p == '-' && p + 1 < end && is_digit(p[1]))) {
            const char* s = p;
            if (*p == '-') ++p;
            // Integer part
            while (p < end && is_digit(*p)) ++p;
            // Fractional part
            if (p < end && *p == '.') {
                ++p;
                while (p < end && is_digit(*p)) ++p;
            }
            // Exponent part
            if (p < end && (*p == 'e' || *p == 'E')) {
                ++p;
                if (p < end && (*p == '+' || *p == '-')) ++p;
                while (p < end && is_digit(*p)) ++p;
            }
            push(Token::Kind::Number, s, p);
            continue;
        }

        // ---- Identifier: keywords (true/false/null) or JSON5 bare key --
        if (is_ident_start(*p)) {
            const char* s = p;
            while (p < end && is_ident_cont(*p)) ++p;
            std::string_view word(s, static_cast<size_t>(p - s));

            Token::Kind kind;
            if (word == "true" || word == "false" || word == "null") {
                kind = Token::Kind::Keyword;
            } else {
                // JSON5 unquoted key — treat as identifier (Plain).
                // If followed by ':', it's a key; otherwise emit as Plain.
                kind = Token::Kind::Plain;
            }
            push(kind, s, p);
            continue;
        }

        // ---- Structural punctuation: { } [ ] , : -----------------------
        {
            char c = *p;
            if (c == '{' || c == '}' || c == '[' || c == ']' ||
                c == ',' || c == ':') {
                push(Token::Kind::Operator, p, p + 1);
                ++p;
                continue;
            }
        }

        // ---- Anything else: emit as Plain and advance -------------------
        push(Token::Kind::Plain, p, p + 1);
        ++p;
    }

    return tokens;
}

} // namespace batbox::tui::detail
