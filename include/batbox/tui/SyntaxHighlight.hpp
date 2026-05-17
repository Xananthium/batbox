// include/batbox/tui/SyntaxHighlight.hpp
// ---------------------------------------------------------------------------
// batbox::tui::highlight_code()
//
// Design
// ------
// highlight_code(language, code, theme) converts a fenced code block into a
// coloured ftxui::Element.  Two paths exist:
//
//   BATBOX_SYNTAX == 1 (default ON):
//     Uses the tree-sitter C API with one of 10 vendored grammars (c, cpp,
//     python, javascript, typescript, rust, go, bash, json, markdown).
//     Node-type strings from the parse tree are mapped to ThemeRoles; the
//     resulting coloured spans are assembled into an ftxui vbox of hbox lines.
//
//   BATBOX_SYNTAX == 0:
//     Falls back to hand-rolled manual lexers for 5 languages:
//     C++, Python, JS/TS, Rust, Go.  Each lexer emits token kinds
//     (Keyword, String, Comment, Number, Operator, Plain) which are mapped to
//     the same ThemeRoles as the tree-sitter path.
//
// Unknown or empty language string → plain monospace rendering; no exception.
//
// Token → ThemeRole mapping (both paths):
//   keyword    → AccentMagenta
//   string     → AccentCyan
//   comment    → Muted
//   number     → Success
//   operator   → Fg
//   plain      → Fg
//   block bg   → CodeBg
//
// Integration
// -----------
// Called by MarkdownRender for every recognised fenced code block:
//
//   ftxui::Element block =
//       batbox::tui::highlight_code("cpp", code_text, active_theme);
//   elements.push_back(block);
//
// The caller owns the returned Element; it is safe to store it in a
// std::vector<ftxui::Element>.
// ---------------------------------------------------------------------------
#pragma once

#include <batbox/theme/Theme.hpp>

#include <ftxui/dom/elements.hpp>

#include <string_view>

namespace batbox::tui {

// ============================================================================
// highlight_code()
// ============================================================================

/// Tokenise and colour `code` according to `language` using either the
/// tree-sitter backend (BATBOX_SYNTAX=1) or the manual-lexer fallback
/// (BATBOX_SYNTAX=0).
///
/// Parameters:
///   language — fence language tag (e.g. "cpp", "python", "rust").
///              Case-insensitive aliases are accepted; see SyntaxHighlight.cpp
///              for the full normalisation table.
///   code     — raw source text of the code block (UTF-8, newline terminated).
///   theme    — active theme; colours are drawn from ThemeRole fields.
///
/// Returns:
///   An ftxui::Element (vbox of hbox lines) with inline colour attributes.
///   Background is theme.code_bg applied to the outermost container.
///
/// Never throws.  Unknown or empty `language` falls back to plain rendering.
[[nodiscard]]
ftxui::Element highlight_code(std::string_view language,
                               std::string_view code,
                               const batbox::theme::Theme& theme);

} // namespace batbox::tui
