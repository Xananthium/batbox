// tests/unit/test_css_lexer.cpp
// ---------------------------------------------------------------------------
// doctest suite for batbox::tui::detail::lex_css()
//
// Coverage:
//   1.  Simple rule: selector + property:value pair
//   2.  Selector types: .class, #id, :hover pseudo-class, ::before pseudo-element
//   3.  Tag selector recognised as Keyword
//   4.  Attribute selector [attr=value] recognised as Keyword
//   5.  Universal selector * recognised as Keyword
//   6.  Property:value pair — property is Plain, value tokens present
//   7.  Hex colors: #abc (3-digit), #aabbcc (6-digit), #aabbccdd (8-digit) → String
//   8.  Numeric values with units — px, em, rem, %, vh, vw → Number
//   9.  Numeric values with other units: deg, s, ms, fr → Number
//   10. calc() function — outer identifier Plain, numeric args Number
//   11. At-rule @media → Keyword
//   12. At-rule @import → Keyword
//   13. At-rule @keyframes → Keyword
//   14. At-rule @font-face → Keyword
//   15. Block comment /* ... */ → Comment
//   16. SCSS $variable → String
//   17. SCSS @mixin → Keyword
//   18. SCSS // line comment → Comment
//   19. SCSS & parent selector → Keyword (in selector context)
//   20. No crash on empty input
//   21. No crash on unclosed block comment
//   22. Negative numeric values (e.g. margin: -4px) → Number
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "manual_lexers/manual_lexers.hpp"

using namespace batbox::tui::detail;

namespace {

// Return true if tokens contains at least one token of the given kind whose
// text contains substr.
bool has_token(const std::vector<Token>& tokens, Token::Kind kind, std::string_view substr) {
    for (const auto& t : tokens) {
        if (t.kind == kind && t.text.find(substr) != std::string_view::npos)
            return true;
    }
    return false;
}

// Return true if every token in tokens is non-empty.
bool all_nonempty(const std::vector<Token>& tokens) {
    for (const auto& t : tokens)
        if (t.text.empty()) return false;
    return true;
}

} // namespace

// ============================================================================
// 1: Simple rule
// ============================================================================

TEST_CASE("css lexer: simple rule tokenises without error") {
    auto tokens = lex_css("p { color: red; }");
    CHECK(!tokens.empty());
    CHECK(all_nonempty(tokens));
}

// ============================================================================
// 2: Selector types
// ============================================================================

TEST_CASE("css lexer: .class selector recognised as Keyword") {
    auto tokens = lex_css(".container { display: flex; }");
    CHECK(has_token(tokens, Token::Kind::Keyword, ".container"));
}

TEST_CASE("css lexer: #id selector recognised as Keyword") {
    auto tokens = lex_css("#header { margin: 0; }");
    CHECK(has_token(tokens, Token::Kind::Keyword, "#header"));
}

TEST_CASE("css lexer: :hover pseudo-class recognised as Keyword") {
    auto tokens = lex_css("a:hover { color: blue; }");
    CHECK(has_token(tokens, Token::Kind::Keyword, ":hover"));
}

TEST_CASE("css lexer: ::before pseudo-element recognised as Keyword") {
    auto tokens = lex_css("p::before { content: ''; }");
    CHECK(has_token(tokens, Token::Kind::Keyword, "::before"));
}

// ============================================================================
// 3: Tag selector
// ============================================================================

TEST_CASE("css lexer: tag name selector recognised as Keyword") {
    auto tokens = lex_css("div { padding: 8px; }");
    CHECK(has_token(tokens, Token::Kind::Keyword, "div"));
}

// ============================================================================
// 4: Attribute selector
// ============================================================================

TEST_CASE("css lexer: [attr=value] selector recognised as Keyword") {
    auto tokens = lex_css("[type=\"text\"] { border: 1px solid; }");
    // The entire [type="text"] span should be a Keyword token.
    CHECK(has_token(tokens, Token::Kind::Keyword, "type"));
}

// ============================================================================
// 5: Universal selector
// ============================================================================

TEST_CASE("css lexer: * universal selector recognised as Keyword") {
    auto tokens = lex_css("* { box-sizing: border-box; }");
    CHECK(has_token(tokens, Token::Kind::Keyword, "*"));
}

// ============================================================================
// 6: Property:value — property is Plain
// ============================================================================

TEST_CASE("css lexer: property name emitted as Plain token") {
    auto tokens = lex_css("p { font-size: 16px; }");
    CHECK(has_token(tokens, Token::Kind::Plain, "font-size"));
}

// ============================================================================
// 7: Hex colors
// ============================================================================

TEST_CASE("css lexer: 3-digit hex color #abc recognised as String") {
    auto tokens = lex_css("a { color: #abc; }");
    CHECK(has_token(tokens, Token::Kind::String, "#abc"));
}

TEST_CASE("css lexer: 6-digit hex color #aabbcc recognised as String") {
    auto tokens = lex_css("a { color: #aabbcc; }");
    CHECK(has_token(tokens, Token::Kind::String, "#aabbcc"));
}

TEST_CASE("css lexer: 8-digit hex color #aabbccdd recognised as String") {
    auto tokens = lex_css("a { color: #aabbccdd; }");
    CHECK(has_token(tokens, Token::Kind::String, "#aabbccdd"));
}

// ============================================================================
// 8: Units — px em rem % vh vw
// ============================================================================

TEST_CASE("css lexer: value with px unit recognised as Number") {
    auto tokens = lex_css("p { margin: 16px; }");
    CHECK(has_token(tokens, Token::Kind::Number, "16px"));
}

TEST_CASE("css lexer: value with em unit recognised as Number") {
    auto tokens = lex_css("p { font-size: 1.5em; }");
    CHECK(has_token(tokens, Token::Kind::Number, "1.5em"));
}

TEST_CASE("css lexer: value with rem unit recognised as Number") {
    auto tokens = lex_css("p { margin: 2rem; }");
    CHECK(has_token(tokens, Token::Kind::Number, "2rem"));
}

TEST_CASE("css lexer: value with % unit recognised as Number") {
    auto tokens = lex_css("div { width: 100%; }");
    CHECK(has_token(tokens, Token::Kind::Number, "100%"));
}

TEST_CASE("css lexer: value with vh unit recognised as Number") {
    auto tokens = lex_css("section { height: 50vh; }");
    CHECK(has_token(tokens, Token::Kind::Number, "50vh"));
}

TEST_CASE("css lexer: value with vw unit recognised as Number") {
    auto tokens = lex_css("section { width: 80vw; }");
    CHECK(has_token(tokens, Token::Kind::Number, "80vw"));
}

// ============================================================================
// 9: Additional units — deg s ms fr
// ============================================================================

TEST_CASE("css lexer: value with deg unit recognised as Number") {
    auto tokens = lex_css("div { transform: rotate(45deg); }");
    CHECK(has_token(tokens, Token::Kind::Number, "45deg"));
}

TEST_CASE("css lexer: value with s unit recognised as Number") {
    auto tokens = lex_css("div { transition: all 0.3s ease; }");
    CHECK(has_token(tokens, Token::Kind::Number, "0.3s"));
}

TEST_CASE("css lexer: value with ms unit recognised as Number") {
    auto tokens = lex_css("div { animation-duration: 200ms; }");
    CHECK(has_token(tokens, Token::Kind::Number, "200ms"));
}

TEST_CASE("css lexer: value with fr unit recognised as Number") {
    auto tokens = lex_css(".grid { grid-template-columns: 1fr 2fr; }");
    CHECK(has_token(tokens, Token::Kind::Number, "1fr"));
    CHECK(has_token(tokens, Token::Kind::Number, "2fr"));
}

// ============================================================================
// 10: calc() function
// ============================================================================

TEST_CASE("css lexer: calc() — numeric args recognised as Number") {
    auto tokens = lex_css("div { width: calc(100% - 16px); }");
    // Numeric values inside calc() should be Numbers.
    CHECK(has_token(tokens, Token::Kind::Number, "100%"));
    CHECK(has_token(tokens, Token::Kind::Number, "16px"));
}

// ============================================================================
// 11–14: At-rules
// ============================================================================

TEST_CASE("css lexer: @media at-rule recognised as Keyword") {
    auto tokens = lex_css("@media screen and (max-width: 768px) { }");
    CHECK(has_token(tokens, Token::Kind::Keyword, "@media"));
}

TEST_CASE("css lexer: @import at-rule recognised as Keyword") {
    auto tokens = lex_css("@import url('style.css');");
    CHECK(has_token(tokens, Token::Kind::Keyword, "@import"));
}

TEST_CASE("css lexer: @keyframes at-rule recognised as Keyword") {
    auto tokens = lex_css("@keyframes slide { from { } to { } }");
    CHECK(has_token(tokens, Token::Kind::Keyword, "@keyframes"));
}

TEST_CASE("css lexer: @font-face at-rule recognised as Keyword") {
    auto tokens = lex_css("@font-face { font-family: 'MyFont'; }");
    CHECK(has_token(tokens, Token::Kind::Keyword, "@font-face"));
}

// ============================================================================
// 15: Block comment
// ============================================================================

TEST_CASE("css lexer: block comment /* ... */ recognised as Comment") {
    auto tokens = lex_css("/* main layout */ div { }");
    CHECK(has_token(tokens, Token::Kind::Comment, "main layout"));
}

TEST_CASE("css lexer: multi-line block comment recognised as Comment") {
    auto tokens = lex_css("/*\n * Reset styles\n * Author: Dev\n */\nbody { }");
    CHECK(has_token(tokens, Token::Kind::Comment, "Reset styles"));
}

// ============================================================================
// 16: SCSS $variable
// ============================================================================

TEST_CASE("css lexer: SCSS $variable recognised as String") {
    auto tokens = lex_css("$primary-color: #3498db;");
    CHECK(has_token(tokens, Token::Kind::String, "$primary-color"));
}

TEST_CASE("css lexer: SCSS $variable reference in value recognised as String") {
    auto tokens = lex_css("a { color: $link-color; }");
    CHECK(has_token(tokens, Token::Kind::String, "$link-color"));
}

// ============================================================================
// 17: SCSS @mixin / @include
// ============================================================================

TEST_CASE("css lexer: SCSS @mixin recognised as Keyword") {
    auto tokens = lex_css("@mixin flex-center { display: flex; }");
    CHECK(has_token(tokens, Token::Kind::Keyword, "@mixin"));
}

TEST_CASE("css lexer: SCSS @include recognised as Keyword") {
    auto tokens = lex_css(".card { @include flex-center; }");
    CHECK(has_token(tokens, Token::Kind::Keyword, "@include"));
}

// ============================================================================
// 18: SCSS // line comment
// ============================================================================

TEST_CASE("css lexer: SCSS // line comment recognised as Comment") {
    auto tokens = lex_css("// SCSS line comment\ndiv { }");
    CHECK(has_token(tokens, Token::Kind::Comment, "SCSS line comment"));
}

// ============================================================================
// 19: SCSS & parent selector
// ============================================================================

TEST_CASE("css lexer: SCSS & parent selector in selector context recognised as Keyword") {
    // & appears as a selector (depth == 0 at start, or nested inside @mixin/rule).
    // In a flat selector context & should be Keyword.
    auto tokens = lex_css("&:hover { color: red; }");
    CHECK(has_token(tokens, Token::Kind::Keyword, "&"));
}

// ============================================================================
// 20: Empty input
// ============================================================================

TEST_CASE("css lexer: empty input returns empty token vector without crash") {
    auto tokens = lex_css("");
    CHECK(tokens.empty());
}

// ============================================================================
// 21: Unclosed block comment (robustness)
// ============================================================================

TEST_CASE("css lexer: unclosed block comment does not crash") {
    auto tokens = lex_css("/* this comment never closes");
    CHECK(has_token(tokens, Token::Kind::Comment, "this comment never closes"));
}

// ============================================================================
// 22: Negative numeric value
// ============================================================================

TEST_CASE("css lexer: negative numeric value recognised as Number") {
    auto tokens = lex_css("div { margin-top: -4px; }");
    CHECK(has_token(tokens, Token::Kind::Number, "-4px"));
}
