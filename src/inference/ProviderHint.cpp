// src/inference/ProviderHint.cpp
// ---------------------------------------------------------------------------
// batbox::inference — provider-hint resolution (single source of truth, DIS-1006).
//
// The URL→provider detector and the hint resolver used to be duplicated in
// Client.cpp (wire path) and Provider.cpp (identity path); the two copies had
// diverged on the bare-`:1234` lm-studio case.  Both paths now consume these
// definitions, so the vocabulary and the heuristic exist in exactly one place.
//
// Logic here is lifted verbatim from the former Client.cpp anonymous-namespace
// helpers — the wire path's behaviour (incl. the "inference.client" warn
// channel) is unchanged.
//
// Standalone build (no CMake), from repo root:
//   c++ -std=c++20 -I include -I build/vcpkg_installed/x64-linux/include \
//       -c src/inference/ProviderHint.cpp -o /tmp/ProviderHint.o
// ---------------------------------------------------------------------------

#include <batbox/inference/ProviderHint.hpp>

#include <batbox/core/Logging.hpp>

#include <algorithm>
#include <cctype>
#include <string>

namespace batbox::inference {

namespace {

/// Lowercase-fold a string in-place.
std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // anonymous namespace

std::string detect_provider_from_url(const std::string& base_url) {
    const std::string url = to_lower(base_url);
    if (url.find("together.ai") != std::string::npos
            || url.find("together.xyz") != std::string::npos) {
        return "together";
    }
    if (url.find("groq.com") != std::string::npos) {
        return "groq";
    }
    if (url.find("mistral.ai") != std::string::npos) {
        return "mistral";
    }
    if (url.find("anthropic.com") != std::string::npos
            || url.find("litellm") != std::string::npos) {
        return "anthropic";
    }
    if (url.find("11434") != std::string::npos
            || url.find("ollama") != std::string::npos) {
        return "ollama";
    }
    if (url.find("lmstudio") != std::string::npos
            || url.find(":1234/") != std::string::npos
            || url.find(":1234") == url.size() - 5) {
        return "lm-studio";
    }
    if (url.find("llama") != std::string::npos) {
        return "llama-cpp";
    }
    if (url.find("vllm") != std::string::npos) {
        return "vllm";
    }
    return "openai";
}

std::string resolve_provider_hint(const std::string& hint,
                                  const std::string& base_url) {
    const std::string norm = to_lower(hint);

    if (norm.empty() || norm == "auto") {
        return detect_provider_from_url(base_url);
    }

    static const std::string kKnown[] = {
        "openai", "vllm", "together", "ollama",
        "anthropic", "groq", "mistral", "lm-studio", "llama-cpp"
    };
    for (const auto& known : kKnown) {
        if (norm == known) {
            return norm;
        }
    }

    auto lg = batbox::log::get("inference.client");
    lg->warn(
        "BATBOX_PROVIDER_HINT='{}' is not a recognised provider; "
        "falling back to openai semantics. "
        "Valid values: openai|vllm|together|ollama|anthropic|groq|mistral|lm-studio|llama-cpp|auto",
        hint);
    return "openai";
}

} // namespace batbox::inference
