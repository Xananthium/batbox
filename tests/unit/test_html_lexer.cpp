// tests/unit/test_html_lexer.cpp
// ---------------------------------------------------------------------------
// doctest suite for batbox::tui::detail::lex_html().
//
// Covers:
//   - Simple element: <p>text</p>
//   - Void / self-closing: <br /> <input/>
//   - Double-quoted attributes: <a href="url">
//   - Single-quoted attributes: <a href='url'>
//   - Bare boolean attributes: <input disabled>
//   - Unquoted attribute values: <input type=text>
//   - HTML comments: <!-- ... -->
//   - DOCTYPE: <!DOCTYPE html>
//   - Named entities: &amp; &lt;
//   - Numeric entities: &#38; &#x26;
//   - Nested tags
//   - <script> and <style> raw content passthrough (no crash, inner plain)
//   - Malformed: unclosed tag at EOF (no crash, returns tokens)
//   - Empty string input (returns empty)
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "manual_lexers/manual_lexers.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

using batbox::tui::detail::Token;
using Kind = Token::Kind;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::vector<Token> lex(std::string_view src) {
    return batbox::tui::detail::lex_html(src);
}

/// Concatenate all token texts to reconstruct the original source.
static std::string reconstruct(const std::vector<Token>& toks) {
    std::string out;
    for (const auto& t : toks) out += t.text;
    return out;
}

/// Return all tokens of a given Kind.
static std::vector<Token> of_kind(const std::vector<Token>& toks, Kind k) {
    std::vector<Token> out;
    for (const auto& t : toks) if (t.kind == k) out.push_back(t);
    return out;
}

/// True if any token has the given kind AND contains the given substring.
static bool has_token(const std::vector<Token>& toks, Kind k, std::string_view sub) {
    for (const auto& t : toks)
        if (t.kind == k && t.text.find(sub) != std::string_view::npos)
            return true;
    return false;
}

// ---------------------------------------------------------------------------
// Suite: reconstruction invariant
// ---------------------------------------------------------------------------

TEST_SUITE("lex_html — reconstruction") {

    TEST_CASE("empty source") {
        auto toks = lex("");
        CHECK(toks.empty());
        CHECK(reconstruct(toks) == "");
    }

    TEST_CASE("simple paragraph reconstructs exactly") {
        std::string src = "<p>Hello, world!</p>";
        CHECK(reconstruct(lex(src)) == src);
    }

    TEST_CASE("complex document reconstructs exactly") {
        std::string src =
            "<!DOCTYPE html>\n"
            "<html lang=\"en\">\n"
            "<head><title>Test</title></head>\n"
            "<body>\n"
            "  <!-- a comment -->\n"
            "  <p class='note'>text &amp; more</p>\n"
            "  <br />\n"
            "</body>\n"
            "</html>";
        CHECK(reconstruct(lex(src)) == src);
    }
}

// ---------------------------------------------------------------------------
// Suite: DOCTYPE
// ---------------------------------------------------------------------------

TEST_SUITE("lex_html — DOCTYPE") {

    TEST_CASE("DOCTYPE is emitted as Comment") {
        auto toks = lex("<!DOCTYPE html>");
        CHECK(has_token(toks, Kind::Comment, "DOCTYPE"));
    }

    TEST_CASE("DOCTYPE with mixed case") {
        auto toks = lex("<!doctype HTML>");
        // The whole thing lands in a Comment token
        CHECK_FALSE(of_kind(toks, Kind::Comment).empty());
    }
}

// ---------------------------------------------------------------------------
// Suite: HTML comments
// ---------------------------------------------------------------------------

TEST_SUITE("lex_html — comments") {

    TEST_CASE("single-line comment") {
        auto toks = lex("<!-- hello -->");
        REQUIRE_FALSE(of_kind(toks, Kind::Comment).empty());
        CHECK(has_token(toks, Kind::Comment, "hello"));
    }

    TEST_CASE("multi-line comment") {
        auto toks = lex("<!--\nline one\nline two\n-->");
        CHECK(has_token(toks, Kind::Comment, "line one"));
    }

    TEST_CASE("comment in mixed content") {
        std::string src = "<p>before</p><!-- note --><p>after</p>";
        auto toks = lex(src);
        CHECK(has_token(toks, Kind::Comment, "note"));
        CHECK(reconstruct(toks) == src);
    }

    TEST_CASE("unterminated comment at EOF does not crash") {
        auto toks = lex("<!-- unclosed");
        // Must not throw / crash; some tokens returned
        CHECK_FALSE(toks.empty());
        CHECK(reconstruct(toks) == "<!-- unclosed");
    }
}

// ---------------------------------------------------------------------------
// Suite: Tags
// ---------------------------------------------------------------------------

TEST_SUITE("lex_html — tags") {

    TEST_CASE("opening tag name is Keyword") {
        auto toks = lex("<div>");
        CHECK(has_token(toks, Kind::Keyword, "div"));
    }

    TEST_CASE("closing tag name is Keyword") {
        auto toks = lex("</section>");
        CHECK(has_token(toks, Kind::Keyword, "section"));
    }

    TEST_CASE("self-closing tag name is Keyword") {
        auto toks = lex("<br />");
        CHECK(has_token(toks, Kind::Keyword, "br"));
    }

    TEST_CASE("void tag with no space before slash") {
        auto toks = lex("<input/>");
        CHECK(has_token(toks, Kind::Keyword, "input"));
    }

    TEST_CASE("nested tags all produce Keyword tokens") {
        auto toks = lex("<ul><li>item</li></ul>");
        auto kws = of_kind(toks, Kind::Keyword);
        // ul (open), li (open), li (close), ul (close) = 4 Keyword tokens
        CHECK(kws.size() == 4);
    }

    TEST_CASE("tag with namespace prefix") {
        auto toks = lex("<svg:rect />");
        CHECK(has_token(toks, Kind::Keyword, "svg:rect"));
    }

    TEST_CASE("angle brackets emitted as Operator") {
        auto toks = lex("<p></p>");
        // Each '<' and '>' should be an Operator
        auto ops = of_kind(toks, Kind::Operator);
        CHECK(ops.size() >= 4);
    }
}

// ---------------------------------------------------------------------------
// Suite: Attributes
// ---------------------------------------------------------------------------

TEST_SUITE("lex_html — attributes") {

    TEST_CASE("double-quoted attribute value is String") {
        auto toks = lex(R"(<a href="https://example.com">)");
        CHECK(has_token(toks, Kind::String, "https://example.com"));
    }

    TEST_CASE("single-quoted attribute value is String") {
        auto toks = lex("<a href='https://example.com'>");
        CHECK(has_token(toks, Kind::String, "https://example.com"));
    }

    TEST_CASE("double-quoted attribute with embedded single quote") {
        auto toks = lex(R"(<p title="it's fine">)");
        CHECK(has_token(toks, Kind::String, "it's fine"));
    }

    TEST_CASE("single-quoted attribute with embedded double quote") {
        auto toks = lex("<p title='say \"hi\"'>");
        CHECK(has_token(toks, Kind::String, "say \"hi\""));
    }

    TEST_CASE("bare boolean attribute is Plain") {
        auto toks = lex("<input disabled>");
        // 'disabled' lands in a Plain token (not String, not Keyword)
        CHECK(has_token(toks, Kind::Plain, "disabled"));
        CHECK_FALSE(has_token(toks, Kind::String, "disabled"));
        CHECK_FALSE(has_token(toks, Kind::Keyword, "disabled"));
    }

    TEST_CASE("multiple attributes") {
        auto toks = lex(R"(<img src="cat.png" alt="a cat" loading="lazy">)");
        auto strs = of_kind(toks, Kind::String);
        // Three quoted values
        CHECK(strs.size() == 3);
    }

    TEST_CASE("multiple boolean attributes") {
        auto toks = lex("<input type=\"checkbox\" checked disabled required>");
        // checked, disabled, required → Plain
        CHECK(has_token(toks, Kind::Plain, "checked"));
        CHECK(has_token(toks, Kind::Plain, "disabled"));
        CHECK(has_token(toks, Kind::Plain, "required"));
    }
}

// ---------------------------------------------------------------------------
// Suite: Entities
// ---------------------------------------------------------------------------

TEST_SUITE("lex_html — entities") {

    TEST_CASE("named entity &amp;") {
        auto toks = lex("&amp;");
        CHECK(has_token(toks, Kind::String, "&amp;"));
    }

    TEST_CASE("named entity &lt; inside text") {
        auto toks = lex("a &lt; b");
        CHECK(has_token(toks, Kind::String, "&lt;"));
    }

    TEST_CASE("decimal numeric entity &#38;") {
        auto toks = lex("&#38;");
        CHECK(has_token(toks, Kind::String, "&#38;"));
    }

    TEST_CASE("hex numeric entity &#x26;") {
        auto toks = lex("&#x26;");
        CHECK(has_token(toks, Kind::String, "&#x26;"));
    }

    TEST_CASE("entity reconstruction") {
        std::string src = "Tom &amp; Jerry &#169;";
        CHECK(reconstruct(lex(src)) == src);
    }
}

// ---------------------------------------------------------------------------
// Suite: script / style passthrough
// ---------------------------------------------------------------------------

TEST_SUITE("lex_html — script and style passthrough") {

    TEST_CASE("<script> content is Plain (no JS lexer applied)") {
        std::string src = "<script>var x = 1;</script>";
        auto toks = lex(src);
        // The JS body "var x = 1;" must NOT produce any Keyword tokens
        // (lex_html never recurses into JS lexer)
        CHECK(has_token(toks, Kind::Plain, "var x = 1;"));
    }

    TEST_CASE("<style> content is Plain") {
        std::string src = "<style>body { color: red; }</style>";
        auto toks = lex(src);
        CHECK(has_token(toks, Kind::Plain, "body { color: red; }"));
    }

    TEST_CASE("<SCRIPT> uppercase tag still gets passthrough") {
        std::string src = "<SCRIPT>alert(1);</SCRIPT>";
        auto toks = lex(src);
        CHECK(has_token(toks, Kind::Plain, "alert(1);"));
    }

    TEST_CASE("script open/close tags themselves use Keyword for tag name") {
        std::string src = "<script>x=1;</script>";
        auto toks = lex(src);
        auto kws = of_kind(toks, Kind::Keyword);
        // "script" appears twice: open tag + close tag
        int script_count = 0;
        for (const auto& t : kws)
            if (t.text == "script" || t.text == "SCRIPT") ++script_count;
        CHECK(script_count == 2);
    }

    TEST_CASE("script with unclosed content at EOF does not crash") {
        auto toks = lex("<script>var x = ");
        CHECK_FALSE(toks.empty());
        CHECK(reconstruct(toks) == "<script>var x = ");
    }
}

// ---------------------------------------------------------------------------
// Suite: malformed / tolerance
// ---------------------------------------------------------------------------

TEST_SUITE("lex_html — malformed input tolerance") {

    TEST_CASE("unclosed tag at EOF does not crash") {
        std::string src = "<div class=\"test\"";
        auto toks = lex(src);
        CHECK_FALSE(toks.empty());
        CHECK(reconstruct(toks) == src);
    }

    TEST_CASE("lone < character does not crash") {
        // '<' at end with nothing following
        auto toks = lex("<");
        CHECK(reconstruct(toks) == "<");
    }

    TEST_CASE("stray & without semicolon does not crash") {
        std::string src = "foo & bar";
        auto toks = lex(src);
        CHECK_FALSE(toks.empty());
        CHECK(reconstruct(toks) == src);
    }

    TEST_CASE("deeply nested tags do not crash") {
        std::string src;
        for (int i = 0; i < 50; ++i) src += "<div>";
        src += "content";
        for (int i = 0; i < 50; ++i) src += "</div>";
        auto toks = lex(src);
        CHECK(reconstruct(toks) == src);
    }

    TEST_CASE("empty attribute value") {
        std::string src = R"(<input value="">)";
        auto toks = lex(src);
        CHECK(reconstruct(toks) == src);
        CHECK(has_token(toks, Kind::String, "\"\""));
    }

    TEST_CASE("tag with no name (stray bracket) does not crash") {
        std::string src = "< p>";
        auto toks = lex(src);
        CHECK(reconstruct(toks) == src);
    }
}
