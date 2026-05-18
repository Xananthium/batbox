// src/tui/manual_lexers/ts_lexer.cpp
// ---------------------------------------------------------------------------
// Manual TypeScript / TSX lexer.
//
// TypeScript is a strict superset of JavaScript.  This lexer extends the JS
// lexer's behaviour with:
//
//   TS-specific keywords:
//     interface, type, enum, namespace, declare, as, satisfies, readonly,
//     public, private, protected, abstract, override, keyof, typeof, infer,
//     never, unknown, any — all emitted as Kind::Keyword.
//
//   Type annotations:
//     A bare identifier that follows ':' (possibly with surrounding
//     whitespace) in a non-object-literal context is tagged Kind::Keyword so
//     it gets the type-name colour.  The heuristic tracks whether we are
//     "expecting a type" (after ':', after '<', or after 'extends'/'implements').
//
//   Generics:
//     Angle-bracket pairs '<' ... '>' encountered while in
//     declaration/annotation position are treated as type-delimiters
//     (Kind::Plain) -- the identifiers inside them go through the normal
//     keyword/identifier path so type-parameter names and constraint keywords
//     ('extends') receive correct colouring.
//
//   Template literals:
//     Backtick strings are lexed span-by-span.  Inside ${...} the expression
//     content is re-lexed recursively so it receives full TS colourisation.
//
//   TSX tags:
//     Opening tags  <Foo>, <Foo />, <Foo bar={baz}>
//     Closing tags  </Foo>
//     The tag name and attribute names are emitted as Kind::Plain.
//     Attribute string values are emitted as Kind::String.
//     The delimiter characters < > / = are emitted as Kind::Operator.
//
//   Everything else follows the same rules as js_lexer.cpp:
//     Line comments // ... \n, block comments /* ... */
//     Double / single-quoted strings with escape sequences
//     Regex literals (heuristic: /.../ after operator or keyword)
//     Numeric literals: decimal, hex, octal, binary, BigInt n suffix
//     Plain identifiers and operators
// ---------------------------------------------------------------------------
#include "manual_lexers.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <string_view>
#include <vector>

namespace batbox::tui::detail {

namespace {

// ---------------------------------------------------------------------------
// Keyword table -- JS baseline plus all TS additions
// ---------------------------------------------------------------------------
constexpr std::array<std::string_view, 100> kTsKeywords = {{
    // JavaScript core
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
    "this", "throw", "true", "try",
    "undefined",
    "var", "void",
    "while", "with",
    "yield",
    // Async / generator
    "async", "await",
    // TypeScript keywords
    "abstract", "as", "asserts",
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
    // TS built-in utility / primitive type names (coloured as keywords)
    "any",
    "never",
    "unknown",
    "void",
    "null",
    "undefined",
    "object",
    "symbol",
    "Record", "Partial", "Required", "Readonly",
    "Pick", "Omit", "Exclude", "Extract",
    "ReturnType", "Parameters", "InstanceType",
    "Promise", "Array", "Map", "Set",
    "typeof",
}};

bool is_ts_keyword(std::string_view w) {
    return std::find(kTsKeywords.begin(), kTsKeywords.end(), w) != kTsKeywords.end();
}

bool is_ident_start(char c) { return std::isalpha((unsigned char)c) || c == '_' || c == '$'; }
bool is_ident_cont(char c)  { return std::isalnum((unsigned char)c) || c == '_' || c == '$'; }
bool is_digit(char c)        { return std::isdigit((unsigned char)c); }

// Returns true if the sequence after '<' looks like a TSX/JSX tag name.
bool looks_like_tag_start(const char* p, const char* end) {
    // p points at '<', peek at what follows
    if (p + 1 >= end) return false;
    char n = p[1];
    if (n == '/') {
        return (p + 2 < end && (std::isupper((unsigned char)p[2]) ||
                                 std::islower((unsigned char)p[2]) ||
                                 p[2] == '_'));
    }
    return std::isupper((unsigned char)n) || std::islower((unsigned char)n) || n == '_';
}

// ---------------------------------------------------------------------------
// TSX tag lexer helper.
// p points AT '<'.  Returns new position past the closing '>', or returns
// the original p if the parse fails (caller treats '<' as plain operator).
// ---------------------------------------------------------------------------
const char* lex_tsx_tag(const char* p, const char* end,
                         std::vector<Token>& out) {
    const char* start = p;

    auto push = [&](Token::Kind k, const char* b, const char* e) {
        if (e > b) out.push_back({k, std::string_view(b, static_cast<size_t>(e - b))});
    };

    // Emit '<'
    push(Token::Kind::Operator, p, p + 1);
    ++p;

    // Closing tag: </Foo>
    bool is_closing = (p < end && *p == '/');
    if (is_closing) {
        push(Token::Kind::Operator, p, p + 1);
        ++p;
    }

    // Tag name
    if (p >= end || !is_ident_start(*p)) return start; // bail
    const char* name_start = p;
    while (p < end && (is_ident_cont(*p) || *p == '.')) ++p; // e.g. MyComp.Sub
    push(Token::Kind::Plain, name_start, p);

    if (is_closing) {
        while (p < end && (*p == ' ' || *p == '\t')) ++p;
        if (p < end && *p == '>') { push(Token::Kind::Operator, p, p + 1); ++p; }
        return p;
    }

    // Attributes
    while (p < end && *p != '>' && !(*p == '/' && p + 1 < end && p[1] == '>')) {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') { ++p; continue; }
        if (is_ident_start(*p)) {
            const char* attr_s = p;
            while (p < end && (is_ident_cont(*p) || *p == '-')) ++p;
            push(Token::Kind::Plain, attr_s, p);
            if (p < end && *p == '=') {
                push(Token::Kind::Operator, p, p + 1); ++p;
                if (p < end && *p == '"') {
                    const char* sv = p++;
                    while (p < end && *p != '"') {
                        if (*p == '\\') ++p;
                        if (p < end) ++p;
                    }
                    if (p < end) ++p;
                    push(Token::Kind::String, sv, p);
                } else if (p < end && *p == '{') {
                    push(Token::Kind::Operator, p, p + 1); ++p;
                    int depth = 1;
                    const char* expr_s = p;
                    while (p < end && depth > 0) {
                        if (*p == '{') { ++depth; ++p; }
                        else if (*p == '}') {
                            --depth;
                            if (depth > 0) ++p;
                            else break;
                        } else {
                            ++p;
                        }
                    }
                    push(Token::Kind::Plain, expr_s, p);
                    if (p < end && *p == '}') { push(Token::Kind::Operator, p, p + 1); ++p; }
                }
            }
        } else {
            push(Token::Kind::Plain, p, p + 1);
            ++p;
        }
    }

    if (p + 1 < end && *p == '/' && p[1] == '>') {
        push(Token::Kind::Operator, p, p + 2);
        p += 2;
    } else if (p < end && *p == '>') {
        push(Token::Kind::Operator, p, p + 1);
        ++p;
    }

    return p;
}

// ---------------------------------------------------------------------------
// Forward declarations for mutual recursion.
// ---------------------------------------------------------------------------
void lex_ts_impl(const char* p, const char* end, std::vector<Token>& out,
                 bool top_level);

void lex_template_literal(const char*& p, const char* end,
                           std::vector<Token>& out);

// ---------------------------------------------------------------------------
// Template literal lexer.
// On entry *p == '`'.  Lexes the template literal including all ${...} spans,
// recursing into each expression with full TS treatment.
// ---------------------------------------------------------------------------
void lex_template_literal(const char*& p, const char* end,
                           std::vector<Token>& out) {
    auto push = [&](Token::Kind k, const char* b, const char* e) {
        if (e > b) out.push_back({k, std::string_view(b, static_cast<size_t>(e - b))});
    };

    const char* seg_start = p; // includes the opening backtick
    ++p; // consume '`'

    while (p < end) {
        if (*p == '`') {
            ++p;
            push(Token::Kind::String, seg_start, p);
            return;
        }
        if (*p == '\\') {
            ++p;
            if (p < end) ++p;
            continue;
        }
        if (*p == '$' && p + 1 < end && p[1] == '{') {
            push(Token::Kind::String, seg_start, p + 2);
            p += 2;
            int depth = 1;
            const char* expr_start = p;
            while (p < end && depth > 0) {
                if (*p == '`') {
                    // Nested template literal -- flush expr so far, recurse
                    if (p > expr_start) lex_ts_impl(expr_start, p, out, false);
                    lex_template_literal(p, end, out);
                    expr_start = p;
                    continue;
                }
                if (*p == '{') { ++depth; ++p; }
                else if (*p == '}') {
                    --depth;
                    if (depth == 0) break;
                    ++p;
                } else {
                    ++p;
                }
            }
            if (p > expr_start) lex_ts_impl(expr_start, p, out, false);
            if (p < end && *p == '}') {
                push(Token::Kind::String, p, p + 1);
                ++p;
            }
            seg_start = p;
            continue;
        }
        ++p;
    }
    // Unterminated template literal
    push(Token::Kind::String, seg_start, p);
}

// ---------------------------------------------------------------------------
// Core TS lexer shared by lex_template_literal (expressions) and public entry.
// ---------------------------------------------------------------------------
void lex_ts_impl(const char* p, const char* end, std::vector<Token>& out,
                 bool /*top_level*/) {
    auto push = [&](Token::Kind k, const char* b, const char* e) {
        if (e > b) out.push_back({k, std::string_view(b, static_cast<size_t>(e - b))});
    };

    bool after_operator = true;  // heuristic: can '/' start a regex?
    bool after_ident     = false; // last non-ws token was an identifier/number
    bool expect_type    = false; // after ':', '<', 'extends', 'implements'

    while (p < end) {
        // ---- Whitespace (preserved so after_operator survives '= /re/') -----
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            push(Token::Kind::Plain, p, p + 1);
            ++p;
            continue;
        }
        // ---- Line comment --------------------------------------------------
        if (p + 1 < end && p[0] == '/' && p[1] == '/') {
            const char* s = p;
            while (p < end && *p != '\n') ++p;
            push(Token::Kind::Comment, s, p);
            after_operator = false;
            after_ident    = false;
            expect_type    = false;
            continue;
        }
        // ---- Block comment -------------------------------------------------
        if (p + 1 < end && p[0] == '/' && p[1] == '*') {
            const char* s = p;
            p += 2;
            while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) ++p;
            if (p + 1 < end) p += 2;
            push(Token::Kind::Comment, s, p);
            after_operator = false;
            after_ident    = false;
            expect_type    = false;
            continue;
        }
        // ---- Template literal ----------------------------------------------
        if (*p == '`') {
            lex_template_literal(p, end, out);
            after_operator = false;
            after_ident    = false;
            expect_type    = false;
            continue;
        }
        // ---- Regex literal (heuristic: '/' after operator / keyword) ------
        if (*p == '/' && after_operator) {
            const char* s = p++;
            while (p < end && *p != '/' && *p != '\n') {
                if (*p == '\\') ++p;
                if (p < end) ++p;
            }
            if (p < end && *p == '/') {
                ++p;
                while (p < end && std::isalpha((unsigned char)*p)) ++p;
                push(Token::Kind::String, s, p);
                after_operator = false;
                after_ident    = false;
                expect_type    = false;
                continue;
            }
            // Not a regex -- treat as plain '/'
            push(Token::Kind::Plain, s, p);
            after_operator = true;
            continue;
        }
        // ---- String literal ------------------------------------------------
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
            after_ident    = false;
            expect_type    = false;
            continue;
        }
        // ---- TSX / JSX tag  <Foo> </Foo> <Foo /> --------------------------
        // Recognise TSX when '<' is in expression position (after an operator
        // like '=', '(', 'return') but NOT after an identifier (which would
        // make '<' a comparison or generic opener).
        if (*p == '<' && after_operator && !after_ident && looks_like_tag_start(p, end)) {
            const char* new_p = lex_tsx_tag(p, end, out);
            if (new_p != p) {
                p = new_p;
                after_operator = false;
                after_ident    = false;
                expect_type    = false;
                continue;
            }
        }
        // ---- Numeric literal -----------------------------------------------
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
            after_ident    = true;  // numbers are ident-like for < purposes
            expect_type    = false;
            continue;
        }
        // ---- Identifier or keyword ----------------------------------------
        if (is_ident_start(*p)) {
            const char* s = p;
            while (p < end && is_ident_cont(*p)) ++p;
            std::string_view word(s, static_cast<size_t>(p - s));
            bool kw = is_ts_keyword(word);
            // In type-annotation position, colour unknown identifiers as Keyword
            bool as_type = expect_type && !kw;
            Token::Kind kind = (kw || as_type) ? Token::Kind::Keyword : Token::Kind::Plain;
            push(kind, s, p);
            // 'extends' / 'implements' introduce a type name on the right
            expect_type    = (word == "extends" || word == "implements");
            after_operator = kw; // keywords can precede regex
            after_ident    = true;  // identifiers and keywords block TSX '<'
            continue;
        }
        // ---- Operators / punctuation --------------------------------------
        {
            char c = *p;
            if (c == ':' || c == '<') {
                expect_type = true;
            } else if (c == '>' || c == ',' || c == ')' || c == '{' ||
                       c == '}' || c == '[' || c == ']' || c == ';') {
                expect_type = false;
            }
            bool is_op = c == '(' || c == ')' || c == '[' || c == ']' ||
                         c == '{' || c == '}' || c == ';' || c == ',' ||
                         c == '=' || c == '!' || c == '<' || c == '>' ||
                         c == '+' || c == '-' || c == '*' || c == '%' ||
                         c == '&' || c == '|' || c == '^' || c == '~' ||
                         c == '?' || c == ':' || c == '.';
            push(Token::Kind::Plain, p, p + 1);
            after_operator = is_op;
            after_ident    = false; // operators clear the ident context
            ++p;
        }
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------
std::vector<Token> lex_typescript(std::string_view src) {
    std::vector<Token> tokens;
    tokens.reserve(src.size() / 4);
    lex_ts_impl(src.data(), src.data() + src.size(), tokens, true);
    return tokens;
}

} // namespace batbox::tui::detail
