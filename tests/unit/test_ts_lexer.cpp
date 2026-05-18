// tests/unit/test_ts_lexer.cpp
// ---------------------------------------------------------------------------
// doctest suite for batbox::tui::detail::lex_typescript
//
// Coverage:
//  1.  Basic JS keywords
//  2.  TS-specific keywords: interface, type, enum, namespace, declare
//  3.  TS-specific keywords: as, satisfies, readonly, abstract, override
//  4.  TS-specific keywords: keyof, infer, never, unknown, any
//  5.  TS-specific: public, private, protected
//  6.  Line comment // …
//  7.  Block comment /* … */
//  8.  Double-quoted string
//  9.  Single-quoted string
//  10. Template literal without expression
//  11. Template literal with ${expr} — expr gets keyword colouring
//  12. Type annotation after ':' → type name coloured as Keyword
//  13. Generic angle brackets with type param → content coloured as Keyword
//  14. Interface declaration — 'interface' keyword + body
//  15. Enum declaration
//  16. TSX opening tag  <Foo>
//  17. TSX self-closing  <Foo />
//  18. TSX closing tag  </Foo>
//  19. TSX tag with string attribute  <Foo bar="baz">
//  20. Numeric literals: integer, float, hex, binary, BigInt
//  21. Regex literal coloured as String
//  22. Full interface snippet — all token kinds present
//  23. 'extends' keyword triggers type-name colouring for next identifier
//  24. Nested generics  Array<Map<K, V>>
//  25. No tokens for empty input
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

// We include the lexer source directly so the test binary doesn't need to
// link the full batbox_tui OBJECT library.
#include "../../src/tui/manual_lexers/manual_lexers.hpp"

using namespace batbox::tui::detail;
using K = Token::Kind;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
namespace {

// Collect only tokens whose kind == k.
std::vector<std::string_view> tokens_of_kind(const std::vector<Token>& toks, K k) {
    std::vector<std::string_view> out;
    for (auto& t : toks)
        if (t.kind == k) out.push_back(t.text);
    return out;
}

bool has_token(const std::vector<Token>& toks, K k, std::string_view text) {
    for (auto& t : toks)
        if (t.kind == k && t.text == text) return true;
    return false;
}

} // namespace

// ===========================================================================
// 1. Basic JS keywords
// ===========================================================================
TEST_CASE("basic JS keywords are Keyword tokens") {
    auto toks = lex_typescript("const x = 42; return x;");
    CHECK(has_token(toks, K::Keyword, "const"));
    CHECK(has_token(toks, K::Keyword, "return"));
    CHECK(has_token(toks, K::Number,  "42"));
    CHECK(has_token(toks, K::Plain,   "x"));
}

// ===========================================================================
// 2. TS-specific structural keywords
// ===========================================================================
TEST_CASE("TS structural keywords: interface type enum namespace declare") {
    auto toks = lex_typescript("interface IFoo {} type T = string; enum E {} namespace N {} declare const x: number;");
    CHECK(has_token(toks, K::Keyword, "interface"));
    CHECK(has_token(toks, K::Keyword, "type"));
    CHECK(has_token(toks, K::Keyword, "enum"));
    CHECK(has_token(toks, K::Keyword, "namespace"));
    CHECK(has_token(toks, K::Keyword, "declare"));
}

// ===========================================================================
// 3. TS modifier keywords
// ===========================================================================
TEST_CASE("TS modifier keywords: as satisfies readonly abstract override") {
    auto toks = lex_typescript("const x = y as string; const z = w satisfies T; readonly prop: boolean; abstract class A {} override method() {}");
    CHECK(has_token(toks, K::Keyword, "as"));
    CHECK(has_token(toks, K::Keyword, "satisfies"));
    CHECK(has_token(toks, K::Keyword, "readonly"));
    CHECK(has_token(toks, K::Keyword, "abstract"));
    CHECK(has_token(toks, K::Keyword, "override"));
}

// ===========================================================================
// 4. TS type-operator keywords
// ===========================================================================
TEST_CASE("TS type keywords: keyof infer never unknown any") {
    auto toks = lex_typescript("type K = keyof T; type I = T extends U ? infer R : never; type A = unknown; type B = any;");
    CHECK(has_token(toks, K::Keyword, "keyof"));
    CHECK(has_token(toks, K::Keyword, "infer"));
    CHECK(has_token(toks, K::Keyword, "never"));
    CHECK(has_token(toks, K::Keyword, "unknown"));
    CHECK(has_token(toks, K::Keyword, "any"));
}

// ===========================================================================
// 5. Access-modifier keywords
// ===========================================================================
TEST_CASE("TS access modifier keywords: public private protected") {
    auto toks = lex_typescript("class C { public x: number; private y: string; protected z: boolean; }");
    CHECK(has_token(toks, K::Keyword, "public"));
    CHECK(has_token(toks, K::Keyword, "private"));
    CHECK(has_token(toks, K::Keyword, "protected"));
    CHECK(has_token(toks, K::Keyword, "class"));
}

// ===========================================================================
// 6. Line comment
// ===========================================================================
TEST_CASE("line comment is a Comment token") {
    auto toks = lex_typescript("const x = 1; // this is a comment\nconst y = 2;");
    CHECK(has_token(toks, K::Comment, "// this is a comment"));
    CHECK(has_token(toks, K::Keyword, "const"));
}

// ===========================================================================
// 7. Block comment
// ===========================================================================
TEST_CASE("block comment is a Comment token") {
    auto toks = lex_typescript("/* hello world */ const x = 1;");
    CHECK(has_token(toks, K::Comment, "/* hello world */"));
    CHECK(has_token(toks, K::Keyword, "const"));
}

// ===========================================================================
// 8. Double-quoted string
// ===========================================================================
TEST_CASE("double-quoted string is a String token") {
    auto toks = lex_typescript(R"(const s = "hello world";)");
    CHECK(has_token(toks, K::String, "\"hello world\""));
}

// ===========================================================================
// 9. Single-quoted string
// ===========================================================================
TEST_CASE("single-quoted string is a String token") {
    auto toks = lex_typescript("const s = 'hello';");
    CHECK(has_token(toks, K::String, "'hello'"));
}

// ===========================================================================
// 10. Template literal without expression
// ===========================================================================
TEST_CASE("template literal without expression is a String token") {
    auto toks = lex_typescript("const s = `hello world`;");
    auto strings = tokens_of_kind(toks, K::String);
    REQUIRE(!strings.empty());
    // The whole backtick span must appear in at least one string token
    bool found = false;
    for (auto sv : strings)
        if (sv.find("hello world") != std::string_view::npos) found = true;
    CHECK(found);
}

// ===========================================================================
// 11. Template literal with ${expr}
// ===========================================================================
TEST_CASE("template literal expression content is re-lexed") {
    // const keyword inside ${} should come out as Keyword
    auto toks = lex_typescript("const s = `prefix ${name} suffix`;");
    // 'name' inside the expression should be a Plain token
    CHECK(has_token(toks, K::Plain, "name"));
    // String segments should be present
    auto strings = tokens_of_kind(toks, K::String);
    CHECK(!strings.empty());
}

// ===========================================================================
// 12. Type annotation after ':'
// ===========================================================================
TEST_CASE("type annotation after colon is coloured as Keyword") {
    // 'MyType' is not in the keyword table but follows ':'
    auto toks = lex_typescript("const x: MyType = value;");
    CHECK(has_token(toks, K::Keyword, "MyType"));
}

// ===========================================================================
// 13. Generic type parameter
// ===========================================================================
TEST_CASE("identifier after '<' in generic position is coloured as Keyword") {
    auto toks = lex_typescript("function id<T>(x: T): T { return x; }");
    // T after < should be Keyword; T after ':' should also be Keyword
    CHECK(has_token(toks, K::Keyword, "T"));
    CHECK(has_token(toks, K::Keyword, "function"));
}

// ===========================================================================
// 14. Interface declaration
// ===========================================================================
TEST_CASE("interface with typed members tokenises correctly") {
    auto toks = lex_typescript("interface User { name: string; age: number; }");
    CHECK(has_token(toks, K::Keyword, "interface"));
    CHECK(has_token(toks, K::Plain,   "User"));
    CHECK(has_token(toks, K::Keyword, "string"));
    CHECK(has_token(toks, K::Keyword, "number"));
}

// ===========================================================================
// 15. Enum declaration
// ===========================================================================
TEST_CASE("enum declaration tokenises correctly") {
    auto toks = lex_typescript("enum Direction { Up, Down, Left, Right }");
    CHECK(has_token(toks, K::Keyword, "enum"));
    CHECK(has_token(toks, K::Plain,   "Direction"));
    CHECK(has_token(toks, K::Plain,   "Up"));
}

// ===========================================================================
// 16. TSX opening tag
// ===========================================================================
TEST_CASE("TSX opening tag emits Operator for delimiters, Plain for name") {
    auto toks = lex_typescript("const el = <MyComponent>;");
    CHECK(has_token(toks, K::Plain,    "MyComponent"));
    CHECK(has_token(toks, K::Operator, "<"));
    CHECK(has_token(toks, K::Operator, ">"));
}

// ===========================================================================
// 17. TSX self-closing tag
// ===========================================================================
TEST_CASE("TSX self-closing tag emits Operator for />") {
    auto toks = lex_typescript("const el = <MyComponent />;");
    CHECK(has_token(toks, K::Plain,    "MyComponent"));
    CHECK(has_token(toks, K::Operator, "<"));
    // Self-closing emits "/>" as a single Operator token
    bool found_selfclose = false;
    for (auto& t : toks)
        if (t.kind == K::Operator && t.text == "/>") found_selfclose = true;
    CHECK(found_selfclose);
}

// ===========================================================================
// 18. TSX closing tag
// ===========================================================================
TEST_CASE("TSX closing tag </Foo> emits correct tokens") {
    auto toks = lex_typescript("const el = </MyComponent>;");
    CHECK(has_token(toks, K::Plain,    "MyComponent"));
    CHECK(has_token(toks, K::Operator, "<"));
    CHECK(has_token(toks, K::Operator, "/"));
    CHECK(has_token(toks, K::Operator, ">"));
}

// ===========================================================================
// 19. TSX tag with string attribute
// ===========================================================================
TEST_CASE("TSX tag string attribute is a String token") {
    auto toks = lex_typescript(R"(const el = <Foo bar="baz">;)");
    CHECK(has_token(toks, K::Plain,  "Foo"));
    CHECK(has_token(toks, K::Plain,  "bar"));
    CHECK(has_token(toks, K::String, "\"baz\""));
}

// ===========================================================================
// 20. Numeric literals
// ===========================================================================
TEST_CASE("numeric literals: integer float hex binary BigInt") {
    auto toks = lex_typescript("const a=42; const b=3.14; const c=0xFF; const d=0b1010; const e=100n;");
    CHECK(has_token(toks, K::Number, "42"));
    CHECK(has_token(toks, K::Number, "3.14"));
    CHECK(has_token(toks, K::Number, "0xFF"));
    CHECK(has_token(toks, K::Number, "0b1010"));
    CHECK(has_token(toks, K::Number, "100n"));
}

// ===========================================================================
// 21. Regex literal
// ===========================================================================
TEST_CASE("regex literal after operator is coloured as String") {
    auto toks = lex_typescript("const re = /hello/gi;");
    CHECK(has_token(toks, K::String, "/hello/gi"));
}

// ===========================================================================
// 22. Full interface snippet — all expected token kinds present
// ===========================================================================
TEST_CASE("full interface snippet produces all expected token kinds") {
    std::string_view src = R"(
// User model
interface User {
    id: number;
    name: string;
    readonly createdAt: Date;
}
)";
    auto toks = lex_typescript(src);
    auto kws  = tokens_of_kind(toks, K::Keyword);
    auto cmts = tokens_of_kind(toks, K::Comment);
    auto plns = tokens_of_kind(toks, K::Plain);

    CHECK(!kws.empty());
    CHECK(!cmts.empty());
    CHECK(!plns.empty());

    CHECK(has_token(toks, K::Comment, "// User model"));
    CHECK(has_token(toks, K::Keyword, "interface"));
    CHECK(has_token(toks, K::Keyword, "readonly"));
    CHECK(has_token(toks, K::Keyword, "number"));
    CHECK(has_token(toks, K::Keyword, "string"));
}

// ===========================================================================
// 23. 'extends' triggers type-name colouring
// ===========================================================================
TEST_CASE("extends keyword triggers type colouring for following identifier") {
    auto toks = lex_typescript("class Dog extends Animal {}");
    CHECK(has_token(toks, K::Keyword, "class"));
    CHECK(has_token(toks, K::Keyword, "extends"));
    // 'Animal' is not a built-in keyword but follows 'extends'
    CHECK(has_token(toks, K::Keyword, "Animal"));
}

// ===========================================================================
// 24. Nested generics  Array<Map<K, V>>
// ===========================================================================
TEST_CASE("nested generics tokenise without crash") {
    // Should not crash or assert — just produce a well-formed token list.
    auto toks = lex_typescript("const m: Array<Map<string, number>> = [];");
    CHECK(!toks.empty());
    CHECK(has_token(toks, K::Keyword, "Array"));
    CHECK(has_token(toks, K::Keyword, "Map"));
    CHECK(has_token(toks, K::Keyword, "string"));
    CHECK(has_token(toks, K::Keyword, "number"));
}

// ===========================================================================
// 25. Empty input
// ===========================================================================
TEST_CASE("empty input produces no tokens") {
    auto toks = lex_typescript("");
    CHECK(toks.empty());
}
