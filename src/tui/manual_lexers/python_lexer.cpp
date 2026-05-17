// src/tui/manual_lexers/python_lexer.cpp
// ---------------------------------------------------------------------------
// Manual Python 3 lexer for BATBOX_SYNTAX=0 fallback.
//
// Recognises:
//   - Python 3 keywords
//   - Line comments #
//   - Triple-quoted strings (""" and ''') — single and double
//   - String prefixes: r, b, f, rb, br, rf, fr (case-insensitive)
//   - Double-quoted and single-quoted strings (with escape sequences)
//   - Integer, float, and complex literals
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

constexpr std::array<std::string_view, 36> kPyKeywords = {{
    "False", "None", "True",
    "and", "as", "assert", "async", "await",
    "break", "class", "continue",
    "def", "del",
    "elif", "else", "except",
    "finally", "for", "from",
    "global",
    "if", "import", "in", "is",
    "lambda",
    "nonlocal", "not",
    "or",
    "pass",
    "raise", "return",
    "try", "type",
    "while", "with",
    "yield",
}};

bool is_py_keyword(std::string_view w) {
    return std::find(kPyKeywords.begin(), kPyKeywords.end(), w) != kPyKeywords.end();
}

bool is_ident_start(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
bool is_ident_cont(char c)  { return std::isalnum((unsigned char)c) || c == '_'; }
bool is_digit(char c)        { return std::isdigit((unsigned char)c); }

} // namespace

std::vector<Token> lex_python(std::string_view src) {
    std::vector<Token> tokens;
    tokens.reserve(src.size() / 4);

    const char* p   = src.data();
    const char* end = src.data() + src.size();

    auto push = [&](Token::Kind k, const char* begin, const char* finish) {
        if (finish > begin)
            tokens.push_back({k, std::string_view(begin, static_cast<size_t>(finish - begin))});
    };

    while (p < end) {
        // ---- Line comment ------------------------------------------------
        if (*p == '#') {
            const char* s = p;
            while (p < end && *p != '\n') ++p;
            push(Token::Kind::Comment, s, p);
            continue;
        }
        // ---- String (with optional prefix) ------------------------------
        {
            const char* q = p;
            // consume optional string prefix: r/R/b/B/f/F/u/U and combinations
            while (q < end && (*q == 'r' || *q == 'R' || *q == 'b' || *q == 'B' ||
                               *q == 'f' || *q == 'F' || *q == 'u' || *q == 'U') &&
                   (size_t)(q - p) < 3) {
                ++q;
            }
            if (q < end && (*q == '"' || *q == '\'')) {
                char delim = *q;
                const char* s = p;
                p = q;
                // Check for triple quote
                if (p + 2 < end && p[1] == delim && p[2] == delim) {
                    p += 3;
                    // scan for matching triple quote
                    while (p + 2 < end && !(p[0] == delim && p[1] == delim && p[2] == delim)) {
                        if (*p == '\\') ++p;
                        if (p < end) ++p;
                    }
                    if (p + 2 < end) p += 3;
                } else {
                    ++p; // opening quote
                    while (p < end && *p != delim && *p != '\n') {
                        if (*p == '\\') ++p;
                        if (p < end) ++p;
                    }
                    if (p < end && *p == delim) ++p;
                }
                push(Token::Kind::String, s, p);
                continue;
            }
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
                if (p < end && *p == '.') { ++p; while (p < end && (is_digit(*p) || *p == '_')) ++p; }
                if (p < end && (*p == 'e' || *p == 'E')) {
                    ++p;
                    if (p < end && (*p == '+' || *p == '-')) ++p;
                    while (p < end && is_digit(*p)) ++p;
                }
            }
            // complex suffix
            if (p < end && (*p == 'j' || *p == 'J')) ++p;
            push(Token::Kind::Number, s, p);
            continue;
        }
        // ---- Identifier or keyword --------------------------------------
        if (is_ident_start(*p)) {
            const char* s = p;
            while (p < end && is_ident_cont(*p)) ++p;
            std::string_view word(s, static_cast<size_t>(p - s));
            push(is_py_keyword(word) ? Token::Kind::Keyword : Token::Kind::Plain, s, p);
            continue;
        }
        // ---- Everything else --------------------------------------------
        push(Token::Kind::Plain, p, p + 1);
        ++p;
    }

    return tokens;
}

} // namespace batbox::tui::detail
