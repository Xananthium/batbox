# src/tui/manual_lexers

Hand-written fallback syntax lexers used when tree-sitter is unavailable (BATBOX_SYNTAX=OFF) or when a language has no tree-sitter grammar linked.

## Files

### manual_lexers.hpp
Internal header declaring the fallback lexer function signature shared by all language lexers: `vector<HighlightSpan> lex_<language>(string_view source)`.

### cpp_lexer.cpp
Tokenises C++ source: keywords, string literals (including raw strings), character literals, line/block comments, preprocessor directives, numeric literals; returns highlight spans for MarkdownRenderer.

### python_lexer.cpp
Tokenises Python source: keywords, triple-quoted and regular string literals, f-strings, comments, decorators, numeric literals; returns highlight spans.

### js_lexer.cpp
Tokenises JavaScript/TypeScript source: keywords, template literals, regular string literals, regex literals, comments, arrow functions; returns highlight spans.

### go_lexer.cpp
Tokenises Go source: keywords, raw string literals (backtick), regular string literals, rune literals, comments, numeric literals; returns highlight spans.

### rust_lexer.cpp
Tokenises Rust source: keywords, lifetime annotations, macro invocations, string literals (including raw strings), char literals, comments, numeric literals with suffixes; returns highlight spans.
