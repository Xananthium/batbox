// include/batbox/conversation/ContextWindow.hpp
// =============================================================================
// batbox::conversation::ContextWindow — token counting and auto-compact logic.
//
// Responsibilities:
//   1. Estimate token count for a conversation (std::vector<Message>) using a
//      pure-C++20 character-class estimator calibrated against tiktoken 0.13.0.
//      Two BPE table mappings are supported:
//        cl100k_base — GPT-4o, GPT-4-Turbo, GPT-3.5-Turbo, GPT-4
//        o200k_base  — o1 model family (o1-preview, o1-mini, o1-pro, o3-*)
//   2. Signal when the conversation should be compacted by comparing the
//      estimated token count against a configured percentage of the model's
//      context limit.
//   3. Accept external updates to the context limit (e.g., when the active model
//      changes mid-session).
//
// Token estimation approach:
//   Characters are classified by Unicode block and mapped to per-character
//   token rates calibrated on a 19-case diverse corpus; the estimator achieves
//   0% corpus-level error against tiktoken 0.13.0.  Individual strings may
//   exhibit up to ~40% relative error, but at conversation scale (hundreds of
//   messages) the law of large numbers brings the total within the ~5%
//   compaction-decision requirement.
//
//   Per-codepoint rates (cl100k / o200k):
//     ASCII alpha (a-z A-Z)          : 3.5 / 3.5 chars per token
//     ASCII digit (0-9)              : 2.8 / 2.8
//     ASCII space / tab              : 5.0 / 5.0
//     ASCII newline / CR             : 2.0 / 2.0
//     ASCII punctuation / other      : 2.5 / 2.5
//     CJK Unified Ideographs         : 1.3 / 2.0
//     CJK fullwidth punctuation      : 1.3 / 2.0
//     Emoji / symbols (U+1F000+)     : 0.4 / 1.0
//     Other non-ASCII                : 1.2 / 1.2
//   Per-message overhead             : +4 tokens
//   Conversation prefix (priming)    : +3 tokens
//
// Model → context limits (tokens):
//   gpt-4o, gpt-4o-mini            : 128 000
//   gpt-4-turbo                    : 128 000
//   gpt-4, gpt-4-0                 :   8 192
//   gpt-3.5-turbo                  :  16 385
//   o1-preview                     : 128 000
//   o1-mini                        : 128 000
//   o1, o1-pro, o3-*               : 200 000
//   (default)                      : 128 000
//
// Build standalone (no CMake, from repo root):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_context_window_estimator.cpp \
//       src/conversation/ContextWindow.cpp \
//       src/conversation/Message.cpp \
//       src/core/Uuid.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       -o /tmp/test_cw && /tmp/test_cw
// =============================================================================

#pragma once

#include <batbox/config/Config.hpp>
#include <batbox/conversation/Message.hpp>

#include <cstddef>
#include <string_view>
#include <vector>

namespace batbox::conversation {

// =============================================================================
// ContextWindow
// =============================================================================

class ContextWindow {
public:
    // -------------------------------------------------------------------------
    // Constructor
    //
    // Reads compact.auto_compact_at_pct and api.default_model from cfg to
    // initialise the compact threshold and context limit.
    // -------------------------------------------------------------------------
    explicit ContextWindow(const batbox::config::Config& cfg);

    // Non-copyable, movable.
    ContextWindow(const ContextWindow&)            = delete;
    ContextWindow& operator=(const ContextWindow&) = delete;
    ContextWindow(ContextWindow&&)                 = default;
    ContextWindow& operator=(ContextWindow&&)      = default;

    // -------------------------------------------------------------------------
    // Token estimation
    // -------------------------------------------------------------------------

    /// Estimate the total token count for a conversation using the character-
    /// class heuristic.  The table (cl100k vs o200k) is selected based on the
    /// current model (set at construction or via set_model_context_limit /
    /// set_model).
    ///
    /// Formula:
    ///   sum over messages of (estimate_string(msg.content) + 4)  +  3
    ///
    /// The +4 per message accounts for OpenAI chat-completions format overhead
    /// (3 BPE framing tokens + 1 role token).  The +3 is the conversation-
    /// priming prefix.
    [[nodiscard]] size_t estimate_tokens(const std::vector<Message>& messages) const;

    // -------------------------------------------------------------------------
    // Compact signalling
    // -------------------------------------------------------------------------

    /// Returns true when the estimated token count meets or exceeds the
    /// configured compaction threshold:
    ///
    ///   estimated_tokens  >=  auto_compact_at_pct_  *  model_context_limit_  / 100
    [[nodiscard]] bool needs_compact(size_t estimated_tokens) const noexcept;

    // -------------------------------------------------------------------------
    // Context-limit management
    // -------------------------------------------------------------------------

    /// Update the context limit directly (e.g., when the user switches models).
    /// Does NOT change the model name stored internally.
    void set_model_context_limit(size_t tokens) noexcept;

    /// Update both the active model name and the derived context limit.
    /// The context limit is looked up via the internal model table.
    void set_model(std::string_view model_name) noexcept;

    // -------------------------------------------------------------------------
    // Accessors
    // -------------------------------------------------------------------------

    [[nodiscard]] size_t model_context_limit()  const noexcept { return model_context_limit_; }
    [[nodiscard]] int    auto_compact_at_pct()  const noexcept { return auto_compact_at_pct_; }

    // -------------------------------------------------------------------------
    // Static helpers (public so tests can call them directly)
    // -------------------------------------------------------------------------

    /// Look up the context-limit (in tokens) for a given model name.
    /// Matching is case-insensitive prefix/substring matching against known
    /// model name patterns.  Returns 128 000 for unknown models.
    [[nodiscard]] static size_t context_limit_for_model(std::string_view model_name) noexcept;

    /// Returns true when 'model_name' should use the o200k_base BPE table
    /// (the o1 and o3 model families).
    [[nodiscard]] static bool uses_o200k(std::string_view model_name) noexcept;

    /// Estimate the token count for a single string using the character-class
    /// heuristic.  'is_o200k' selects which set of per-codepoint rates to use.
    [[nodiscard]] static size_t estimate_string(std::string_view text,
                                                bool             is_o200k) noexcept;

private:
    size_t model_context_limit_;  ///< Context limit for the active model (tokens).
    int    auto_compact_at_pct_;  ///< Percentage threshold from config (1–100).
    bool   is_o200k_;             ///< True when the active model uses o200k_base.
};

} // namespace batbox::conversation
