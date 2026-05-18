// src/tui/SyntaxHighlight.cpp
// ---------------------------------------------------------------------------
// batbox::tui::highlight_code() — manual lexer implementation.
//
// Uses hand-rolled lexers for C++, Python, JS, TS, Rust, Go, HTML, CSS, and JSON.
// Tree-sitter removed in PEXT2 1.4a.
//
// Token → ThemeRole mapping:
//   keyword  → AccentMagenta
//   string   → AccentCyan
//   comment  → Muted
//   number   → Success
//   operator → Fg
//   plain    → Fg
//
// The block's background is set to theme.code_bg.
// Unknown or empty language → plain monospace rendering; no exception is thrown.
// ---------------------------------------------------------------------------

#include <batbox/tui/SyntaxHighlight.hpp>
#include <batbox/tui/ThemeApply.hpp>
#include <batbox/theme/Theme.hpp>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

#include "manual_lexers/manual_lexers.hpp"

namespace batbox::tui {

namespace {

// ============================================================================
// Language normalisation
// ============================================================================

/// Normalise a user-supplied fence language tag to a canonical internal name.
/// Returns an empty string_view for unrecognised tags.
std::string_view normalise_language(std::string_view lang) {
    // Make a lowercase copy for case-insensitive comparison.
    std::string lower(lang);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    std::string_view l(lower);

    if (l == "c")                                    return "c";
    if (l == "c++" || l == "cpp" || l == "cxx" ||
        l == "cc"  || l == "h"   || l == "hpp")     return "cpp";
    if (l == "py" || l == "python" || l == "python3")return "python";
    if (l == "js" || l == "javascript" || l == "mjs" ||
        l == "cjs" || l == "jsx")                    return "javascript";
    if (l == "ts" || l == "typescript" || l == "tsx") return "typescript";
    if (l == "rs" || l == "rust")                    return "rust";
    if (l == "go" || l == "golang")                  return "go";
    if (l == "sh" || l == "bash" || l == "shell" ||
        l == "zsh" || l == "ksh" || l == "fish")    return "bash";
    if (l == "json" || l == "jsonc" || l == "json5") return "json";
    if (l == "html" || l == "htm" || l == "xhtml" ||
        l == "xml")                                 return "html";
    if (l == "css"  || l == "scss" || l == "sass" ||
        l == "less")                                return "css";
    if (l == "md" || l == "markdown")               return "markdown";

    return {};
}

// ============================================================================
// Plain (monospace, no colour) rendering
// ============================================================================

ftxui::Element render_plain(std::string_view code,
                             const batbox::theme::Theme& theme) {
    using namespace ftxui;
    ftxui::Color fg = color_for(theme, ThemeRole::Fg);

    std::vector<Element> lines;
    const char* p   = code.data();
    const char* end = p + code.size();

    while (p < end) {
        const char* line_start = p;
        while (p < end && *p != '\n') ++p;
        std::string line_str(line_start, static_cast<size_t>(p - line_start));
        lines.push_back(text(std::move(line_str)) | color(fg));
        if (p < end) ++p; // consume '\n'
    }

    if (lines.empty()) lines.push_back(text(""));
    return vbox(std::move(lines)) | bgcolor(color_for(theme, ThemeRole::CodeBg));
}

// ============================================================================
// Token-colour → ftxui::Color helper
// ============================================================================

ftxui::Color colour_for_kind(detail::Token::Kind kind,
                              const batbox::theme::Theme& theme) {
    switch (kind) {
        case detail::Token::Kind::Keyword:  return color_for(theme, ThemeRole::AccentMagenta);
        case detail::Token::Kind::String:   return color_for(theme, ThemeRole::AccentCyan);
        case detail::Token::Kind::Comment:  return color_for(theme, ThemeRole::Muted);
        case detail::Token::Kind::Number:   return color_for(theme, ThemeRole::Success);
        case detail::Token::Kind::Operator: return color_for(theme, ThemeRole::Fg);
        case detail::Token::Kind::Plain:    return color_for(theme, ThemeRole::Fg);
    }
    return color_for(theme, ThemeRole::Fg);
}

// ============================================================================
// Manual-lexer rendering: tokens → ftxui::Element
// ============================================================================

ftxui::Element render_tokens(const std::vector<detail::Token>& tokens,
                              const batbox::theme::Theme& theme) {
    using namespace ftxui;

    std::vector<Element> all_lines;
    std::vector<Element> current_line;

    auto flush_line = [&] {
        if (current_line.empty()) {
            all_lines.push_back(text(""));
        } else {
            all_lines.push_back(hbox(std::move(current_line)));
            current_line.clear();
        }
    };

    for (const auto& tok : tokens) {
        ftxui::Color c = colour_for_kind(tok.kind, theme);

        // Tokens may contain embedded newlines (multi-line comments, etc.)
        // Split at newline boundaries so each line is an ftxui hbox row.
        std::string_view sv = tok.text;
        while (!sv.empty()) {
            auto nl = sv.find('\n');
            if (nl == std::string_view::npos) {
                if (!sv.empty())
                    current_line.push_back(text(std::string(sv)) | color(c));
                break;
            }
            // Text before newline
            if (nl > 0)
                current_line.push_back(text(std::string(sv.substr(0, nl))) | color(c));
            flush_line();
            sv = sv.substr(nl + 1);
        }
    }

    // Flush any pending line (last line may not end with '\n')
    flush_line();

    if (all_lines.empty()) all_lines.push_back(text(""));
    return vbox(std::move(all_lines)) | bgcolor(color_for(theme, ThemeRole::CodeBg));
}

} // namespace (anonymous)

// ============================================================================
// Public API
// ============================================================================

ftxui::Element highlight_code(std::string_view language,
                               std::string_view code,
                               const batbox::theme::Theme& theme) {
    std::string_view canon = normalise_language(language);

    // ---- Manual-lexer path ----------------------------------------------
    std::vector<detail::Token> tokens;

    if      (canon == "c" || canon == "cpp")  tokens = detail::lex_cpp(code);
    else if (canon == "python")               tokens = detail::lex_python(code);
    else if (canon == "javascript")           tokens = detail::lex_js(code);
    else if (canon == "typescript")           tokens = detail::lex_typescript(code);
    else if (canon == "rust")                 tokens = detail::lex_rust(code);
    else if (canon == "go")                   tokens = detail::lex_go(code);
    else if (canon == "html")                 tokens = detail::lex_html(code);
    else if (canon == "css")                  tokens = detail::lex_css(code);
    else if (canon == "json")                 tokens = detail::lex_json(code);
    else                                    return render_plain(code, theme);

    return render_tokens(tokens, theme);
}

} // namespace batbox::tui
