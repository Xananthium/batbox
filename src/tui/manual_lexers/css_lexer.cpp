// src/tui/manual_lexers/css_lexer.cpp
// ---------------------------------------------------------------------------
// Manual CSS / SCSS / Sass lexer.
//
// Recognises:
//   Selectors (before '{'):
//     tag names, .class, #id, [attr], :pseudo, ::pseudo-element, * — Keyword
//   Properties (inside '{ ... }'):
//     property-name — Plain; value tokens — String/Number/Keyword as appropriate
//   Values:
//     numbers with units (px em rem % vh vw deg s ms fr) — Number
//     bare numbers — Number
//     hex colors (#abc #aabbcc #aabbccdd) — String
//     function calls (rgb rgba hsl url calc var ...) — identifier Plain, parens Plain
//   At-rules:
//     @media @import @keyframes @font-face @supports @charset — Keyword
//     SCSS: @mixin @include @if @else @for @each — Keyword
//   Comments:
//     /* ... */ block comment — Comment
//     SCSS // line comment — Comment
//   SCSS extensions:
//     $variable — String (variable declarations and references)
//     & parent selector — Keyword
// ---------------------------------------------------------------------------
#include "manual_lexers.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>
#include <vector>

namespace batbox::tui::detail {

namespace {

// CSS at-rule keywords (without the '@' prefix — we match after consuming '@').
constexpr std::array<std::string_view, 11> kAtRules = {{
    "media", "import", "keyframes", "font-face", "supports", "charset",
    "namespace", "layer", "container",
    // SCSS at-rules
    "mixin", "include",
}};

// SCSS flow control at-rules (short names, separate array for clarity).
constexpr std::array<std::string_view, 4> kScssFlowRules = {{
    "if", "else", "for", "each",
}};

bool is_at_keyword(std::string_view w) {
    return std::find(kAtRules.begin(), kAtRules.end(), w) != kAtRules.end() ||
           std::find(kScssFlowRules.begin(), kScssFlowRules.end(), w) != kScssFlowRules.end();
}

bool is_ident_start(char c) { return std::isalpha((unsigned char)c) || c == '_' || c == '-'; }
bool is_ident_cont(char c)  {
    return std::isalnum((unsigned char)c) || c == '_' || c == '-';
}
bool is_hex_digit(char c)    { return std::isxdigit((unsigned char)c); }
bool is_digit(char c)        { return std::isdigit((unsigned char)c); }

// Collect a CSS identifier starting at *p (caller ensures is_ident_start(*p)).
// Advances p past the identifier.  Returns the identifier as string_view.
std::string_view collect_ident(const char*& p, const char* end) {
    const char* s = p;
    while (p < end && is_ident_cont(*p)) ++p;
    return {s, static_cast<size_t>(p - s)};
}

// Push a single-character Plain token.
void push_plain_char(std::vector<Token>& tokens, const char* p) {
    tokens.push_back({Token::Kind::Plain, std::string_view(p, 1)});
}

} // namespace

// ---------------------------------------------------------------------------
// lex_css — main entry point
// ---------------------------------------------------------------------------
// State machine: we track whether we are inside a rule block ({...}) with a
// nesting counter.  When depth == 0, text up to '{' is selector territory.
// When depth > 0, text follows property/value grammar.
// ---------------------------------------------------------------------------
std::vector<Token> lex_css(std::string_view src) {
    std::vector<Token> tokens;
    tokens.reserve(src.size() / 4);

    const char* p   = src.data();
    const char* end = src.data() + src.size();
    int depth = 0; // brace nesting depth

    auto push = [&](Token::Kind k, const char* begin, const char* finish) {
        if (finish > begin)
            tokens.push_back({k, std::string_view(begin, static_cast<size_t>(finish - begin))});
    };

    while (p < end) {
        // ---- SCSS line comment // -------------------------------------------
        if (p + 1 < end && p[0] == '/' && p[1] == '/') {
            const char* s = p;
            while (p < end && *p != '\n') ++p;
            push(Token::Kind::Comment, s, p);
            continue;
        }

        // ---- Block comment /* ... */ ----------------------------------------
        if (p + 1 < end && p[0] == '/' && p[1] == '*') {
            const char* s = p;
            p += 2;
            while (p < end && !(p + 1 < end && p[0] == '*' && p[1] == '/')) ++p;
            if (p + 1 < end) p += 2; // consume */
            else p = end;             // unclosed comment — consume to EOF
            push(Token::Kind::Comment, s, p);
            continue;
        }

        // ---- Opening brace --------------------------------------------------
        if (*p == '{') {
            ++depth;
            push_plain_char(tokens, p);
            ++p;
            continue;
        }

        // ---- Closing brace --------------------------------------------------
        if (*p == '}') {
            if (depth > 0) --depth;
            push_plain_char(tokens, p);
            ++p;
            continue;
        }

        // ---- At-rule @keyword -----------------------------------------------
        if (*p == '@') {
            const char* s = p++;
            // collect the at-rule name (may contain letters and hyphens)
            const char* name_start = p;
            while (p < end && (std::isalpha((unsigned char)*p) || *p == '-')) ++p;
            std::string_view name(name_start, static_cast<size_t>(p - name_start));
            push(is_at_keyword(name) ? Token::Kind::Keyword : Token::Kind::Plain, s, p);
            continue;
        }

        // ---- SCSS $variable -------------------------------------------------
        if (*p == '$') {
            const char* s = p++;
            while (p < end && (is_ident_cont(*p))) ++p;
            push(Token::Kind::String, s, p);
            continue;
        }

        // =====================================================================
        // SELECTOR CONTEXT (depth == 0)
        // =====================================================================
        if (depth == 0) {
            // . class selector
            if (*p == '.') {
                const char* s = p++;
                while (p < end && is_ident_cont(*p)) ++p;
                push(Token::Kind::Keyword, s, p);
                continue;
            }
            // # id selector — but check hex color fallthrough: selectors have
            // no digit immediately after # (hex colors only appear inside blocks).
            if (*p == '#') {
                const char* s = p++;
                while (p < end && is_ident_cont(*p)) ++p;
                push(Token::Kind::Keyword, s, p);
                continue;
            }
            // [ attribute selector ]
            if (*p == '[') {
                const char* s = p;
                while (p < end && *p != ']' && *p != '{' && *p != '\n') ++p;
                if (p < end && *p == ']') ++p;
                push(Token::Kind::Keyword, s, p);
                continue;
            }
            // : and :: pseudo-classes / pseudo-elements
            if (*p == ':') {
                const char* s = p++;
                if (p < end && *p == ':') ++p; // ::
                while (p < end && (is_ident_cont(*p) || *p == '(')) {
                    if (*p == '(') {
                        ++p;
                        // consume until ')' (simple, one level)
                        while (p < end && *p != ')') ++p;
                        if (p < end) ++p;
                        break;
                    }
                    ++p;
                }
                push(Token::Kind::Keyword, s, p);
                continue;
            }
            // * universal selector and other operators/combinators
            if (*p == '*' || *p == '>' || *p == '+' || *p == '~' || *p == ',') {
                push(Token::Kind::Keyword, p, p + 1);
                ++p;
                continue;
            }
            // Tag-name / element selector (starts with alpha or underscore)
            if (is_ident_start(*p) && *p != '-') {
                const char* s = p;
                (void)collect_ident(p, end);
                push(Token::Kind::Keyword, s, p);
                continue;
            }
            // & SCSS parent selector
            if (*p == '&') {
                push(Token::Kind::Keyword, p, p + 1);
                ++p;
                continue;
            }
            // Whitespace and anything else in selector context → Plain
            push_plain_char(tokens, p);
            ++p;
            continue;
        }

        // =====================================================================
        // BLOCK CONTEXT (depth > 0) — property: value; grammar
        // =====================================================================

        // Hex color #abc #aabbcc #aabbccdd
        if (*p == '#') {
            const char* s = p++;
            size_t hex_count = 0;
            while (p < end && is_hex_digit(*p) && hex_count < 8) { ++p; ++hex_count; }
            // Valid hex color lengths: 3, 4, 6, 8
            if (hex_count == 3 || hex_count == 4 || hex_count == 6 || hex_count == 8) {
                push(Token::Kind::String, s, p);
            } else {
                // Not a valid hex color — emit as Plain (id selectors inside @keyframes etc.)
                push(Token::Kind::Plain, s, p);
            }
            continue;
        }

        // Quoted string values "..." or '...'
        if (*p == '"' || *p == '\'') {
            char delim = *p;
            const char* s = p++;
            while (p < end && *p != delim && *p != '\n') {
                if (*p == '\\') ++p;
                if (p < end) ++p;
            }
            if (p < end) ++p;
            push(Token::Kind::String, s, p);
            continue;
        }

        // Numeric value (possibly followed by a unit)
        if (is_digit(*p) || (*p == '.' && p + 1 < end && is_digit(p[1])) ||
            (*p == '-' && p + 1 < end && (is_digit(p[1]) || (p[1] == '.' && p + 2 < end && is_digit(p[2]))))) {
            const char* s = p;
            if (*p == '-') ++p;
            while (p < end && is_digit(*p)) ++p;
            if (p < end && *p == '.') { ++p; while (p < end && is_digit(*p)) ++p; }
            // Optional unit suffix: px em rem % vh vw deg s ms fr ch ex lh dvh dvw svh svw
            if (p < end && std::isalpha((unsigned char)*p)) {
                const char* u = p;
                while (p < end && std::isalpha((unsigned char)*p)) ++p;
                // Accept the unit as part of the number token
                (void)u;
            }
            if (p < end && *p == '%') ++p; // percent unit
            push(Token::Kind::Number, s, p);
            continue;
        }

        // Identifier (property name, function name, keyword value, color name)
        if (is_ident_start(*p)) {
            const char* s = p;
            std::string_view word = collect_ident(p, end);
            // Peek: if next non-space char is '(' it's a function call name
            // — emit the name as Plain (function identifier), '(' as Plain.
            // Values like 'inherit', 'none', 'auto', 'solid', etc. → Plain.
            // CSS does not have value-level keywords we distinguish separately;
            // the property name itself is also Plain (identifier colour = Fg).
            push(Token::Kind::Plain, s, p);
            continue;
        }

        // Colon separating property from value → Plain operator
        if (*p == ':') {
            push_plain_char(tokens, p);
            ++p;
            continue;
        }

        // Semicolon, parentheses, comma, exclamation (for !important)
        if (*p == ';' || *p == '(' || *p == ')' || *p == ',' || *p == '!') {
            push_plain_char(tokens, p);
            ++p;
            continue;
        }

        // Whitespace and anything remaining → Plain
        push_plain_char(tokens, p);
        ++p;
    }

    return tokens;
}

} // namespace batbox::tui::detail
