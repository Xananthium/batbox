// src/inference/ReasoningTagProfile.cpp
// ---------------------------------------------------------------------------
// batbox::inference — reasoning_tags_for_provider (S10 provider profile).
//
// The S8 provider profile declares, per provider, which inline reasoning-tag
// convention that endpoint uses (or that it uses none and relies on the
// structured delta.reasoning_content field instead).  This is a pure function of
// the canonical provider name — data-light, no models.json lookup — exactly like
// its profile siblings map_to_canonical_model() and should_use_responses_api().
//
// Kept in its own leaf TU (mirrors CanonicalModel.cpp) so light consumers can
// link the profile without dragging in the Provider/Client/cpr chain: this TU
// depends only on ThinkSplitter.hpp (for ReasoningTags) and the standard library.
//
// Conventions (the matrix batbox targets today):
//   - openai / groq / mistral / together  → NONE.  These surface reasoning only
//     via the structured field; declaring none keeps the splitter a pass-through
//     so ordinary content is never scanned for tags.
//   - anthropic                           → `<thinking>` / `</thinking>`.
//   - everyone else (deepseek, ollama, vllm, llama-cpp, lm-studio, kimi, and any
//     unknown/auto provider)              → the de-facto `<think>` / `</think>`
//     default, because that is what raw DeepSeek-R1 and the common local configs
//     emit inline.
// ---------------------------------------------------------------------------

#include <batbox/inference/ThinkSplitter.hpp>   // ReasoningTags + declaration

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace batbox::inference {

namespace {

std::string to_lower_copy(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool one_of(const std::string& key, std::initializer_list<std::string_view> set) {
    return std::any_of(set.begin(), set.end(),
                       [&](std::string_view k) { return key == k; });
}

} // anonymous namespace

ReasoningTags reasoning_tags_for_provider(std::string_view provider_name) {
    const std::string p = to_lower_copy(provider_name);

    // Structured-field providers: no inline tags → splitter is a pass-through.
    if (one_of(p, {"openai", "groq", "mistral", "together"})) {
        return ReasoningTags::none();
    }

    // Anthropic-style inline convention.
    if (p == "anthropic") {
        return ReasoningTags{"<thinking>", "</thinking>"};
    }

    // Default inline convention for everyone else (deepseek/ollama/vllm/
    // llama-cpp/lm-studio/kimi/unknown).
    return ReasoningTags{};   // "<think>" / "</think>"
}

} // namespace batbox::inference
