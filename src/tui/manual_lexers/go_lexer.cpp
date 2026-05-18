// src/tui/manual_lexers/go_lexer.cpp
// ---------------------------------------------------------------------------
// Manual Go lexer manual lexer.
//
// Recognises:
//   - Go keywords and predeclared identifiers
//   - Line comments //  and block comments /* */
//   - Interpreted string literals "…" (with escape sequences)
//   - Raw string literals `…`  (backtick, no escapes)
//   - Rune literals '…'
//   - Integer literals: decimal, hex, octal (0o), binary (0b), legacy octal (0…)
//   - Float literals with optional exponent and imaginary suffix (i)
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

constexpr std::array<std::string_view, 62> kGoKeywords = {{
    // Keywords
    "break", "case", "chan", "const", "continue",
    "default", "defer", "else", "fallthrough", "for",
    "func", "go", "goto", "if", "import",
    "interface", "map", "package", "range", "return",
    "select", "struct", "switch", "type", "var",
    // Predeclared identifiers (types, constants, functions)
    "any", "bool", "byte", "comparable",
    "complex64", "complex128",
    "error",
    "false", "float32", "float64",
    "imag", "int", "int8", "int16", "int32", "int64",
    "iota",
    "len", "make", "max", "min",
    "new", "nil",
    "panic", "print", "println",
    "real", "recover", "rune",
    "string",
    "true",
    "uint", "uint8", "uint16", "uint32", "uint64",
    "uintptr",
}};

bool is_go_keyword(std::string_view w) {
    return std::find(kGoKeywords.begin(), kGoKeywords.end(), w) != kGoKeywords.end();
}

bool is_ident_start(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
bool is_ident_cont(char c)  { return std::isalnum((unsigned char)c) || c == '_'; }
bool is_digit(char c)        { return std::isdigit((unsigned char)c); }

} // namespace

std::vector<Token> lex_go(std::string_view src) {
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
        if (p + 1 < end && p[0] == '/' && p[1] == '/') {
            const char* s = p;
            while (p < end && *p != '\n') ++p;
            push(Token::Kind::Comment, s, p);
            continue;
        }
        // ---- Block comment -----------------------------------------------
        if (p + 1 < end && p[0] == '/' && p[1] == '*') {
            const char* s = p;
            p += 2;
            while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) ++p;
            if (p + 1 < end) p += 2;
            push(Token::Kind::Comment, s, p);
            continue;
        }
        // ---- Raw string literal (backtick) ------------------------------
        if (*p == '`') {
            const char* s = p++;
            while (p < end && *p != '`') ++p;
            if (p < end) ++p;
            push(Token::Kind::String, s, p);
            continue;
        }
        // ---- Interpreted string literal ---------------------------------
        if (*p == '"') {
            const char* s = p++;
            while (p < end && *p != '"' && *p != '\n') {
                if (*p == '\\') ++p;
                if (p < end) ++p;
            }
            if (p < end && *p == '"') ++p;
            push(Token::Kind::String, s, p);
            continue;
        }
        // ---- Rune literal -----------------------------------------------
        if (*p == '\'') {
            const char* s = p++;
            if (p < end && *p == '\\') { ++p; if (p < end) ++p; }
            else if (p < end) ++p;
            if (p < end && *p == '\'') ++p;
            push(Token::Kind::String, s, p);
            continue;
        }
        // ---- Numeric literal --------------------------------------------
        if (is_digit(*p)) {
            const char* s = p;
            if (*p == '0' && p + 1 < end) {
                if (p[1] == 'x' || p[1] == 'X') {
                    p += 2; while (p < end && (std::isxdigit((unsigned char)*p) || *p == '_')) ++p;
                } else if (p[1] == 'o' || p[1] == 'O') {
                    p += 2; while (p < end && (*p >= '0' && *p <= '7' || *p == '_')) ++p;
                } else if (p[1] == 'b' || p[1] == 'B') {
                    p += 2; while (p < end && (*p == '0' || *p == '1' || *p == '_')) ++p;
                } else {
                    while (p < end && (is_digit(*p) || *p == '_')) ++p;
                }
            } else {
                while (p < end && (is_digit(*p) || *p == '_')) ++p;
                if (p < end && *p == '.') { ++p; while (p < end && (is_digit(*p) || *p == '_')) ++p; }
                if (p < end && (*p == 'e' || *p == 'E' || *p == 'p' || *p == 'P')) {
                    ++p;
                    if (p < end && (*p == '+' || *p == '-')) ++p;
                    while (p < end && is_digit(*p)) ++p;
                }
            }
            if (p < end && *p == 'i') ++p; // imaginary
            push(Token::Kind::Number, s, p);
            continue;
        }
        // ---- Identifier or keyword --------------------------------------
        if (is_ident_start(*p)) {
            const char* s = p;
            while (p < end && is_ident_cont(*p)) ++p;
            std::string_view word(s, static_cast<size_t>(p - s));
            push(is_go_keyword(word) ? Token::Kind::Keyword : Token::Kind::Plain, s, p);
            continue;
        }
        // ---- Everything else --------------------------------------------
        push(Token::Kind::Plain, p, p + 1);
        ++p;
    }

    return tokens;
}

} // namespace batbox::tui::detail
