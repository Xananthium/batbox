// tests/unit/test_json_lexer.cpp
// ---------------------------------------------------------------------------
// doctest unit tests for batbox::tui::detail::lex_json()
//
// Coverage:
//   1.  Empty string — no crash, returns empty vector
//   2.  Empty object {} — two Operator tokens
//   3.  Empty array  [] — two Operator tokens
//   4.  String value — Kind::String
//   5.  Number — integer, negative, decimal, scientific
//   6.  Keywords — true, false, null → Kind::Keyword
//   7.  Structural punctuation — { } [ ] , : → Kind::Operator
//   8.  Object key detection — key strings emit Kind::Plain
//   9.  Nested object + array
//   10. Escape sequences in strings (\", \\, \n, \t, \uXXXX)
//   11. JSON5 line comments — Kind::Comment
//   12. JSON5 block comments — Kind::Comment
//   13. JSON5 single-quoted strings
//   14. Trailing comma (JSON5) — no crash, comma is Operator
//   15. Malformed: unterminated string — no crash
//   16. Malformed: bare minus with no digit — no crash
//   17. Malformed: garbage bytes — no crash
//   18. Full document round-trip (all token kinds present)
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "../../src/tui/manual_lexers/manual_lexers.hpp"

using namespace batbox::tui::detail;

namespace {

bool has_token(const std::vector<Token>& toks, Token::Kind kind, std::string_view substr) {
    for (const auto& t : toks) {
        if (t.kind == kind && t.text.find(substr) != std::string_view::npos)
            return true;
    }
    return false;
}

bool has_kind(const std::vector<Token>& toks, Token::Kind kind) {
    for (const auto& t : toks)
        if (t.kind == kind) return true;
    return false;
}

size_t count_kind(const std::vector<Token>& toks, Token::Kind kind) {
    size_t n = 0;
    for (const auto& t : toks)
        if (t.kind == kind) ++n;
    return n;
}

} // namespace

// ============================================================================
// 1. Empty input
// ============================================================================

TEST_CASE("json lexer: empty string yields no tokens") {
    auto tokens = lex_json("");
    CHECK(tokens.empty());
}

// ============================================================================
// 2. Empty object
// ============================================================================

TEST_CASE("json lexer: empty object {} yields two Operator tokens") {
    auto tokens = lex_json("{}");
    CHECK(count_kind(tokens, Token::Kind::Operator) == 2);
    CHECK(has_token(tokens, Token::Kind::Operator, "{"));
    CHECK(has_token(tokens, Token::Kind::Operator, "}"));
}

// ============================================================================
// 3. Empty array
// ============================================================================

TEST_CASE("json lexer: empty array [] yields two Operator tokens") {
    auto tokens = lex_json("[]");
    CHECK(count_kind(tokens, Token::Kind::Operator) == 2);
    CHECK(has_token(tokens, Token::Kind::Operator, "["));
    CHECK(has_token(tokens, Token::Kind::Operator, "]"));
}

// ============================================================================
// 4. String value
// ============================================================================

TEST_CASE("json lexer: bare string value is Kind::String") {
    auto tokens = lex_json(R"("hello world")");
    CHECK(has_token(tokens, Token::Kind::String, "hello world"));
}

// ============================================================================
// 5. Numbers — integer, negative, decimal, scientific
// ============================================================================

TEST_CASE("json lexer: integer literal is Kind::Number") {
    auto tokens = lex_json("42");
    CHECK(has_token(tokens, Token::Kind::Number, "42"));
}

TEST_CASE("json lexer: negative integer is Kind::Number") {
    auto tokens = lex_json("-1");
    CHECK(has_token(tokens, Token::Kind::Number, "-1"));
}

TEST_CASE("json lexer: decimal number is Kind::Number") {
    auto tokens = lex_json("1.5");
    CHECK(has_token(tokens, Token::Kind::Number, "1.5"));
}

TEST_CASE("json lexer: negative decimal is Kind::Number") {
    auto tokens = lex_json("-1.5");
    CHECK(has_token(tokens, Token::Kind::Number, "-1.5"));
}

TEST_CASE("json lexer: scientific notation is Kind::Number") {
    auto tokens = lex_json("1e10");
    CHECK(has_token(tokens, Token::Kind::Number, "1e10"));
}

TEST_CASE("json lexer: negative scientific notation is Kind::Number") {
    auto tokens = lex_json("-1.5e-3");
    CHECK(has_token(tokens, Token::Kind::Number, "-1.5e-3"));
}

TEST_CASE("json lexer: positive exponent scientific is Kind::Number") {
    auto tokens = lex_json("2.5E+4");
    CHECK(has_token(tokens, Token::Kind::Number, "2.5E+4"));
}

// ============================================================================
// 6. Keywords
// ============================================================================

TEST_CASE("json lexer: true is Kind::Keyword") {
    auto tokens = lex_json("true");
    CHECK(has_token(tokens, Token::Kind::Keyword, "true"));
}

TEST_CASE("json lexer: false is Kind::Keyword") {
    auto tokens = lex_json("false");
    CHECK(has_token(tokens, Token::Kind::Keyword, "false"));
}

TEST_CASE("json lexer: null is Kind::Keyword") {
    auto tokens = lex_json("null");
    CHECK(has_token(tokens, Token::Kind::Keyword, "null"));
}

// ============================================================================
// 7. Structural punctuation
// ============================================================================

TEST_CASE("json lexer: all structural characters are Kind::Operator") {
    auto tokens = lex_json(R"({"a":1})");
    CHECK(has_token(tokens, Token::Kind::Operator, "{"));
    CHECK(has_token(tokens, Token::Kind::Operator, "}"));
    CHECK(has_token(tokens, Token::Kind::Operator, ":"));
}

TEST_CASE("json lexer: comma is Kind::Operator") {
    auto tokens = lex_json("[1,2]");
    CHECK(has_token(tokens, Token::Kind::Operator, ","));
}

// ============================================================================
// 8. Object key detection — key strings are Kind::Plain
// ============================================================================

TEST_CASE("json lexer: object key string is Kind::Plain (not String)") {
    // {"name": "Alice"} — "name" is a key, "Alice" is a value
    auto tokens = lex_json(R"({"name":"Alice"})");
    // "name" should be Plain (key)
    CHECK(has_token(tokens, Token::Kind::Plain, "\"name\""));
    // "Alice" should be String (value)
    CHECK(has_token(tokens, Token::Kind::String, "\"Alice\""));
}

TEST_CASE("json lexer: value string in array is Kind::String (not Plain)") {
    auto tokens = lex_json(R"(["hello", "world"])");
    CHECK(has_token(tokens, Token::Kind::String, "\"hello\""));
    CHECK(has_token(tokens, Token::Kind::String, "\"world\""));
}

// ============================================================================
// 9. Nested structures
// ============================================================================

TEST_CASE("json lexer: nested object and array") {
    const char* src = R"({
  "user": {
    "name": "Bob",
    "scores": [10, 20, 30],
    "active": true
  }
})";
    auto tokens = lex_json(src);
    // Keys are Plain
    CHECK(has_token(tokens, Token::Kind::Plain,    "\"user\""));
    CHECK(has_token(tokens, Token::Kind::Plain,    "\"name\""));
    CHECK(has_token(tokens, Token::Kind::Plain,    "\"scores\""));
    CHECK(has_token(tokens, Token::Kind::Plain,    "\"active\""));
    // Values
    CHECK(has_token(tokens, Token::Kind::String,   "\"Bob\""));
    CHECK(has_token(tokens, Token::Kind::Number,   "10"));
    CHECK(has_token(tokens, Token::Kind::Number,   "20"));
    CHECK(has_token(tokens, Token::Kind::Number,   "30"));
    CHECK(has_token(tokens, Token::Kind::Keyword,  "true"));
}

// ============================================================================
// 10. Escape sequences in strings
// ============================================================================

TEST_CASE("json lexer: escaped double quote inside string") {
    // "say \"hi\""
    auto tokens = lex_json(R"("say \"hi\"")");
    CHECK(has_kind(tokens, Token::Kind::String));
    // Whole thing must be one String token (not split at \")
    CHECK(tokens.size() == 1u);
    CHECK(tokens[0].kind == Token::Kind::String);
}

TEST_CASE("json lexer: backslash escape sequences are consumed") {
    auto tokens = lex_json(R"("line1\nline2\ttab\\end")");
    CHECK(has_kind(tokens, Token::Kind::String));
    CHECK(tokens.size() == 1u);
}

TEST_CASE("json lexer: unicode escape \\uXXXX inside string") {
    auto tokens = lex_json(R"("ABC")");
    CHECK(has_kind(tokens, Token::Kind::String));
    // Must be a single token, no crash
    CHECK(tokens.size() == 1u);
}

// ============================================================================
// 11. JSON5 line comments
// ============================================================================

TEST_CASE("json5: line comment is Kind::Comment") {
    const char* src = "// this is a comment\n42";
    auto tokens = lex_json(src);
    CHECK(has_token(tokens, Token::Kind::Comment, "this is a comment"));
    CHECK(has_token(tokens, Token::Kind::Number, "42"));
}

TEST_CASE("json5: line comment at end of object field") {
    const char* src = R"({
  "x": 1, // trailing comment
  "y": 2
})";
    auto tokens = lex_json(src);
    CHECK(has_token(tokens, Token::Kind::Comment, "trailing comment"));
    CHECK(has_token(tokens, Token::Kind::Number, "1"));
    CHECK(has_token(tokens, Token::Kind::Number, "2"));
}

// ============================================================================
// 12. JSON5 block comments
// ============================================================================

TEST_CASE("json5: block comment is Kind::Comment") {
    const char* src = "/* block comment */ 99";
    auto tokens = lex_json(src);
    CHECK(has_token(tokens, Token::Kind::Comment, "block comment"));
    CHECK(has_token(tokens, Token::Kind::Number, "99"));
}

TEST_CASE("json5: multi-line block comment") {
    const char* src = "/* line1\nline2 */ true";
    auto tokens = lex_json(src);
    CHECK(has_token(tokens, Token::Kind::Comment, "line1"));
    CHECK(has_token(tokens, Token::Kind::Keyword, "true"));
}

// ============================================================================
// 13. JSON5 single-quoted strings
// ============================================================================

TEST_CASE("json5: single-quoted string value is Kind::String") {
    auto tokens = lex_json("'hello'");
    CHECK(has_token(tokens, Token::Kind::String, "'hello'"));
}

TEST_CASE("json5: single-quoted key is Kind::Plain") {
    auto tokens = lex_json("{'key': 1}");
    CHECK(has_token(tokens, Token::Kind::Plain, "'key'"));
    CHECK(has_token(tokens, Token::Kind::Number, "1"));
}

// ============================================================================
// 14. Trailing comma (JSON5)
// ============================================================================

TEST_CASE("json5: trailing comma is Kind::Operator and does not crash") {
    auto tokens = lex_json(R"([1, 2, 3,])");
    CHECK(has_token(tokens, Token::Kind::Operator, ","));
    // Last comma before ] should still tokenise correctly
    size_t commas = 0;
    for (const auto& t : tokens)
        if (t.kind == Token::Kind::Operator && t.text == ",") ++commas;
    CHECK(commas == 3u);
}

// ============================================================================
// 15. Malformed: unterminated string
// ============================================================================

TEST_CASE("malformed: unterminated string does not crash") {
    // No closing quote — lexer must not loop infinitely or crash.
    auto tokens = lex_json(R"("unterminated)");
    // Must return at least one token and not throw
    CHECK(!tokens.empty());
}

// ============================================================================
// 16. Malformed: bare minus
// ============================================================================

TEST_CASE("malformed: bare minus with no digit does not crash") {
    // "-" is not followed by a digit so the number path won't fire.
    // Expect it to be emitted as a Plain token.
    auto tokens = lex_json("-");
    CHECK(!tokens.empty());
}

// ============================================================================
// 17. Malformed: garbage / random bytes
// ============================================================================

TEST_CASE("malformed: garbage bytes do not crash") {
    auto tokens = lex_json("!@#$%^&*()");
    // Must return without throwing regardless of token kinds
    CHECK(true); // reaching here means no crash
}

TEST_CASE("malformed: partial escape at EOF does not crash") {
    // Backslash at very end of input
    auto tokens = lex_json(R"("abc\)");
    CHECK(true); // no crash
}

// ============================================================================
// 18. Full document — all token kinds present
// ============================================================================

TEST_CASE("json lexer: full document contains all expected token kinds") {
    const char* src = R"({
  "name": "Digital Eclipse",
  "version": 1,
  "ratio": 3.14,
  "enabled": true,
  "disabled": false,
  "nothing": null,
  "tags": ["alpha", "beta"],
  "score": -1.5e-3
})";
    auto tokens = lex_json(src);

    // All six kinds must appear
    CHECK(has_kind(tokens, Token::Kind::Plain));     // keys
    CHECK(has_kind(tokens, Token::Kind::String));    // string values
    CHECK(has_kind(tokens, Token::Kind::Number));    // numbers
    CHECK(has_kind(tokens, Token::Kind::Keyword));   // true/false/null
    CHECK(has_kind(tokens, Token::Kind::Operator));  // { } [ ] , :

    // Spot checks
    CHECK(has_token(tokens, Token::Kind::Plain,   "\"name\""));
    CHECK(has_token(tokens, Token::Kind::String,  "\"Digital Eclipse\""));
    CHECK(has_token(tokens, Token::Kind::Number,  "3.14"));
    CHECK(has_token(tokens, Token::Kind::Keyword, "true"));
    CHECK(has_token(tokens, Token::Kind::Keyword, "false"));
    CHECK(has_token(tokens, Token::Kind::Keyword, "null"));
    CHECK(has_token(tokens, Token::Kind::Number,  "-1.5e-3"));
}
