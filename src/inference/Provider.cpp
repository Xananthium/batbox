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
//   provider_key_for_config() resolves the identity key shown by name()/
//   metadata().  Its contract is a deliberate *superset* of the Client wire
//   path's resolve_provider_hint: empty/"auto" detects from base_url, and any
//   non-empty hint surfaces verbatim (lowercased) — so OpenAI-compatible-but-
//   unquirked providers ("kimi"/"deepseek") keep their own identity even though
//   Client folds them to plain openai semantics.  The URL→provider detection
//   and the hint vocabulary are NOT re-implemented here: both come from the one
//   shared source in ProviderHint.{hpp,cpp} (DIS-1006), so the bare-`:1234`
//   lm-studio case now resolves identically on the wire and identity paths.
// ---------------------------------------------------------------------------

#include <batbox/inference/Provider.hpp>
#include <batbox/inference/ProviderHint.hpp>

#include <algorithm>
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

/// Resolve a provider identity key for a Config (the name()/metadata() value).
///
/// Contract — a superset of the shared wire-path resolve_provider_hint:
///   - empty / "auto"           → detect_provider_from_url (the shared detector).
///   - any other non-empty hint → that hint verbatim (lowercased).  Both the
///     Client-known keys and unknown-but-OpenAI-compatible hints (kimi,
///     deepseek, …) surface as their own identity here; the wire path's
///     warn-and-fall-back-to-openai applies only to which *quirks* run, not to
///     identity.
std::string provider_key_for_config(const batbox::config::Config& cfg) {
    const std::string hint = to_lower_copy(cfg.api.provider_hint);
    if (hint.empty() || hint == "auto") {
        return detect_provider_from_url(cfg.api.base_url);
    }
    return hint; // verbatim identity (known keys + kimi/deepseek/etc.)
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
