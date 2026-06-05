// src/inference/CanonicalModel.cpp
// =============================================================================
// map_to_canonical_model — provider model-id normalisation (S8, scope #4).
//
// Declared in Provider.hpp alongside the rest of the provider abstraction, but
// defined in its own translation unit because it is a *pure* string function
// with zero dependency on Client/Config/cpr.  Keeping it here lets light
// consumers (e.g. ModelPricing.cpp, which wires it as a canonical-fallback for
// pricing lookups) link the symbol WITHOUT dragging in the whole Provider/
// Client/HTTP include+link chain.  Provider.cpp keeps the heavier provider
// machinery; this TU stays leaf.
//
// Transform (see the Provider.hpp doc-comment for the contract): trim → keep
// the final '/'-segment → strip a trailing ":tag" → ASCII-lowercase.
// Deterministic and idempotent: f(f(x)) == f(x) for every input.
// =============================================================================

#include <batbox/inference/Provider.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace batbox::inference {

std::string map_to_canonical_model(std::string_view raw) {
    // 1. Trim surrounding ASCII whitespace.
    std::size_t begin = 0;
    std::size_t end   = raw.size();
    auto is_ws = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r'
            || c == '\f' || c == '\v';
    };
    while (begin < end && is_ws(static_cast<unsigned char>(raw[begin])))   ++begin;
    while (end > begin && is_ws(static_cast<unsigned char>(raw[end - 1]))) --end;
    std::string_view s = raw.substr(begin, end - begin);

    if (s.empty()) {
        return std::string{};
    }

    // 2. Strip leading "<vendor>/" prefix segment(s): keep only the final
    //    '/'-segment.  "openai/gpt-4o" → "gpt-4o"; "a/b/c" → "c".  Using the
    //    LAST segment (not just dropping the first) makes this a fixed point so
    //    the function is idempotent.  A trailing '/' is ignored (no empty tail).
    if (const std::size_t slash = s.rfind('/');
        slash != std::string_view::npos && slash + 1 < s.size()) {
        s = s.substr(slash + 1);
    }

    // 3. Strip a trailing ":tag" deployment suffix (Ollama/Together style).
    //    "llama3.2:3b-cloud" → "llama3.2"; "qwen2.5:latest" → "qwen2.5".
    if (const std::size_t colon = s.find(':'); colon != std::string_view::npos) {
        s = s.substr(0, colon);
    }

    // 4. ASCII lowercase-fold the result.
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

} // namespace batbox::inference
