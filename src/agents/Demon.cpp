// src/agents/Demon.cpp
// =============================================================================
// batbox::agents::Demon — Party Monster easter-egg sub-agent (CPP 6.8).
//
// Provides:
//   Demon::spec()                    — built-in AgentSpec (Party Monster system prompt)
//   DemonRateLimiter::is_allowed()   — time + token rate-limit check
//   DemonRateLimiter::record_comment() — update rate-limit state after comment
//
// See include/batbox/agents/Demon.hpp for the full design contract.
// =============================================================================

#include <batbox/agents/Demon.hpp>
#include "demon_taglines.hpp"

#include <chrono>
#include <cstddef>
#include <string>

namespace batbox::agents {

// =============================================================================
// Demon::spec — baked-in Party Monster AgentSpec
//
// This spec is used by DemonCmd (CPP S.14) so that the demon agent never
// requires a ~/.batbox/agents/demon.md file to exist on disk.
//
// Persona voice: Michael Alig glamour-ghoul meets Miss Kittin electroclash.
// No tools are allowed — the Demon is a commentator, not a tool-caller.
// =============================================================================

// static
AgentSpec Demon::spec() {
    AgentSpec s;
    s.name        = "demon";
    s.description = "Party Monster easter-egg — glamour-ghoul commentary";
    // No model override: use the parent session's default model.
    s.model       = std::nullopt;
    // No tools: the Demon only speaks; it does not act.
    s.allowed_tools.clear();

    s.prompt_body =
        "You are the Party Monster. You are a glamour-ghoul, a creature of chaos "
        "and nightlife beauty, channelling the spirit of Michael Alig and the "
        "electroclash era of Miss Kittin.\n"
        "\n"
        "Your voice:\n"
        "  - Affirmations: 'Oh my god!', 'Tell me about it!', 'I live for this!'\n"
        "  - Glamorous chaos: fabulous, iconic, deceased (from beauty), serving looks\n"
        "  - Electroclash vocabulary: Detroit, Berlin, dancefloor, raving, machine\n"
        "  - Self-aware camp: arch, meta, theatrical, always slightly too much\n"
        "  - Rate this: 'I rate this nine point five out of ten'\n"
        "  - Taglines like: 'Copyright me. All rights reserved. All rights fabulous.'\n"
        "\n"
        "Rules:\n"
        "  - You are rate-limited. You speak RARELY, but when you speak it is ICONIC.\n"
        "  - Keep comments SHORT: one to three sentences maximum.\n"
        "  - Never offer technical help. You observe and comment. You do not fix.\n"
        "  - React to the DRAMA of the conversation, not the technical content.\n"
        "  - If asked to do a task directly (/demon find TODOs in src/), do it in your voice:\n"
        "    fabulous, dramatic, slightly too much — but still complete the task.\n"
        "\n"
        "You are here because someone typed /demon. Welcome them to the party.\n";

    // source_path is left default (empty) — this spec is baked in, not on disk.
    return s;
}

// =============================================================================
// DemonRateLimiter::is_allowed
// =============================================================================

bool DemonRateLimiter::is_allowed(std::size_t session_token_budget) const noexcept {
    // Check time-based rate limit.
    if (!first_comment) {
        const auto now     = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_comment_time).count();
        if (elapsed < static_cast<long long>(kDemonMinCommentIntervalSec)) {
            return false;
        }
    }

    // Check token-budget rate limit (skipped when budget is unknown / zero).
    if (session_token_budget > 0) {
        const std::size_t token_cap =
            session_token_budget * static_cast<std::size_t>(kDemonMaxTokenPercent) / 100;
        if (tokens_used >= token_cap) {
            return false;
        }
    }

    return true;
}

// =============================================================================
// DemonRateLimiter::record_comment
// =============================================================================

void DemonRateLimiter::record_comment(std::size_t tokens_this_comment) noexcept {
    last_comment_time  = std::chrono::steady_clock::now();
    tokens_used       += tokens_this_comment;
    first_comment      = false;
}

} // namespace batbox::agents
