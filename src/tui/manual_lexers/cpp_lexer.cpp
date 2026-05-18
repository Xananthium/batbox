// src/tui/manual_lexers/cpp_lexer.cpp
// ---------------------------------------------------------------------------
// Manual C++ lexer manual lexer.
//
// Recognises:
//   - C++20 keywords (including auto, constexpr, concept, co_await, etc.)
//   - Line comments //  and block comments /* */
//   - Double-quoted string literals (with \" escape)
//   - Single-quoted char literals (with \' escape)
//   - Raw string literals R"(…)"
//   - Integer and float literals (decimal, hex 0x, octal 0, binary 0b)
//   - Preprocessor directives (#include, #define, …)
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

// All C++ keywords (C++20 standard plus common extensions).
constexpr std::array<std::string_view, 105> kCppKeywords = {{
    "alignas", "alignof", "and", "and_eq", "asm", "auto",
    "bitand", "bitor", "bool", "break",
    "case", "catch", "char", "char8_t", "char16_t", "char32_t",
    "class", "compl", "concept", "const", "consteval", "constexpr",
    "constinit", "const_cast", "continue", "co_await", "co_return",
    "co_yield",
    "decltype", "default", "delete", "do", "double", "dynamic_cast",
    "else", "enum", "explicit", "export", "extern",
    "false", "float", "for", "friend",
    "goto",
    "if", "inline", "int",
    "long",
    "mutable",
    "namespace", "new", "noexcept", "not", "not_eq", "nullptr",
    "operator", "or", "or_eq",
    "private", "protected", "public",
    "register", "reinterpret_cast", "requires", "return",
    "short", "signed", "sizeof", "static", "static_assert",
    "static_cast", "struct", "switch",
    "template", "this", "thread_local", "throw", "true", "try",
    "typedef", "typeid", "typename",
    "union", "unsigned", "using",
    "virtual", "void", "volatile",
    "wchar_t", "while",
    "xor", "xor_eq",
    // Common type aliases that read like keywords
    "size_t", "ptrdiff_t", "nullptr_t", "int8_t", "int16_t",
    "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t",
    "uint64_t", "override", "final",
}};

bool is_cpp_keyword(std::string_view w) {
    return std::find(kCppKeywords.begin(), kCppKeywords.end(), w) != kCppKeywords.end();
}

bool is_ident_start(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
bool is_ident_cont(char c)  { return std::isalnum((unsigned char)c) || c == '_'; }
bool is_digit(char c)        { return std::isdigit((unsigned char)c); }

} // namespace

std::vector<Token> lex_cpp(std::string_view src) {
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
            if (p + 1 < end) p += 2; // consume */
            push(Token::Kind::Comment, s, p);
            continue;
        }
        // ---- Raw string literal R"delim(…)delim" -------------------------
        if (p + 2 < end && p[0] == 'R' && p[1] == '"') {
            const char* s = p;
            p += 2;
            // collect delimiter
            const char* delim_start = p;
            while (p < end && *p != '(') ++p;
            std::string_view delim(delim_start, static_cast<size_t>(p - delim_start));
            if (p < end) ++p; // consume '('
            // search for )delim"
            while (p < end) {
                if (*p == ')') {
                    const char* q = p + 1;
                    if (static_cast<size_t>(end - q) >= delim.size() &&
                        std::string_view(q, delim.size()) == delim &&
                        q + delim.size() < end && q[delim.size()] == '"') {
                        p = q + delim.size() + 1;
                        break;
                    }
                }
                ++p;
            }
            push(Token::Kind::String, s, p);
            continue;
        }
        // ---- String prefix + double-quoted literal -----------------------
        if ((*p == '"') ||
            (p + 1 < end && (*p == 'u' || *p == 'U' || *p == 'L') && p[1] == '"') ||
            (p + 2 < end && p[0] == 'u' && p[1] == '8' && p[2] == '"')) {
            const char* s = p;
            // skip prefix
            while (p < end && *p != '"') ++p;
            if (p < end) ++p; // opening "
            while (p < end && *p != '"') {
                if (*p == '\\') ++p; // skip escape
                if (p < end) ++p;
            }
            if (p < end) ++p; // closing "
            push(Token::Kind::String, s, p);
            continue;
        }
        // ---- Char literal ------------------------------------------------
        if (*p == '\'') {
            const char* s = p++;
            while (p < end && *p != '\'') {
                if (*p == '\\') ++p;
                if (p < end) ++p;
            }
            if (p < end) ++p;
            push(Token::Kind::String, s, p);
            continue;
        }
        // ---- Preprocessor directive -------------------------------------
        if (*p == '#') {
            const char* s = p;
            while (p < end && *p != '\n') ++p;
            push(Token::Kind::Keyword, s, p);
            continue;
        }
        // ---- Numeric literal --------------------------------------------
        if (is_digit(*p) || (p + 1 < end && *p == '.' && is_digit(p[1]))) {
            const char* s = p;
            if (*p == '0' && p + 1 < end) {
                if (p[1] == 'x' || p[1] == 'X') {
                    p += 2;
                    while (p < end && std::isxdigit((unsigned char)*p)) ++p;
                } else if (p[1] == 'b' || p[1] == 'B') {
                    p += 2;
                    while (p < end && (*p == '0' || *p == '1')) ++p;
                } else {
                    while (p < end && is_digit(*p)) ++p;
                }
            } else {
                while (p < end && is_digit(*p)) ++p;
                if (p < end && *p == '.') { ++p; while (p < end && is_digit(*p)) ++p; }
                if (p < end && (*p == 'e' || *p == 'E')) {
                    ++p;
                    if (p < end && (*p == '+' || *p == '-')) ++p;
                    while (p < end && is_digit(*p)) ++p;
                }
            }
            // integer suffix u/l/ul/ll/ull
            while (p < end && (*p == 'u' || *p == 'U' || *p == 'l' || *p == 'L' ||
                                *p == 'f' || *p == 'F')) ++p;
            push(Token::Kind::Number, s, p);
            continue;
        }
        // ---- Identifier or keyword --------------------------------------
        if (is_ident_start(*p)) {
            const char* s = p;
            while (p < end && is_ident_cont(*p)) ++p;
            std::string_view word(s, static_cast<size_t>(p - s));
            push(is_cpp_keyword(word) ? Token::Kind::Keyword : Token::Kind::Plain, s, p);
            continue;
        }
        // ---- Everything else (operators, punctuation, whitespace) --------
        push(Token::Kind::Plain, p, p + 1);
        ++p;
    }

    return tokens;
}

} // namespace batbox::tui::detail
