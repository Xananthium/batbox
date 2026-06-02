// src/inference/Provider.cpp
// ---------------------------------------------------------------------------
// batbox::inference Provider abstraction implementation (S8 + S9).
//
// OpenAiCompatibleProvider delegates every chat/stream_chat call straight to
// its owned batbox::inference::Client, so all retry/backoff and the
// vllm/ollama/llama-cpp quirk handling live in exactly one place (Client.cpp).
// This translation unit adds only:
//   - the polymorphic plumbing (vtable + delegation),
//   - provider identity resolution for name()/metadata(),
//   - the deterministic canonical model-name normaliser, and
//   - the Chat-vs-Responses routing predicate (currently always Chat).
//
// Provider identity:
//   provider_key_for_config() reproduces the Client's hint-resolution contract
//   (the same rules verified by tests/unit/test_provider_hint.cpp) for the
//   keys Client special-cases, and passes through any other non-empty hint
//   verbatim (so "kimi"/"deepseek" — which are OpenAI-compatible and need no
//   Client quirk — still surface as their own identity).  This duplicates the
//   *rule shape*, not Client's internal symbol: Client's resolve_provider_hint
//   is file-local by design.
// ---------------------------------------------------------------------------

#include <batbox/inference/Provider.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace batbox::inference {

namespace {

/// ASCII lowercase-fold a string copy.
std::string to_lower_copy(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// True when @p s is one of the provider keys the Client special-cases via its
/// hint vocabulary (resolve_provider_hint kKnown set).
bool is_client_known_key(const std::string& s) noexcept {
    static constexpr std::array<std::string_view, 9> kKnown{
        "openai", "vllm", "together", "ollama",
        "anthropic", "groq", "mistral", "lm-studio", "llama-cpp"};
    return std::any_of(kKnown.begin(), kKnown.end(),
                       [&](std::string_view k) { return s == k; });
}

/// Detect a provider key from the base_url for the "auto"/empty-hint path.
/// Mirrors Client.cpp's detect_provider_from_url contract (same precedence).
std::string detect_from_url(std::string_view base_url) {
    const std::string url = to_lower_copy(base_url);
    auto has = [&](std::string_view needle) {
        return url.find(needle) != std::string::npos;
    };
    if (has("together.ai") || has("together.xyz")) return "together";
    if (has("groq.com"))                           return "groq";
    if (has("mistral.ai"))                         return "mistral";
    if (has("anthropic.com") || has("litellm"))    return "anthropic";
    if (has("11434") || has("ollama"))             return "ollama";
    if (has("lmstudio") || has(":1234/"))          return "lm-studio";
    if (has("llama"))                              return "llama-cpp";
    if (has("vllm"))                               return "vllm";
    return "openai";
}

/// Resolve a provider identity key for a Config.
///
/// Contract (a superset of Client's resolve_provider_hint, for identity only):
///   - empty / "auto"            → detect from base_url.
///   - a Client-known key        → that key (lowercased).
///   - any other non-empty hint  → the hint verbatim (lowercased).  This lets
///     OpenAI-compatible-but-unquirked providers (kimi, deepseek, …) keep their
///     own identity even though Client treats them as plain openai semantics.
std::string provider_key_for_config(const batbox::config::Config& cfg) {
    const std::string hint = to_lower_copy(cfg.api.provider_hint);
    if (hint.empty() || hint == "auto") {
        return detect_from_url(cfg.api.base_url);
    }
    if (is_client_known_key(hint)) {
        return hint;
    }
    return hint; // verbatim identity for kimi/deepseek/etc.
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// OpenAiCompatibleProvider
// ---------------------------------------------------------------------------

OpenAiCompatibleProvider::OpenAiCompatibleProvider(
    const batbox::config::Config& cfg,
    bool manages_own_context)
    : cfg_(cfg),
      client_(cfg),
      manages_own_context_(manages_own_context) {}

Result<ChatResponse> OpenAiCompatibleProvider::chat(const ChatRequest& req) {
    // Pure delegation — Client owns quirks + error semantics.
    return client_.chat(req);
}

Result<UsageDelta> OpenAiCompatibleProvider::stream_chat(
    const ChatRequest&                      req,
    std::function<void(const StreamDelta&)> on_delta,
    CancelToken                             ct) {
    // Pure delegation — Client owns retry/backoff, quirks, and cancellation.
    return client_.stream_chat(req, std::move(on_delta), std::move(ct));
}

std::string OpenAiCompatibleProvider::name() const {
    return provider_key_for_config(cfg_);
}

ProviderMetadata OpenAiCompatibleProvider::metadata() const {
    ProviderMetadata md;
    md.name        = provider_key_for_config(cfg_);
    md.base_url    = cfg_.api.base_url;
    md.description = "OpenAI-compatible provider (" + md.name + ")";
    return md;
}

ReasoningTags OpenAiCompatibleProvider::reasoning_tags() const {
    // The reasoning-tag convention is a property of the provider identity.
    // reasoning_tags_for_provider lives in the ReasoningTagProfile leaf TU.
    return reasoning_tags_for_provider(provider_key_for_config(cfg_));
}

// ---------------------------------------------------------------------------
// ProviderRegistry
// ---------------------------------------------------------------------------

std::unique_ptr<Provider> ProviderRegistry::create(
    const batbox::config::Config& cfg,
    bool manages_own_context) {
    // Every endpoint in the current batbox matrix is OpenAI-compatible, so the
    // factory always yields an OpenAiCompatibleProvider.  This is the single
    // seam where a future non-compatible provider would be branched in
    // (keyed off provider_key_for_config(cfg) / cfg.api.base_url).
    return std::make_unique<OpenAiCompatibleProvider>(cfg, manages_own_context);
}

// ---------------------------------------------------------------------------
// map_to_canonical_model — defined in CanonicalModel.cpp (kept in its own leaf
// TU so light consumers like ModelPricing.cpp can link the symbol without the
// Provider/Client/cpr chain).  Declared in Provider.hpp.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// should_use_responses_api
// ---------------------------------------------------------------------------

bool should_use_responses_api(std::string_view /*provider_name*/,
                              std::string_view /*model*/) noexcept {
    // batbox speaks only Chat Completions today.  The Responses API is not yet
    // wired, so every (provider, model) pair routes to Chat Completions.
    // When a Responses-API transport lands, branch here on provider_name/model.
    return false;
}

// ---------------------------------------------------------------------------
// reasoning_tags_for_config — Config → provider identity → tag convention.
// reasoning_tags_for_provider itself is defined in ReasoningTagProfile.cpp.
// ---------------------------------------------------------------------------

ReasoningTags reasoning_tags_for_config(const batbox::config::Config& cfg) {
    return reasoning_tags_for_provider(provider_key_for_config(cfg));
}

} // namespace batbox::inference
