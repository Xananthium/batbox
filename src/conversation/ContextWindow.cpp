// src/conversation/ContextWindow.cpp
// =============================================================================
// Implementation of batbox::conversation::ContextWindow (CPP 3.2).
//
// Responsibilities:
//   - Token estimation via character-class heuristic (no external deps).
//   - Model → context-limit and model → BPE-table resolution.
//   - Compaction threshold check.
//
// Estimator calibration
// ---------------------
// The per-codepoint rates below were derived by running a 19-case diverse
// corpus through tiktoken 0.13.0 (cl100k_base) and performing a grid search
// over {alpha_rate, space_rate, newline_rate, punct_rate} to minimise the
// absolute error in the corpus TOTAL token count.  The search found that
//   alpha=3.5, space=5.0, newline=2.0, punct=2.5
// achieves corpus_error = 0.0% (estimated total = 356, actual total = 356).
//
// Individual strings can have up to ~40% relative error, which is acceptable
// because (a) the compaction decision is made at conversation scale (many
// messages, hundreds of tokens), and (b) a slight overestimate causes early
// compaction which is safe; a slight underestimate is caught next turn.
//
// Codepoint rate tables:
//
//   Category                         cl100k rate   o200k rate
//   ASCII alpha (a-z A-Z)               3.5           3.5
//   ASCII digit (0-9)                   2.8           2.8
//   ASCII space / tab                   5.0           5.0
//   ASCII newline / CR                  2.0           2.0
//   ASCII punct / other                 2.5           2.5
//   CJK Unified Ideographs              1.3           2.0
//   CJK fullwidth punct (FF01-FF60)     1.3           2.0
//   CJK ext blocks + compat             1.3           2.0
//   Emoji / symbols (U+1F000+)          0.4           1.0
//   Other non-ASCII                     1.2           1.2
//
// Per-message overhead: +4 tokens (3 BPE framing + 1 role token)
// Conversation prefix:  +3 tokens (reply priming)
//
// Model context limits and BPE table selection:
//   - o200k: o1-*, o3-* model families
//   - cl100k: everything else
// =============================================================================

#include <batbox/conversation/ContextWindow.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string_view>

namespace batbox::conversation {

// =============================================================================
// Internal: case-insensitive string helpers
// =============================================================================

namespace {

/// Convert ASCII character to lowercase (non-ASCII returned unchanged).
[[nodiscard]] static inline char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c | 0x20) : c;
}

/// Case-insensitive prefix check for ASCII strings.
[[nodiscard]] static bool starts_with_ci(std::string_view s, std::string_view prefix) noexcept {
    if (s.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (ascii_lower(s[i]) != ascii_lower(prefix[i])) return false;
    }
    return true;
}

/// Case-insensitive equality check for ASCII strings.
[[nodiscard]] static bool equals_ci(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
    }
    return true;
}

// =============================================================================
// Internal: UTF-8 codepoint decoder
// =============================================================================

/// Decode one UTF-8 codepoint from [ptr, end).
/// Returns the codepoint and advances *ptr past the consumed bytes.
/// On invalid byte sequences, returns the raw byte value (U+0000..U+00FF)
/// and advances by 1 byte.
[[nodiscard]] static std::uint32_t decode_utf8(const char*& ptr, const char* end) noexcept {
    const auto byte = static_cast<std::uint8_t>(*ptr);

    // Single-byte (ASCII): 0xxxxxxx
    if (byte < 0x80) {
        ++ptr;
        return byte;
    }

    // Two-byte: 110xxxxx 10xxxxxx
    if ((byte & 0xE0) == 0xC0 && ptr + 1 < end &&
        (static_cast<std::uint8_t>(ptr[1]) & 0xC0) == 0x80) {
        std::uint32_t cp = ((byte & 0x1F) << 6) |
                           (static_cast<std::uint8_t>(ptr[1]) & 0x3F);
        ptr += 2;
        return cp;
    }

    // Three-byte: 1110xxxx 10xxxxxx 10xxxxxx
    if ((byte & 0xF0) == 0xE0 && ptr + 2 < end &&
        (static_cast<std::uint8_t>(ptr[1]) & 0xC0) == 0x80 &&
        (static_cast<std::uint8_t>(ptr[2]) & 0xC0) == 0x80) {
        std::uint32_t cp = ((byte & 0x0F) << 12) |
                           ((static_cast<std::uint8_t>(ptr[1]) & 0x3F) << 6) |
                           (static_cast<std::uint8_t>(ptr[2]) & 0x3F);
        ptr += 3;
        return cp;
    }

    // Four-byte: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
    if ((byte & 0xF8) == 0xF0 && ptr + 3 < end &&
        (static_cast<std::uint8_t>(ptr[1]) & 0xC0) == 0x80 &&
        (static_cast<std::uint8_t>(ptr[2]) & 0xC0) == 0x80 &&
        (static_cast<std::uint8_t>(ptr[3]) & 0xC0) == 0x80) {
        std::uint32_t cp = ((byte & 0x07) << 18) |
                           ((static_cast<std::uint8_t>(ptr[1]) & 0x3F) << 12) |
                           ((static_cast<std::uint8_t>(ptr[2]) & 0x3F) << 6) |
                           (static_cast<std::uint8_t>(ptr[3]) & 0x3F);
        ptr += 4;
        return cp;
    }

    // Invalid byte: consume 1 and treat as raw byte value
    ++ptr;
    return byte;
}

/// Returns true when the codepoint falls in a CJK unified ideograph block.
[[nodiscard]] static bool is_cjk(std::uint32_t cp) noexcept {
    return (cp >= 0x4E00 && cp <= 0x9FFF)  // CJK Unified Ideographs
        || (cp >= 0x3400 && cp <= 0x4DBF)  // CJK Extension A
        || (cp >= 0xF900 && cp <= 0xFAFF)  // CJK Compatibility Ideographs
        || (cp >= 0x20000 && cp <= 0x2A6DF) // CJK Extension B
        || (cp >= 0x2A700 && cp <= 0x2B73F) // CJK Extension C
        || (cp >= 0x2B740 && cp <= 0x2B81F) // CJK Extension D
        || (cp >= 0x2B820 && cp <= 0x2CEAF) // CJK Extension E
        || (cp >= 0xFF01 && cp <= 0xFF60)   // CJK fullwidth punctuation
        || (cp >= 0x3000 && cp <= 0x303F);  // CJK Symbols and Punctuation
}

/// Returns true for codepoints in the Supplementary Multilingual Plane emoji
/// ranges (U+1F000 and above), as well as common symbol blocks.
[[nodiscard]] static bool is_emoji(std::uint32_t cp) noexcept {
    return cp >= 0x1F000;  // SMP and beyond: emoji, symbols, supplementary chars
}

// =============================================================================
// Rate constants
// =============================================================================

// All rates are stored as 10x integers to avoid floating-point division
// in the inner loop. The accumulator counts (chars * 10) per category, and
// we divide by the rate at the end.

// Rates are "chars per token * 10":
static constexpr int kAlphaRate    = 35;  // 3.5 chars/token
static constexpr int kDigitRate    = 28;  // 2.8
static constexpr int kSpaceRate    = 50;  // 5.0
static constexpr int kNewlineRate  = 20;  // 2.0
static constexpr int kPunctRate    = 25;  // 2.5
static constexpr int kCjkRateCl    = 13;  // 1.3 (cl100k)
static constexpr int kCjkRateO2    = 20;  // 2.0 (o200k)
static constexpr int kEmojiRateCl  =  4;  // 0.4 chars/token → 2.5 tokens/emoji (cl100k)
static constexpr int kEmojiRateO2  = 10;  // 1.0 (o200k)
static constexpr int kOtherNaRate  = 12;  // 1.2

// Multiply: tokens = (count * 10) / rate.
// Using integer arithmetic with rounding: (count * 10 + rate/2) / rate.
[[nodiscard]] static inline std::size_t rate_div(std::size_t count, int rate) noexcept {
    if (count == 0) return 0;
    return (count * 10 + static_cast<std::size_t>(rate) / 2) / static_cast<std::size_t>(rate);
}

} // anonymous namespace

// =============================================================================
// ContextWindow::context_limit_for_model
// =============================================================================

size_t ContextWindow::context_limit_for_model(std::string_view model_name) noexcept {
    // --- o3 family ---
    if (starts_with_ci(model_name, "o3")) {
        return 200'000;
    }
    // --- o1 family ---
    if (equals_ci(model_name, "o1") || equals_ci(model_name, "o1-pro")) {
        return 200'000;
    }
    if (starts_with_ci(model_name, "o1-preview")) {
        return 128'000;
    }
    if (starts_with_ci(model_name, "o1-mini")) {
        return 128'000;
    }
    if (starts_with_ci(model_name, "o1-")) {
        return 128'000;
    }
    // --- gpt-4o family ---
    if (starts_with_ci(model_name, "gpt-4o")) {
        return 128'000;
    }
    // --- gpt-4-turbo family ---
    if (starts_with_ci(model_name, "gpt-4-turbo")) {
        return 128'000;
    }
    // --- gpt-4 base (8k context) ---
    if (equals_ci(model_name, "gpt-4") || starts_with_ci(model_name, "gpt-4-0")) {
        return 8'192;
    }
    // --- gpt-3.5-turbo ---
    if (starts_with_ci(model_name, "gpt-3.5-turbo")) {
        return 16'385;
    }
    // --- gpt-3.5 ---
    if (starts_with_ci(model_name, "gpt-3.5")) {
        return 4'096;
    }
    // Default: assume modern large context
    return 128'000;
}

// =============================================================================
// ContextWindow::uses_o200k
// =============================================================================

bool ContextWindow::uses_o200k(std::string_view model_name) noexcept {
    // o1 family
    if (starts_with_ci(model_name, "o1")) return true;
    // o3 family
    if (starts_with_ci(model_name, "o3")) return true;
    return false;
}

// =============================================================================
// ContextWindow::estimate_string
// =============================================================================

size_t ContextWindow::estimate_string(std::string_view text, bool is_o200k) noexcept {
    if (text.empty()) return 0;

    const int cjk_rate   = is_o200k ? kCjkRateO2   : kCjkRateCl;
    const int emoji_rate = is_o200k ? kEmojiRateO2  : kEmojiRateCl;

    // Codepoint class accumulators
    std::size_t alpha   = 0;  // ASCII letters
    std::size_t digit   = 0;  // ASCII digits
    std::size_t space   = 0;  // ASCII space / tab
    std::size_t newline = 0;  // ASCII newline / carriage-return
    std::size_t punct   = 0;  // other ASCII
    std::size_t cjk_cnt = 0;  // CJK ideographs + fullwidth
    std::size_t emoji_cnt = 0; // Emoji / SMP symbols
    std::size_t other_na  = 0; // Other non-ASCII (Latin-extended, Greek, etc.)

    const char* ptr = text.data();
    const char* end = ptr + text.size();

    while (ptr < end) {
        const std::uint32_t cp = decode_utf8(ptr, end);

        if (cp < 128) {
            // ASCII: classify by character type
            if (cp >= 'a' && cp <= 'z') { ++alpha; }
            else if (cp >= 'A' && cp <= 'Z') { ++alpha; }
            else if (cp >= '0' && cp <= '9') { ++digit; }
            else if (cp == ' ' || cp == '\t') { ++space; }
            else if (cp == '\n' || cp == '\r') { ++newline; }
            else { ++punct; }
        } else {
            // Non-ASCII: classify by Unicode block
            if (is_emoji(cp))       { ++emoji_cnt; }
            else if (is_cjk(cp))    { ++cjk_cnt; }
            else                    { ++other_na; }
        }
    }

    // Accumulate token estimates for each category
    const std::size_t tokens =
          rate_div(alpha,     kAlphaRate)
        + rate_div(digit,     kDigitRate)
        + rate_div(space,     kSpaceRate)
        + rate_div(newline,   kNewlineRate)
        + rate_div(punct,     kPunctRate)
        + rate_div(cjk_cnt,   cjk_rate)
        + rate_div(emoji_cnt, emoji_rate)
        + rate_div(other_na,  kOtherNaRate);

    return tokens;
}

// =============================================================================
// ContextWindow constructor
// =============================================================================

ContextWindow::ContextWindow(const batbox::config::Config& cfg)
    : model_context_limit_(context_limit_for_model(cfg.api.default_model))
    , auto_compact_at_pct_(cfg.compact.auto_compact_at_pct)
    , is_o200k_(uses_o200k(cfg.api.default_model))
{
    // Clamp the percentage to a sane range [1, 100] to prevent
    // divide-by-zero or nonsensical thresholds.
    if (auto_compact_at_pct_ < 1)   auto_compact_at_pct_ = 1;
    if (auto_compact_at_pct_ > 100) auto_compact_at_pct_ = 100;
}

// =============================================================================
// ContextWindow::estimate_tokens_from_bytes (G9 bytes/4 path)
// =============================================================================
// Primary estimator used when a serialized request body is available.
// This is the path adopted by Conversation::run_turn() (G9 / A1).
//
// bytes / 4 is a ±20% slack estimate; that is why the compact threshold default
// is 80% not 95%.  The overhead of tool schemas, system prompt, and JSON
// encoding is automatically included because we measure the full wire body.

// static free function
size_t ContextWindow::estimate_tokens_from_bytes(std::size_t serialized_bytes) noexcept {
    return serialized_bytes / 4;
}

// =============================================================================
// ContextWindow::estimate_tokens (walking path — fallback)
// =============================================================================
// Kept as a fallback for callers that do not have a serialized request body.
// Conversation::run_turn() uses estimate_tokens_from_bytes() instead (G9).

size_t ContextWindow::estimate_tokens(const std::vector<Message>& messages) const {
    // Conversation-level prefix overhead (reply priming): 3 tokens.
    std::size_t total = 3;

    for (const auto& msg : messages) {
        // Per-message format overhead: 3 BPE framing tokens + 1 role token.
        total += 4;
        // Content token estimate.
        total += estimate_string(msg.content, is_o200k_);
    }

    return total;
}

// =============================================================================
// ContextWindow::needs_compact
// =============================================================================

bool ContextWindow::needs_compact(size_t estimated_tokens) const noexcept {
    // Threshold = auto_compact_at_pct_ * model_context_limit_ / 100
    // Use integer arithmetic; multiply first to avoid truncation.
    const std::size_t threshold =
        (model_context_limit_ * static_cast<std::size_t>(auto_compact_at_pct_)) / 100;
    return estimated_tokens >= threshold;
}

// =============================================================================
// ContextWindow::set_model_context_limit
// =============================================================================

void ContextWindow::set_model_context_limit(size_t tokens) noexcept {
    model_context_limit_ = tokens;
}

// =============================================================================
// ContextWindow::set_model
// =============================================================================

void ContextWindow::set_model(std::string_view model_name) noexcept {
    model_context_limit_ = context_limit_for_model(model_name);
    is_o200k_            = uses_o200k(model_name);
}

} // namespace batbox::conversation
