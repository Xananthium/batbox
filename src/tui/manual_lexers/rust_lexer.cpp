// src/tui/manual_lexers/rust_lexer.cpp
// ---------------------------------------------------------------------------
// Manual Rust lexer manual lexer.
//
// Recognises:
//   - Rust keywords (edition 2021, including reserved words)
//   - Line comments //  and block comments /* … */  and doc comments /// /**
//   - Raw string literals r"…" and r#"…"# (any delimiter count)
//   - Byte literals b'…' and byte string literals b"…"
//   - String literals (with escape sequences)
//   - Character literals
//   - Integer suffixes: i8/i16/i32/i64/i128/isize/u8/…/usize
//   - Float suffixes: f32/f64
//   - Hex, octal, binary integer literals
//   - Lifetime annotations treated as Plain (e.g. 'a, 'static)
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

constexpr std::array<std::string_view, 71> kRustKeywords = {{
    "abstract", "as", "async", "await",
    "become", "box", "break",
    "const", "continue", "crate",
    "do", "dyn",
    "else", "enum", "extern",
    "false", "final", "fn", "for",
    "if", "impl", "in",
    "let", "loop",
    "macro", "match", "mod", "move", "mut",
    "new",
    "override",
    "priv", "pub",
    "ref", "return",
    "Self", "self", "static", "struct", "super",
    "trait", "true", "try", "type", "typeof",
    "union", "unsafe", "unsized", "use",
    "virtual",
    "where", "while",
    "yield",
    // Common type keywords
    "bool", "char", "f32", "f64",
    "i8", "i16", "i32", "i64", "i128", "isize",
    "str", "String",
    "u8", "u16", "u32", "u64", "u128", "usize",
}};

bool is_rust_keyword(std::string_view w) {
    return std::find(kRustKeywords.begin(), kRustKeywords.end(), w) != kRustKeywords.end();
}

bool is_ident_start(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
bool is_ident_cont(char c)  { return std::isalnum((unsigned char)c) || c == '_'; }
bool is_digit(char c)        { return std::isdigit((unsigned char)c); }

} // namespace

std::vector<Token> lex_rust(std::string_view src) {
    std::vector<Token> tokens;
    tokens.reserve(src.size() / 4);

    const char* p   = src.data();
    const char* end = src.data() + src.size();

    auto push = [&](Token::Kind k, const char* begin, const char* finish) {
        if (finish > begin)
            tokens.push_back({k, std::string_view(begin, static_cast<size_t>(finish - begin))});
    };

    while (p < end) {
        // ---- Doc / line comment -----------------------------------------
        if (p + 1 < end && p[0] == '/' && p[1] == '/') {
            const char* s = p;
            while (p < end && *p != '\n') ++p;
            push(Token::Kind::Comment, s, p);
            continue;
        }
        // ---- Block comment ----------------------------------------------
        if (p + 1 < end && p[0] == '/' && p[1] == '*') {
            const char* s = p;
            p += 2;
            int depth = 1;
            while (p + 1 < end && depth > 0) {
                if (p[0] == '/' && p[1] == '*') { ++depth; p += 2; }
                else if (p[0] == '*' && p[1] == '/') { --depth; p += 2; }
                else ++p;
            }
            push(Token::Kind::Comment, s, p);
            continue;
        }
        // ---- Raw string r"…" or r#"…"# ----------------------------------
        if (*p == 'r' && p + 1 < end && (p[1] == '"' || p[1] == '#')) {
            const char* s = p++;
            int hashes = 0;
            while (p < end && *p == '#') { ++hashes; ++p; }
            if (p < end && *p == '"') {
                ++p; // opening "
                // find closing " followed by hashes
                while (p < end) {
                    if (*p == '"') {
                        const char* q = p + 1;
                        int h = 0;
                        while (q < end && *q == '#') { ++h; ++q; }
                        if (h >= hashes) { p = q; break; }
                    }
                    ++p;
                }
                push(Token::Kind::String, s, p);
                continue;
            }
            // not a raw string — treat 'r' as ident start
            p = s;
        }
        // ---- Byte string b"…" or byte literal b'…' ----------------------
        if (*p == 'b' && p + 1 < end && (p[1] == '"' || p[1] == '\'')) {
            char delim = p[1];
            const char* s = p;
            p += 2;
            while (p < end && *p != delim) {
                if (*p == '\\') ++p;
                if (p < end) ++p;
            }
            if (p < end) ++p;
            push(Token::Kind::String, s, p);
            continue;
        }
        // ---- String literal ---------------------------------------------
        if (*p == '"') {
            const char* s = p++;
            while (p < end && *p != '"') {
                if (*p == '\\') ++p;
                if (p < end) ++p;
            }
            if (p < end) ++p;
            push(Token::Kind::String, s, p);
            continue;
        }
        // ---- Character literal or lifetime ------------------------------
        if (*p == '\'') {
            // heuristic: if followed by identifier chars but NOT a closing quote, it's a lifetime
            const char* s = p++;
            if (p < end && is_ident_start(*p)) {
                while (p < end && is_ident_cont(*p)) ++p;
                if (p < end && *p == '\'') {
                    ++p; // char literal
                    push(Token::Kind::String, s, p);
                } else {
                    push(Token::Kind::Plain, s, p); // lifetime
                }
            } else {
                // char literal: single char or escape
                if (p < end && *p == '\\') ++p;
                if (p < end) ++p;
                if (p < end && *p == '\'') ++p;
                push(Token::Kind::String, s, p);
            }
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
                if (p < end && (*p == 'e' || *p == 'E')) {
                    ++p;
                    if (p < end && (*p == '+' || *p == '-')) ++p;
                    while (p < end && is_digit(*p)) ++p;
                }
            }
            // Type suffix: i8/u8/…/f32/f64 (attached without separator)
            if (p < end && (is_ident_start(*p))) {
                const char* ts = p;
                while (p < end && is_ident_cont(*p)) ++p;
                (void)ts; // suffix consumed, still part of number token
            }
            push(Token::Kind::Number, s, p);
            continue;
        }
        // ---- Macro invocation: name! — treat name as keyword-ish --------
        // (handled implicitly: 'name' gets lex'd as ident, '!' as plain)
        // ---- Identifier or keyword --------------------------------------
        if (is_ident_start(*p)) {
            const char* s = p;
            while (p < end && is_ident_cont(*p)) ++p;
            std::string_view word(s, static_cast<size_t>(p - s));
            push(is_rust_keyword(word) ? Token::Kind::Keyword : Token::Kind::Plain, s, p);
            continue;
        }
        // ---- Everything else --------------------------------------------
        push(Token::Kind::Plain, p, p + 1);
        ++p;
    }

    return tokens;
}

} // namespace batbox::tui::detail
