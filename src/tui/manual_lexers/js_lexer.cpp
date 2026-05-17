// src/tui/manual_lexers/js_lexer.cpp
// ---------------------------------------------------------------------------
// Manual JavaScript / TypeScript lexer for BATBOX_SYNTAX=0 fallback.
//
// Recognises:
//   - JS + TS keywords (combined)
//   - Line comments //  and block comments /* */
//   - Template literals `…` (with ${} elision — treated as one string span)
//   - Double-quoted and single-quoted strings
//   - Regular expression literals (heuristic: /…/ after operator or keyword)
//   - Integer, float, BigInt, hex, octal, binary literals
//   - Remaining text as Plain tokens
// ---------------------------------------------------------------------------
#include "manual_lexers.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>
#include <vector>

namespace batbox::tui::detail {

namespace {

constexpr std::array<std::string_view, 78> kJsKeywords = {{
    // JavaScript
    "break", "case", "catch", "class", "const", "continue",
    "debugger", "default", "delete", "do",
    "else", "export", "extends",
    "false", "finally", "for", "function",
    "if", "import", "in", "instanceof",
    "let",
    "new", "null",
    "of",
    "return",
    "static", "super", "switch",
    "this", "throw", "true", "try", "typeof",
    "undefined",
    "var", "void",
    "while", "with",
    "yield",
    // TypeScript additions
    "abstract", "as", "asserts", "async", "await",
    "bigint", "boolean",
    "declare",
    "enum",
    "from",
    "global",
    "implements", "infer", "interface", "intrinsic",
    "is",
    "keyof",
    "module",
    "namespace", "never", "noInfer",
    "number",
    "object", "override",
    "private", "protected", "public",
    "readonly", "require",
    "satisfies", "string", "symbol",
    "type",
    "unique", "unknown",
    "using",
}};

bool is_js_keyword(std::string_view w) {
    return std::find(kJsKeywords.begin(), kJsKeywords.end(), w) != kJsKeywords.end();
}

bool is_ident_start(char c) { return std::isalpha((unsigned char)c) || c == '_' || c == '$'; }
bool is_ident_cont(char c)  { return std::isalnum((unsigned char)c) || c == '_' || c == '$'; }
bool is_digit(char c)        { return std::isdigit((unsigned char)c); }

} // namespace

std::vector<Token> lex_js(std::string_view src) {
    std::vector<Token> tokens;
    tokens.reserve(src.size() / 4);

    const char* p   = src.data();
    const char* end = src.data() + src.size();
    bool after_operator = true; // heuristic for regex detection

    auto push = [&](Token::Kind k, const char* begin, const char* finish) {
        if (finish > begin)
            tokens.push_back({k, std::string_view(begin, static_cast<size_t>(finish - begin))});
    };

    while (p < end) {
        // ---- Line comment ------------------------------------------------
        if (p + 1 < end && p[0] == '/' && p[1] == '/') {
            const char* s = p;
            while (p < end && *p != '\n') ++p;
            push(Token::Kind::Comment, s, p);
            after_operator = false;
            continue;
        }
        // ---- Block comment -----------------------------------------------
        if (p + 1 < end && p[0] == '/' && p[1] == '*') {
            const char* s = p;
            p += 2;
            while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) ++p;
            if (p + 1 < end) p += 2;
            push(Token::Kind::Comment, s, p);
            after_operator = false;
            continue;
        }
        // ---- Regex literal (heuristic) ----------------------------------
        if (*p == '/' && after_operator) {
            const char* s = p++;
            while (p < end && *p != '/' && *p != '\n') {
                if (*p == '\\') ++p;
                if (p < end) ++p;
            }
            if (p < end && *p == '/') {
                ++p;
                // flags: g, i, m, s, u, v, y
                while (p < end && std::isalpha((unsigned char)*p)) ++p;
                push(Token::Kind::String, s, p);
                after_operator = false;
                continue;
            }
            // Not a regex — treat as operator
            push(Token::Kind::Plain, s, p);
            after_operator = true;
            continue;
        }
        // ---- Template literal -------------------------------------------
        if (*p == '`') {
            const char* s = p++;
            while (p < end && *p != '`') {
                if (*p == '\\') ++p;
                if (p < end) ++p;
            }
            if (p < end) ++p;
            push(Token::Kind::String, s, p);
            after_operator = false;
            continue;
        }
        // ---- String literal ----------------------------------------------
        if (*p == '"' || *p == '\'') {
            char delim = *p;
            const char* s = p++;
            while (p < end && *p != delim && *p != '\n') {
                if (*p == '\\') ++p;
                if (p < end) ++p;
            }
            if (p < end) ++p;
            push(Token::Kind::String, s, p);
            after_operator = false;
            continue;
        }
        // ---- Numeric literal --------------------------------------------
        if (is_digit(*p) || (p + 1 < end && *p == '.' && is_digit(p[1]))) {
            const char* s = p;
            if (*p == '0' && p + 1 < end) {
                if (p[1] == 'x' || p[1] == 'X') {
                    p += 2; while (p < end && std::isxdigit((unsigned char)*p)) ++p;
                } else if (p[1] == 'o' || p[1] == 'O') {
                    p += 2; while (p < end && *p >= '0' && *p <= '7') ++p;
                } else if (p[1] == 'b' || p[1] == 'B') {
                    p += 2; while (p < end && (*p == '0' || *p == '1')) ++p;
                } else {
                    while (p < end && (is_digit(*p) || *p == '_')) ++p;
                }
            } else {
                while (p < end && (is_digit(*p) || *p == '_')) ++p;
                if (p < end && *p == '.') { ++p; while (p < end && is_digit(*p)) ++p; }
                if (p < end && (*p == 'e' || *p == 'E')) {
                    ++p;
                    if (p < end && (*p == '+' || *p == '-')) ++p;
                    while (p < end && is_digit(*p)) ++p;
                }
            }
            if (p < end && *p == 'n') ++p; // BigInt suffix
            push(Token::Kind::Number, s, p);
            after_operator = false;
            continue;
        }
        // ---- Identifier or keyword --------------------------------------
        if (is_ident_start(*p)) {
            const char* s = p;
            while (p < end && is_ident_cont(*p)) ++p;
            std::string_view word(s, static_cast<size_t>(p - s));
            bool kw = is_js_keyword(word);
            push(kw ? Token::Kind::Keyword : Token::Kind::Plain, s, p);
            after_operator = kw; // keywords can precede regex
            continue;
        }
        // ---- Operators / punctuation ------------------------------------
        {
            char c = *p;
            bool is_op = c == '(' || c == ')' || c == '[' || c == ']' ||
                         c == '{' || c == '}' || c == ';' || c == ',' ||
                         c == '=' || c == '!' || c == '<' || c == '>' ||
                         c == '+' || c == '-' || c == '*' || c == '%' ||
                         c == '&' || c == '|' || c == '^' || c == '~' ||
                         c == '?' || c == ':' || c == '.';
            push(Token::Kind::Plain, p, p + 1);
            after_operator = is_op;
            ++p;
        }
    }

    return tokens;
}

} // namespace batbox::tui::detail
