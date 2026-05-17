// src/tui/SyntaxHighlight.cpp
// ---------------------------------------------------------------------------
// batbox::tui::highlight_code() — two-path implementation.
//
// BATBOX_SYNTAX=1 (default): tree-sitter C API, 10 vendored grammars.
// BATBOX_SYNTAX=0           : manual lexers for C++, Python, JS/TS, Rust, Go.
//
// Token → ThemeRole mapping (both paths):
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

// ---------------------------------------------------------------------------
// Tree-sitter path
// ---------------------------------------------------------------------------
#if defined(BATBOX_SYNTAX) && BATBOX_SYNTAX == 1

#include <tree_sitter/api.h>

extern "C" {
    TSLanguage *tree_sitter_c(void);
    TSLanguage *tree_sitter_cpp(void);
    TSLanguage *tree_sitter_python(void);
    TSLanguage *tree_sitter_javascript(void);
    TSLanguage *tree_sitter_typescript(void);
    TSLanguage *tree_sitter_rust(void);
    TSLanguage *tree_sitter_go(void);
    TSLanguage *tree_sitter_bash(void);
    TSLanguage *tree_sitter_json(void);
    TSLanguage *tree_sitter_markdown(void);
}

#else
// Manual lexer path
#include "manual_lexers/manual_lexers.hpp"
#endif

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

#if !(defined(BATBOX_SYNTAX) && BATBOX_SYNTAX == 1)

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

#endif // !(BATBOX_SYNTAX == 1)

// ============================================================================
// Tree-sitter path
// ============================================================================

#if defined(BATBOX_SYNTAX) && BATBOX_SYNTAX == 1

/// Map a tree-sitter node type name to a ThemeRole.
/// Returns Fg for types that should not be specially coloured.
ThemeRole role_for_ts_type(std::string_view type) {
    // ---- Comments
    if (type == "comment"        ||
        type == "line_comment"   ||
        type == "block_comment"  ||
        type == "doc_comment")
        return ThemeRole::Muted;

    // ---- String / character / template literals
    if (type == "string"              ||
        type == "string_literal"      ||
        type == "char_literal"        ||
        type == "raw_string_literal"  ||
        type == "template_string"     ||
        type == "interpreted_string_literal" ||
        type == "raw_string"          ||
        type == "string_fragment"     ||
        type == "string_value")
        return ThemeRole::AccentCyan;

    // ---- Numeric literals
    if (type == "number_literal"      ||
        type == "integer_literal"     ||
        type == "float_literal"       ||
        type == "int_literal"         ||
        type == "imaginary_literal"   ||
        type == "number")
        return ThemeRole::Success;

    // ---- Keywords (both "keyword" node type and specific terminal tokens)
    if (type == "keyword"         ||
        type == "true"            ||
        type == "false"           ||
        type == "null"            ||
        type == "nil"             ||
        type == "none")
        return ThemeRole::AccentMagenta;

    return ThemeRole::Fg;
}

/// Returns true if a node type string looks like a language keyword terminal.
bool is_keyword_node_type(std::string_view type) {
    // Tree-sitter grammars expose keywords as named terminal nodes whose
    // names are the keyword itself.  We use a broad heuristic: an all-lowercase
    // identifier that is a known keyword from any of our 10 languages.
    // The explicit list here avoids false positives on identifiers.
    static constexpr std::array<std::string_view, 120> kKeywords = {{
        // C/C++
        "alignas","alignof","and","and_eq","asm","auto",
        "bitand","bitor","bool","break",
        "case","catch","char","class","compl","concept","const",
        "consteval","constexpr","constinit","const_cast","continue",
        "co_await","co_return","co_yield",
        "decltype","default","delete","do","double","dynamic_cast",
        "else","enum","explicit","export","extern",
        "false","float","for","friend","goto","if","inline","int",
        "long","mutable","namespace","new","noexcept","not","not_eq",
        "nullptr","operator","or","or_eq","private","protected","public",
        "register","reinterpret_cast","requires","return",
        "short","signed","sizeof","static","static_assert","static_cast",
        "struct","switch","template","this","thread_local","throw","true",
        "try","typedef","typeid","typename","union","unsigned","using",
        "virtual","void","volatile","wchar_t","while","xor","xor_eq",
        // Python
        "as","assert","async","await",
        "global","import","in","is","lambda","nonlocal","pass","raise",
        "with","yield",
        // JS/TS
        "abstract","class","const","debugger","delete","extends","finally",
        "function","implements","instanceof","interface","let","module",
        "of","satisfies","super","typeof","var","undefined",
        // Rust
        "crate","dyn","fn","impl","match","mod","move","mut","pub",
        "ref","Self","self","trait","unsafe","use","where",
        // Go
        "chan","defer","fallthrough","func","go","map","package",
        "range","select",
        // Bash
        "then","done","fi","esac","in",
        // Markdown
        "atx_heading","fenced_code_block",
    }};
    return std::find(kKeywords.begin(), kKeywords.end(), type) != kKeywords.end();
}

/// Walk a TSNode subtree depth-first.  For each leaf node, determine its
/// ThemeRole and accumulate coloured spans keyed by (start_byte, end_byte).
struct SpanInfo {
    uint32_t   start_byte;
    uint32_t   end_byte;
    ThemeRole  role;
};

void collect_spans(TSNode node, std::vector<SpanInfo>& spans) {
    if (ts_node_child_count(node) == 0) {
        // Leaf node
        std::string_view type(ts_node_type(node));
        ThemeRole role = role_for_ts_type(type);
        if (role == ThemeRole::Fg && is_keyword_node_type(type))
            role = ThemeRole::AccentMagenta;
        spans.push_back({ts_node_start_byte(node), ts_node_end_byte(node), role});
        return;
    }
    uint32_t n = ts_node_child_count(node);
    for (uint32_t i = 0; i < n; ++i) {
        collect_spans(ts_node_child(node, i), spans);
    }
}

ftxui::Element render_with_treesitter(TSLanguage* grammar,
                                       std::string_view code,
                                       const batbox::theme::Theme& theme) {
    using namespace ftxui;

    TSParser* parser = ts_parser_new();
    ts_parser_set_language(parser, grammar);
    TSTree* tree = ts_parser_parse_string(parser, nullptr,
                                          code.data(),
                                          static_cast<uint32_t>(code.size()));

    std::vector<SpanInfo> spans;
    if (tree) {
        TSNode root = ts_tree_root_node(tree);
        collect_spans(root, spans);
        ts_tree_delete(tree);
    }
    ts_parser_delete(parser);

    // Build coloured output by stitching spans with any uncovered bytes (Fg).
    // Spans from collect_spans cover every byte (leaf nodes are exhaustive), but
    // keep a fallback cursor in case of gaps from error nodes.
    std::vector<Element> all_lines;
    std::vector<Element> current_line;
    uint32_t pos = 0;

    auto flush_line = [&] {
        if (current_line.empty()) {
            all_lines.push_back(text(""));
        } else {
            all_lines.push_back(hbox(std::move(current_line)));
            current_line.clear();
        }
    };

    auto push_text = [&](std::string_view sv, ThemeRole role) {
        Color c = color_for(theme, role);
        while (!sv.empty()) {
            auto nl = sv.find('\n');
            if (nl == std::string_view::npos) {
                if (!sv.empty())
                    current_line.push_back(text(std::string(sv)) | color(c));
                break;
            }
            if (nl > 0)
                current_line.push_back(text(std::string(sv.substr(0, nl))) | color(c));
            flush_line();
            sv = sv.substr(nl + 1);
        }
    };

    for (const auto& sp : spans) {
        if (sp.start_byte > pos) {
            // Gap before this span — render as Fg
            push_text(code.substr(pos, sp.start_byte - pos), ThemeRole::Fg);
        }
        if (sp.end_byte > sp.start_byte) {
            push_text(code.substr(sp.start_byte, sp.end_byte - sp.start_byte), sp.role);
        }
        pos = sp.end_byte;
    }
    // Remaining bytes after last span
    if (pos < code.size()) {
        push_text(code.substr(pos), ThemeRole::Fg);
    }

    flush_line();
    if (all_lines.empty()) all_lines.push_back(text(""));
    return vbox(std::move(all_lines)) | bgcolor(color_for(theme, ThemeRole::CodeBg));
}

#endif // BATBOX_SYNTAX == 1

} // namespace (anonymous)

// ============================================================================
// Public API
// ============================================================================

ftxui::Element highlight_code(std::string_view language,
                               std::string_view code,
                               const batbox::theme::Theme& theme) {
    std::string_view canon = normalise_language(language);

#if defined(BATBOX_SYNTAX) && BATBOX_SYNTAX == 1
    // ---- Tree-sitter path -----------------------------------------------
    TSLanguage* grammar = nullptr;

    if      (canon == "c")          grammar = tree_sitter_c();
    else if (canon == "cpp")        grammar = tree_sitter_cpp();
    else if (canon == "python")     grammar = tree_sitter_python();
    else if (canon == "javascript") grammar = tree_sitter_javascript();
    else if (canon == "typescript") grammar = tree_sitter_typescript();
    else if (canon == "rust")       grammar = tree_sitter_rust();
    else if (canon == "go")         grammar = tree_sitter_go();
    else if (canon == "bash")       grammar = tree_sitter_bash();
    else if (canon == "json")       grammar = tree_sitter_json();
    else if (canon == "markdown")   grammar = tree_sitter_markdown();

    if (!grammar) return render_plain(code, theme);
    return render_with_treesitter(grammar, code, theme);

#else
    // ---- Manual-lexer path ----------------------------------------------
    std::vector<detail::Token> tokens;

    if      (canon == "c" || canon == "cpp")        tokens = detail::lex_cpp(code);
    else if (canon == "python")                      tokens = detail::lex_python(code);
    else if (canon == "javascript" ||
             canon == "typescript")                  tokens = detail::lex_js(code);
    else if (canon == "rust")                        tokens = detail::lex_rust(code);
    else if (canon == "go")                          tokens = detail::lex_go(code);
    else                                             return render_plain(code, theme);

    return render_tokens(tokens, theme);
#endif
}

} // namespace batbox::tui
