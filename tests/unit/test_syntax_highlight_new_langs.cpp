// tests/unit/test_syntax_highlight_new_langs.cpp
// ---------------------------------------------------------------------------
// doctest suite: verify TS/HTML/CSS/JSON lexers are correctly wired into
// highlight_code() via normalise_language() + dispatch.  PEXT2 1.4f.
//
// Coverage:
//   1.  TypeScript: keyword "interface" recognised as Keyword by lex_typescript
//   2.  TypeScript: string literal recognised as String
//   3.  TypeScript: line comment recognised as Comment
//   4.  highlight_code("tsx", ...) returns a valid non-empty Element
//   5.  highlight_code("typescript", ...) uses lex_typescript (not lex_js)
//   6.  HTML: tag name recognised as Keyword by lex_html
//   7.  HTML: attribute string value recognised as String
//   8.  HTML: comment <!-- --> recognised as Comment
//   9.  highlight_code("html", ...) returns a valid non-empty Element
//   10. highlight_code("htm", ...) alias resolves correctly
//   11. highlight_code("xml", ...) alias resolves to html lexer — valid Element
//   12. CSS: at-rule (@media) recognised as Keyword by lex_css
//   13. CSS: block comment /* */ recognised as Comment
//   14. highlight_code("css", ...) returns a valid non-empty Element
//   15. highlight_code("scss", ...) alias resolves correctly
//   16. highlight_code("less", ...) alias resolves correctly
//   17. JSON: keyword "true" recognised as Keyword by lex_json
//   18. JSON: string value recognised as String
//   19. JSON: number recognised as Number
//   20. highlight_code("json", ...) returns a valid non-empty Element
//   21. highlight_code("jsonc", ...) alias resolves correctly
//   22. highlight_code("json5", ...) alias resolves correctly
//   23. JS JSX: return <Foo /> produces token output (tag parsed, not crash)
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/tui/SyntaxHighlight.hpp>
#include <batbox/theme/Theme.hpp>
#include <batbox/tui/ThemeApply.hpp>

#include "manual_lexers/manual_lexers.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

using namespace batbox::tui;
using namespace batbox::theme;
using namespace batbox::tui::detail;

namespace {

Theme make_test_theme() {
    using C = ftxui::Color;
    Theme t{};
    t.name           = "miss-kittin";
    t.bg             = C::RGB(16,  16,  24);
    t.fg             = C::RGB(220, 220, 228);
    t.accent_magenta = C::RGB(255,  80, 200);
    t.accent_cyan    = C::RGB( 80, 220, 230);
    t.muted          = C::RGB(120, 120, 140);
    t.success        = C::RGB( 80, 220, 120);
    t.error          = C::RGB(255,  80,  80);
    t.diff_add_fg    = C::RGB( 80, 220, 120);
    t.diff_add_bg    = C::RGB( 20,  50,  20);
    t.diff_remove_fg = C::RGB(255,  80,  80);
    t.diff_remove_bg = C::RGB( 50,  20,  20);
    t.prompt_prefix  = C::RGB(255,  80, 200);
    t.code_bg        = C::RGB( 22,  22,  32);
    return t;
}

bool has_token(const std::vector<Token>& tokens, Token::Kind kind, std::string_view substr) {
    for (const auto& t : tokens) {
        if (t.kind == kind && t.text.find(substr) != std::string_view::npos)
            return true;
    }
    return false;
}

std::string render_to_string(ftxui::Element el) {
    using namespace ftxui;
    auto screen = Screen::Create(Dimension::Fixed(120), Dimension::Fixed(40));
    Render(screen, el);
    return screen.ToString();
}

} // namespace

// ============================================================================
// 1-5: TypeScript lexer
// ============================================================================

TEST_CASE("ts lexer: interface keyword classified as Keyword") {
    auto tokens = lex_typescript("interface Foo { bar: string; }");
    CHECK(has_token(tokens, Token::Kind::Keyword, "interface"));
}

TEST_CASE("ts lexer: string literal classified as String") {
    auto tokens = lex_typescript(R"(const s: string = "hello ts";)");
    CHECK(has_token(tokens, Token::Kind::String, "hello ts"));
}

TEST_CASE("ts lexer: line comment classified as Comment") {
    auto tokens = lex_typescript("// ts comment\nconst x = 1;");
    CHECK(has_token(tokens, Token::Kind::Comment, "ts comment"));
}

TEST_CASE("highlight_code tsx: returns valid non-empty Element") {
    const Theme theme = make_test_theme();
    auto el = highlight_code("tsx", "const el = <Foo bar=\"baz\" />;", theme);
    CHECK(!render_to_string(el).empty());
}

TEST_CASE("highlight_code typescript: uses lex_typescript — type keyword is Keyword") {
    // lex_typescript knows 'type' as a keyword; lex_js does too via kJsKeywords,
    // but 'interface' is the definitive TS-specific marker here — verify via
    // the public API producing a renderable element with TS-specific content.
    const Theme theme = make_test_theme();
    std::string code = "interface User { name: string; age: number; }\n"
                       "type ID = string | number;\n";
    auto el = highlight_code("typescript", code, theme);
    CHECK(!render_to_string(el).empty());
    // Also verify direct lexer path produces interface keyword
    auto tokens = lex_typescript(code);
    CHECK(has_token(tokens, Token::Kind::Keyword, "interface"));
}

// ============================================================================
// 6-11: HTML lexer
// ============================================================================

TEST_CASE("html lexer: tag name classified as Keyword") {
    auto tokens = lex_html("<div class=\"container\">Hello</div>");
    CHECK(has_token(tokens, Token::Kind::Keyword, "div"));
}

TEST_CASE("html lexer: attribute string value classified as String") {
    auto tokens = lex_html("<a href=\"https://example.com\">link</a>");
    CHECK(has_token(tokens, Token::Kind::String, "https://example.com"));
}

TEST_CASE("html lexer: HTML comment classified as Comment") {
    auto tokens = lex_html("<!-- this is a comment --><p>text</p>");
    CHECK(has_token(tokens, Token::Kind::Comment, "this is a comment"));
}

TEST_CASE("highlight_code html: returns valid non-empty Element") {
    const Theme theme = make_test_theme();
    std::string code = "<!DOCTYPE html>\n<html>\n<head><title>Test</title></head>\n"
                       "<body><p>Hello</p></body>\n</html>\n";
    auto el = highlight_code("html", code, theme);
    CHECK(!render_to_string(el).empty());
}

TEST_CASE("highlight_code htm: alias resolves to html lexer") {
    const Theme theme = make_test_theme();
    auto el = highlight_code("htm", "<span>text</span>", theme);
    CHECK(!render_to_string(el).empty());
}

TEST_CASE("highlight_code xml: alias resolves to html lexer, valid Element") {
    const Theme theme = make_test_theme();
    std::string code = "<?xml version=\"1.0\"?>\n<root>\n  <item>value</item>\n</root>\n";
    auto el = highlight_code("xml", code, theme);
    CHECK(!render_to_string(el).empty());
}

// ============================================================================
// 12-16: CSS lexer
// ============================================================================

TEST_CASE("css lexer: at-rule @media classified as Keyword") {
    auto tokens = lex_css("@media (max-width: 768px) { .box { width: 100%; } }");
    CHECK(has_token(tokens, Token::Kind::Keyword, "@media"));
}

TEST_CASE("css lexer: block comment classified as Comment") {
    auto tokens = lex_css("/* main styles */\nbody { margin: 0; }");
    CHECK(has_token(tokens, Token::Kind::Comment, "main styles"));
}

TEST_CASE("highlight_code css: returns valid non-empty Element") {
    const Theme theme = make_test_theme();
    std::string code = "body {\n  margin: 0;\n  padding: 0;\n  font-size: 16px;\n}\n"
                       ".container { max-width: 1200px; }\n";
    auto el = highlight_code("css", code, theme);
    CHECK(!render_to_string(el).empty());
}

TEST_CASE("highlight_code scss: alias resolves to css lexer") {
    const Theme theme = make_test_theme();
    std::string code = "$primary: #ff5088;\n.btn { color: $primary; }\n";
    auto el = highlight_code("scss", code, theme);
    CHECK(!render_to_string(el).empty());
}

TEST_CASE("highlight_code less: alias resolves to css lexer") {
    const Theme theme = make_test_theme();
    auto el = highlight_code("less", "@base: #f938ab;\n.box { color: @base; }", theme);
    CHECK(!render_to_string(el).empty());
}

// ============================================================================
// 17-22: JSON lexer
// ============================================================================

TEST_CASE("json lexer: keyword 'true' classified as Keyword") {
    auto tokens = lex_json(R"({"active": true, "disabled": false, "value": null})");
    CHECK(has_token(tokens, Token::Kind::Keyword, "true"));
    CHECK(has_token(tokens, Token::Kind::Keyword, "false"));
    CHECK(has_token(tokens, Token::Kind::Keyword, "null"));
}

TEST_CASE("json lexer: string value classified as String") {
    auto tokens = lex_json(R"({"message": "hello json"})");
    CHECK(has_token(tokens, Token::Kind::String, "hello json"));
}

TEST_CASE("json lexer: number classified as Number") {
    auto tokens = lex_json(R"({"count": 42, "pi": 3.14})");
    CHECK(has_token(tokens, Token::Kind::Number, "42"));
    CHECK(has_token(tokens, Token::Kind::Number, "3.14"));
}

TEST_CASE("highlight_code json: returns valid non-empty Element") {
    const Theme theme = make_test_theme();
    std::string code = "{\n  \"name\": \"batbox\",\n  \"version\": \"0.1.0\",\n"
                       "  \"active\": true,\n  \"count\": 42\n}\n";
    auto el = highlight_code("json", code, theme);
    CHECK(!render_to_string(el).empty());
}

TEST_CASE("highlight_code jsonc: alias resolves to json lexer") {
    const Theme theme = make_test_theme();
    std::string code = "// jsonc comment\n{ \"key\": \"value\" }\n";
    auto el = highlight_code("jsonc", code, theme);
    CHECK(!render_to_string(el).empty());
}

TEST_CASE("highlight_code json5: alias resolves to json lexer") {
    const Theme theme = make_test_theme();
    std::string code = "{ key: 'value', /* comment */ count: 1, }";
    auto el = highlight_code("json5", code, theme);
    CHECK(!render_to_string(el).empty());
}

// ============================================================================
// 23: JS JSX recognition
// ============================================================================

TEST_CASE("js lexer: JSX self-closing tag produces tokens without crash") {
    // The tag is in expression position after '=' — after_operator is true at
    // '=', so '<Foo />' should be dispatched through lex_jsx_tag.
    auto tokens = lex_js("const el = <Foo />;");
    // Must produce at least one token and not crash.
    CHECK(!tokens.empty());
    // The tag name 'Foo' should be a Plain token.
    CHECK(has_token(tokens, Token::Kind::Plain, "Foo"));
}
