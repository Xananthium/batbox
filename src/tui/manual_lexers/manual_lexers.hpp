// src/tui/manual_lexers/manual_lexers.hpp
// ---------------------------------------------------------------------------
// Internal header: manual lexer declarations.
//
// Each function tokenises its input and returns a vector of Token values.
// Tokens carry a Kind (used to map to ThemeRole) and a string_view into the
// original source.  The source must outlive the returned vector.
//
// This header is NOT part of the public batbox API.  It is only included by
// SyntaxHighlight.cpp and the manual_lexers/*.cpp translation units.
// ---------------------------------------------------------------------------
#pragma once

#include <string_view>
#include <vector>

namespace batbox::tui::detail {

// ============================================================================
// Token — minimal unit of highlighted text
// ============================================================================

struct Token {
    enum class Kind {
        Keyword,   ///< Language keyword          → AccentMagenta
        String,    ///< String / char literal      → AccentCyan
        Comment,   ///< Line or block comment      → Muted
        Number,    ///< Integer or float literal   → Success
        Operator,  ///< Punctuation / operator     → Fg
        Plain,     ///< Identifier / whitespace    → Fg
    };

    Kind           kind;
    std::string_view text; ///< Slice of the original source string_view
};

// ============================================================================
// Manual lexer entry points — one per supported language
// ============================================================================

/// Lex C++ source (C++11 keywords + common operators).
std::vector<Token> lex_cpp(std::string_view src);

/// Lex Python source (Python 3 keywords, string prefixes r/b/f).
std::vector<Token> lex_python(std::string_view src);

/// Lex JavaScript / TypeScript source.
std::vector<Token> lex_js(std::string_view src);

/// Lex TypeScript / TSX source (TS keywords, type annotations, generics, TSX tags).
std::vector<Token> lex_typescript(std::string_view src);

/// Lex Rust source.
std::vector<Token> lex_rust(std::string_view src);

/// Lex Go source.
std::vector<Token> lex_go(std::string_view src);

/// Lex CSS / SCSS / Sass source.
/// Handles selectors, property:value pairs, at-rules, hex colors, units,
/// function calls, block comments, and SCSS extensions ($var, @mixin, //).
std::vector<Token> lex_css(std::string_view src);

/// Lex JSON / JSON5 source (strings, numbers, keywords, structural punctuation).
std::vector<Token> lex_json(std::string_view src);

/// Lex HTML source (tags, attributes, comments, entities, DOCTYPE).
std::vector<Token> lex_html(std::string_view src);

} // namespace batbox::tui::detail
