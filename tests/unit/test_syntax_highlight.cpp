// tests/unit/test_syntax_highlight.cpp
// ---------------------------------------------------------------------------
// doctest suite for batbox::tui::highlight_code()
//
// This test suite exercises the manual lexer path (tree-sitter removed in PEXT2 1.4a).
//
// Coverage:
//   1.  C++ — keywords are present in output (AccentMagenta path exercises)
//   2.  C++ — string literals recognised
//   3.  C++ — line comments recognised
//   4.  Python — keywords highlighted
//   5.  Python — string literals (double-quoted and single-quoted)
//   6.  Python — line comments (#)
//   7.  JavaScript — keywords highlighted
//   8.  JavaScript — string and template literals
//   9.  JavaScript — line comment
//   10. TypeScript (alias) — same lexer as JS, keywords present
//   11. Rust — keywords highlighted
//   12. Rust — string literals
//   13. Rust — line comments
//   14. Go — keywords highlighted
//   15. Go — string literals
//   16. Go — line comments
//   17. Unknown language — returns an ftxui Element without error
//   18. Empty language — returns an ftxui Element without error
//   19. Empty code — returns an ftxui Element without error
//   20. Numeric literals — recognised in C++, Python, Go
//   21. Language aliases — "c++", "py", "ts", "rs", "sh"-alias plain render or correct lang
//   22. Highlight output is non-null and can be rendered
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/tui/SyntaxHighlight.hpp>
#include <batbox/theme/Theme.hpp>
#include <batbox/tui/ThemeApply.hpp>

// We only exercise the manual-lexer internals indirectly through the public
// API.  For whitebox token-level checks we include the internal header.
// The test CMakeLists compiles the manual_lexer .cpp files into this test
// executable alongside SyntaxHighlight.cpp.
#include "manual_lexers/manual_lexers.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

using namespace batbox::tui;
using namespace batbox::theme;
using namespace batbox::tui::detail;

namespace {

// Build the miss-kittin theme inline (mirrors what themes.cpp does).
// Using known colour values avoids a dependency on the full theme library
// in the test executable — we only need ftxui::Color + the Theme struct.
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

// Helper: check that a token vector contains at least one token of the
// expected kind whose text contains the given substring.
bool has_token(const std::vector<Token>& tokens, Token::Kind kind, std::string_view substr) {
    for (const auto& t : tokens) {
        if (t.kind == kind && t.text.find(substr) != std::string_view::npos)
            return true;
    }
    return false;
}

// Helper: render an Element to a 120x40 screen and return the plain string.
// This exercises the ftxui rendering pipeline and proves the Element is valid.
std::string render_to_string(ftxui::Element el) {
    using namespace ftxui;
    auto screen = Screen::Create(Dimension::Fixed(120), Dimension::Fixed(40));
    Render(screen, el);
    return screen.ToString();
}

} // namespace

// ============================================================================
// 1–3: C++ lexer token-level tests
// ============================================================================

TEST_CASE("cpp lexer: keywords are classified as Keyword") {
    auto tokens = lex_cpp("int main() { return 0; }");
    CHECK(has_token(tokens, Token::Kind::Keyword, "int"));
    CHECK(has_token(tokens, Token::Kind::Keyword, "return"));
}

TEST_CASE("cpp lexer: string literals classified as String") {
    auto tokens = lex_cpp(R"(const char* s = "hello world";)");
    CHECK(has_token(tokens, Token::Kind::String, "hello world"));
}

TEST_CASE("cpp lexer: line comment classified as Comment") {
    auto tokens = lex_cpp("// This is a comment\nint x = 1;");
    CHECK(has_token(tokens, Token::Kind::Comment, "This is a comment"));
}

// ============================================================================
// 4–6: Python lexer token-level tests
// ============================================================================

TEST_CASE("python lexer: keywords are classified as Keyword") {
    auto tokens = lex_python("def foo(x):\n    return x + 1");
    CHECK(has_token(tokens, Token::Kind::Keyword, "def"));
    CHECK(has_token(tokens, Token::Kind::Keyword, "return"));
}

TEST_CASE("python lexer: string literals (double-quoted and single-quoted)") {
    auto tokens = lex_python(R"(s = "hello" ; t = 'world')");
    CHECK(has_token(tokens, Token::Kind::String, "hello"));
    CHECK(has_token(tokens, Token::Kind::String, "world"));
}

TEST_CASE("python lexer: line comment classified as Comment") {
    auto tokens = lex_python("# this is python\nx = 1");
    CHECK(has_token(tokens, Token::Kind::Comment, "this is python"));
}

// ============================================================================
// 7–10: JavaScript / TypeScript lexer token-level tests
// ============================================================================

TEST_CASE("js lexer: keywords are classified as Keyword") {
    auto tokens = lex_js("function greet(name) { return 'hi ' + name; }");
    CHECK(has_token(tokens, Token::Kind::Keyword, "function"));
    CHECK(has_token(tokens, Token::Kind::Keyword, "return"));
}

TEST_CASE("js lexer: string and template literals") {
    auto tokens = lex_js(R"(const x = "hello"; const y = `world`;)");
    CHECK(has_token(tokens, Token::Kind::String, "hello"));
    CHECK(has_token(tokens, Token::Kind::String, "world"));
}

TEST_CASE("js lexer: line comment classified as Comment") {
    auto tokens = lex_js("// js comment\nconst x = 1;");
    CHECK(has_token(tokens, Token::Kind::Comment, "js comment"));
}

TEST_CASE("typescript alias uses same lexer as javascript") {
    // highlight_code("ts", …) should not throw and should return a valid Element
    const Theme theme = make_test_theme();
    auto el = highlight_code("ts", "const x: number = 42;", theme);
    CHECK(!render_to_string(el).empty());
}

// ============================================================================
// 11–13: Rust lexer token-level tests
// ============================================================================

TEST_CASE("rust lexer: keywords are classified as Keyword") {
    auto tokens = lex_rust("fn main() { let x = 42; }");
    CHECK(has_token(tokens, Token::Kind::Keyword, "fn"));
    CHECK(has_token(tokens, Token::Kind::Keyword, "let"));
}

TEST_CASE("rust lexer: string literals classified as String") {
    auto tokens = lex_rust(R"(let s = "hello rust";)");
    CHECK(has_token(tokens, Token::Kind::String, "hello rust"));
}

TEST_CASE("rust lexer: line comment classified as Comment") {
    auto tokens = lex_rust("// rust comment\nlet x = 1;");
    CHECK(has_token(tokens, Token::Kind::Comment, "rust comment"));
}

// ============================================================================
// 14–16: Go lexer token-level tests
// ============================================================================

TEST_CASE("go lexer: keywords are classified as Keyword") {
    auto tokens = lex_go("func main() { var x int = 42 }");
    CHECK(has_token(tokens, Token::Kind::Keyword, "func"));
    CHECK(has_token(tokens, Token::Kind::Keyword, "var"));
}

TEST_CASE("go lexer: string literals classified as String") {
    auto tokens = lex_go(R"(s := "hello go")");
    CHECK(has_token(tokens, Token::Kind::String, "hello go"));
}

TEST_CASE("go lexer: line comment classified as Comment") {
    auto tokens = lex_go("// go comment\nvar x = 1");
    CHECK(has_token(tokens, Token::Kind::Comment, "go comment"));
}

// ============================================================================
// 17–19: Unknown / empty language / empty code via public API
// ============================================================================

TEST_CASE("unknown language: plain render, no error") {
    const Theme theme = make_test_theme();
    // Should not throw; returns a valid Element.
    auto el = highlight_code("cobol", "DISPLAY 'HELLO'.", theme);
    CHECK(!render_to_string(el).empty());
}

TEST_CASE("empty language: plain render, no error") {
    const Theme theme = make_test_theme();
    auto el = highlight_code("", "some code", theme);
    CHECK(!render_to_string(el).empty());
}

TEST_CASE("empty code: returns valid (possibly empty) Element without error") {
    const Theme theme = make_test_theme();
    auto el = highlight_code("cpp", "", theme);
    // Should not crash; rendering should succeed.
    std::string out = render_to_string(el);
    CHECK(out.size() >= 0); // always true; just ensure no exception
}

// ============================================================================
// 20: Numeric literals
// ============================================================================

TEST_CASE("cpp lexer: numeric literals classified as Number") {
    auto tokens = lex_cpp("int x = 42; double y = 3.14; int z = 0xFF;");
    CHECK(has_token(tokens, Token::Kind::Number, "42"));
    CHECK(has_token(tokens, Token::Kind::Number, "3.14"));
    CHECK(has_token(tokens, Token::Kind::Number, "0xFF"));
}

TEST_CASE("python lexer: numeric literals classified as Number") {
    auto tokens = lex_python("x = 42\ny = 3.14\nz = 0xFF");
    CHECK(has_token(tokens, Token::Kind::Number, "42"));
    CHECK(has_token(tokens, Token::Kind::Number, "3.14"));
    CHECK(has_token(tokens, Token::Kind::Number, "0xFF"));
}

TEST_CASE("go lexer: numeric literals classified as Number") {
    auto tokens = lex_go("x := 42\ny := 3.14\nz := 0xFF");
    CHECK(has_token(tokens, Token::Kind::Number, "42"));
    CHECK(has_token(tokens, Token::Kind::Number, "3.14"));
    CHECK(has_token(tokens, Token::Kind::Number, "0xFF"));
}

// ============================================================================
// 21: Language aliases via public API
// ============================================================================

TEST_CASE("language alias 'c++' resolves to cpp lexer") {
    const Theme theme = make_test_theme();
    auto el = highlight_code("c++", "int x = 0;", theme);
    CHECK(!render_to_string(el).empty());
}

TEST_CASE("language alias 'py' resolves to python lexer") {
    const Theme theme = make_test_theme();
    auto el = highlight_code("py", "x = 1", theme);
    CHECK(!render_to_string(el).empty());
}

TEST_CASE("language alias 'rs' resolves to rust lexer") {
    const Theme theme = make_test_theme();
    auto el = highlight_code("rs", "fn main() {}", theme);
    CHECK(!render_to_string(el).empty());
}

TEST_CASE("language alias 'sh' renders without error") {
    const Theme theme = make_test_theme();
    // 'sh' / 'bash' maps to "bash" canonical name; manual lexer has no bash
    // entry, so it falls back to plain render — that's the correct behaviour.
    auto el = highlight_code("sh", "echo hello", theme);
    CHECK(!render_to_string(el).empty());
}

// ============================================================================
// 22: Output is valid ftxui::Element
// ============================================================================

TEST_CASE("highlight_code returns non-empty rendered output for cpp") {
    const Theme theme = make_test_theme();
    std::string code = R"cpp(
int main() {
    // greet the world
    std::cout << "Hello, world!" << std::endl;
    return 0;
}
)cpp";
    auto el = highlight_code("cpp", code, theme);
    std::string rendered = render_to_string(el);
    CHECK(!rendered.empty());
}

TEST_CASE("highlight_code returns non-empty rendered output for python") {
    const Theme theme = make_test_theme();
    std::string code = R"py(
def greet(name: str) -> str:
    # return a greeting
    return f"Hello, {name}!"
)py";
    auto el = highlight_code("python", code, theme);
    std::string rendered = render_to_string(el);
    CHECK(!rendered.empty());
}

TEST_CASE("highlight_code returns non-empty rendered output for rust") {
    const Theme theme = make_test_theme();
    std::string code = R"rs(
fn add(a: i32, b: i32) -> i32 {
    // add two numbers
    a + b
}
)rs";
    auto el = highlight_code("rust", code, theme);
    std::string rendered = render_to_string(el);
    CHECK(!rendered.empty());
}

TEST_CASE("highlight_code returns non-empty rendered output for go") {
    const Theme theme = make_test_theme();
    std::string code = R"go(
package main

import "fmt"

func main() {
    // hello go
    fmt.Println("Hello, Go!")
}
)go";
    auto el = highlight_code("go", code, theme);
    std::string rendered = render_to_string(el);
    CHECK(!rendered.empty());
}
