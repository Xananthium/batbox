// include/batbox/inference/ProviderHint.hpp
// ---------------------------------------------------------------------------
// batbox::inference — provider-hint resolution (single source of truth).
//
// Why this exists (DIS-1006):
//   "URL/hint → canonical provider name" was implemented TWICE — once in
//   Client.cpp (the wire path: which quirks to apply) and once in Provider.cpp
//   (the identity path: name()/metadata()).  The two copies DIVERGED: Client's
//   bare-port heuristic matched a base_url ending in `:1234` (no trailing
//   slash), Provider's did not — so the same lm-studio endpoint resolved as
//   `lm-studio` on the wire and misdetected as `openai` for identity.  This TU
//   holds the ONE implementation both paths consume.
//
// Surface:
//   detect_provider_from_url(base_url)
//       Auto/empty-hint detection.  Precedence: most-specific domains first,
//       local-port heuristics last; falls back to "openai".
//   resolve_provider_hint(hint, base_url)
//       The Client WIRE-PATH contract: empty/"auto" → detect_provider_from_url;
//       a known-vocabulary key → that key (lowercased); any other value → warns
//       on the "inference.client" channel and falls back to "openai" semantics.
//       Known vocabulary: openai|vllm|together|ollama|anthropic|groq|mistral|
//       lm-studio|llama-cpp (+ "auto"/empty as the detect sentinel).
//
//   NOTE — the Provider IDENTITY path is a deliberate *superset* of this
//   contract: it surfaces unknown-but-OpenAI-compatible hints (kimi, deepseek)
//   verbatim instead of folding them to "openai".  That composition lives in
//   Provider.cpp and is built on detect_provider_from_url() here; it is NOT a
//   second copy of the detection/vocabulary logic.
//
// Dependency-light: std lib + batbox logging only (no cpr / Client chain), so a
// consumer can link this without pulling the HTTP stack.
//
// Standalone build (no CMake), from repo root:
//   c++ -std=c++20 -I include -I build/vcpkg_installed/x64-linux/include \
//       -c src/inference/ProviderHint.cpp -o /tmp/ProviderHint.o
// ---------------------------------------------------------------------------
#pragma once

#include <string>

namespace batbox::inference {

/// Detect the canonical provider name from @p base_url (the "auto"/empty-hint
/// path).  Precedence: most-specific domains first, local-port heuristics last.
/// Falls back to "openai" when no pattern matches.  Case-insensitive.
std::string detect_provider_from_url(const std::string& base_url);

/// Resolve a provider @p hint to a canonical lowercase provider name, using
/// @p base_url for the "auto"/empty case.  This is the Client wire-path
/// contract: an unrecognised hint warns and falls back to "openai" semantics.
std::string resolve_provider_hint(const std::string& hint,
                                  const std::string& base_url);

} // namespace batbox::inference
