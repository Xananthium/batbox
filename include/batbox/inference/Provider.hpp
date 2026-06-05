// include/batbox/inference/Provider.hpp
// ---------------------------------------------------------------------------
// batbox::inference — Provider abstraction (S8 + S9).
//
// This header defines the provider-abstraction layer that sits *around* the
// existing batbox::inference::Client (the concrete cpr-based OpenAI-compatible
// HTTP client).  The Client already owns retry/backoff and the per-provider
// quirk handling (resolve_provider_hint / apply_provider_quirks); this layer
// adds a polymorphic seam so batbox can grow non-OpenAI-compatible providers
// (e.g. a CLI that manages its own context window) without touching call-sites.
//
// Shape (lifted from goose providers/base.rs::Provider, re-implemented here):
//   Provider                    — pure-virtual interface (chat / stream_chat +
//                                 identity/metadata accessors).
//   OpenAiCompatibleProvider    — the one concrete Provider that parameterises
//                                 openai/ollama/vllm/llama-cpp/together/groq/
//                                 mistral/lm-studio/kimi/deepseek.  Owns a Client
//                                 and delegates all HTTP to it.
//   ProviderRegistry            — factory: Config → unique_ptr<Provider>.
//   map_to_canonical_model()    — provider-specific model id → canonical form.
//   should_use_responses_api()  — Chat-Completions vs Responses-API routing seam.
//
// S9 — manages_own_context():
//   Provider::manages_own_context() defaults to false (batbox owns the window
//   and runs compaction).  A provider whose underlying CLI already owns the
//   context window can override it to return true so batbox stands down its
//   compaction.  OpenAiCompatibleProvider returns false by default; the opt-out
//   is demonstrated via a constructor flag (see below).
//
// Threading:
//   Like Client, a Provider instance is NOT shared across threads.  Construct
//   one per thread or guard external sharing with a mutex.  The owned Client
//   reads the Config by const-ref at call time, so hot-reloads are picked up.
// ---------------------------------------------------------------------------

#pragma once

#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/config/Config.hpp>
#include <batbox/inference/ChatRequest.hpp>
#include <batbox/inference/ChatResponse.hpp>
#include <batbox/inference/Client.hpp>
#include <batbox/inference/ThinkSplitter.hpp>   // ReasoningTags (S10 provider profile)

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace batbox::inference {

// ============================================================================
// ProviderMetadata — provider identity / static description
// ============================================================================

/// Static, human-facing identity for a Provider instance.
///
/// `name` is the canonical lowercase provider key as produced by the Client's
/// resolve_provider_hint (e.g. "openai", "ollama", "vllm", "groq", "kimi",
/// "deepseek").  `base_url` is the resolved endpoint the provider will POST to.
struct ProviderMetadata {
    /// Canonical lowercase provider key (resolve_provider_hint output, or the
    /// raw hint for keys Client does not special-case such as kimi/deepseek).
    std::string name;

    /// Endpoint base URL the provider posts to (Config::api.base_url).
    std::string base_url;

    /// Human-readable description for /config show and diagnostics.
    std::string description;
};

// ============================================================================
// Provider — pure-virtual provider interface
// ============================================================================

/// Polymorphic inference provider.
///
/// The method surface intentionally mirrors batbox::inference::Client so that
/// existing call-sites can migrate to the abstraction with no signature churn:
///   - chat()        : non-streaming completion        (Client::chat)
///   - stream_chat() : SSE streaming completion         (Client::stream_chat)
/// plus provider-identity/metadata accessors and the S9 context-ownership hook.
class Provider {
public:
    virtual ~Provider() = default;

    // -- Inference surface ---------------------------------------------------

    /// Non-streaming chat completion.  See Client::chat for error semantics.
    [[nodiscard]] virtual Result<ChatResponse> chat(const ChatRequest& req) = 0;

    /// Streaming chat completion via SSE.  See Client::stream_chat for error
    /// semantics, retry policy, and cancellation behaviour.
    [[nodiscard]] virtual Result<UsageDelta> stream_chat(
        const ChatRequest&                      req,
        std::function<void(const StreamDelta&)> on_delta,
        CancelToken                             ct) = 0;

    // -- Identity / metadata -------------------------------------------------

    /// Canonical lowercase provider key, e.g. "openai", "ollama", "groq".
    [[nodiscard]] virtual std::string name() const = 0;

    /// Full provider metadata (name + base_url + description).
    [[nodiscard]] virtual ProviderMetadata metadata() const = 0;

    // -- S9: context-ownership opt-out hook ----------------------------------

    /// Whether the provider's underlying backend manages its own context
    /// window.  Default false: batbox manages the window and runs compaction.
    /// A provider whose CLI/runtime already owns the window (cf. goose
    /// claude_code.rs::manages_own_context) returns true so batbox's compaction
    /// stands down.
    [[nodiscard]] virtual bool manages_own_context() const { return false; }
};

// ============================================================================
// OpenAiCompatibleProvider — concrete Provider wrapping Client
// ============================================================================

/// The single concrete Provider that parameterises every OpenAI-compatible
/// endpoint (openai/ollama/vllm/llama-cpp/together/groq/mistral/lm-studio/
/// kimi/deepseek).  It OWNS a batbox::inference::Client and delegates the
/// actual HTTP — including all retry/backoff and the vllm/ollama/llama-cpp
/// quirk handling — to that Client.  No quirk logic is duplicated here.
class OpenAiCompatibleProvider final : public Provider {
public:
    /// Construct from a Config.  The Config is held by const-ref (same lifetime
    /// contract as Client): the caller must keep @p cfg alive for the lifetime
    /// of this provider.
    ///
    /// @param cfg                  Runtime config (api.base_url / api.provider_hint).
    /// @param manages_own_context  S9 opt-out flag.  Defaults to false (the
    ///                             normal case).  Set true to model a backend
    ///                             that owns its own context window so batbox
    ///                             compaction stands down.
    explicit OpenAiCompatibleProvider(const batbox::config::Config& cfg,
                                      bool manages_own_context = false);

    [[nodiscard]] Result<ChatResponse> chat(const ChatRequest& req) override;

    [[nodiscard]] Result<UsageDelta> stream_chat(
        const ChatRequest&                      req,
        std::function<void(const StreamDelta&)> on_delta,
        CancelToken                             ct) override;

    [[nodiscard]] std::string       name() const override;
    [[nodiscard]] ProviderMetadata  metadata() const override;
    [[nodiscard]] bool              manages_own_context() const override {
        return manages_own_context_;
    }

    /// S10: the inline reasoning-tag convention this provider declares (the
    /// open/close markers ThinkSplitter should scan `content` for, or "none"
    /// when the provider uses the structured delta.reasoning_content field).
    /// Sourced from reasoning_tags_for_provider(name()).
    [[nodiscard]] ReasoningTags     reasoning_tags() const;

private:
    const batbox::config::Config& cfg_;
    Client                        client_;
    bool                          manages_own_context_;
};

// ============================================================================
// ProviderRegistry — factory
// ============================================================================

/// Factory that constructs the correct Provider for a given Config.
///
/// Mirrors goose provider_registry.rs::ProviderRegistry.  It consumes exactly
/// the inputs the Client already consumes for provider resolution
/// (config.api.provider_hint / config.api.base_url): for the current batbox
/// endpoint matrix every provider is OpenAI-compatible, so create() always
/// returns an OpenAiCompatibleProvider.  The factory is the single seam where a
/// future non-compatible provider (e.g. one returning manages_own_context()==true)
/// would be branched in.
class ProviderRegistry {
public:
    /// Construct a Provider for @p cfg.
    ///
    /// @param cfg                  Runtime config; held by const-ref by the
    ///                             returned provider — must outlive it.
    /// @param manages_own_context  Forwarded to OpenAiCompatibleProvider's S9
    ///                             flag.  Defaults to false.
    /// @returns                    A non-null unique_ptr<Provider>.
    [[nodiscard]] static std::unique_ptr<Provider> create(
        const batbox::config::Config& cfg,
        bool manages_own_context = false);
};

// ============================================================================
// map_to_canonical_model — provider model-id normalisation
// ============================================================================

/// Normalise a provider-specific model id to a canonical form.
///
/// Lifted in spirit from goose canonical/name_builder.rs::map_to_canonical_model.
/// Deterministic and data-light (no models.json lookup): the transform is a
/// pure function of the input string.
///
/// Rules (applied in order):
///   1. Trim surrounding ASCII whitespace.
///   2. Strip any leading provider/vendor path segment(s): keep only the final
///      '/'-delimited segment (e.g. "openai/gpt-4o" → "gpt-4o",
///      "mistralai/magistral-small" → "magistral-small", "a/b/c" → "c").
///      Keeping the last segment (rather than dropping only the first) is what
///      makes the transform a fixed point — see the idempotence note below.
///   3. Strip a trailing deployment tag introduced by ':' (Ollama/Together
///      style, e.g. "llama3.2:3b-cloud" → "llama3.2", "qwen2.5:latest" →
///      "qwen2.5").
///   4. Lowercase the result (model ids are case-insensitive across the
///      OpenAI-compatible providers batbox targets).
///
/// Idempotent: map_to_canonical_model(map_to_canonical_model(x)) ==
///             map_to_canonical_model(x) for every input x.
[[nodiscard]] std::string map_to_canonical_model(std::string_view raw);

// ============================================================================
// should_use_responses_api — Chat-Completions vs Responses-API routing seam
// ============================================================================

/// Routing predicate: should this (provider, model) pair be sent to the OpenAI
/// Responses API (POST /v1/responses) instead of Chat Completions
/// (POST /v1/chat/completions)?
///
/// Lifted from goose openai.rs::should_use_responses_api.  batbox currently
/// speaks ONLY Chat Completions, so this predicate returns false for every
/// current endpoint.  It exists as the documented routing seam so the
/// abstraction is complete: when batbox gains a Responses-API transport, this
/// is the single place that decides the route.  Until then the boundary is:
/// "always Chat Completions".
[[nodiscard]] bool should_use_responses_api(std::string_view provider_name,
                                            std::string_view model) noexcept;

// ============================================================================
// reasoning_tags_for_config — S10 profile (Config convenience)
// ============================================================================
//
// reasoning_tags_for_provider(std::string_view) is declared in ThinkSplitter.hpp
// (the dependency-light header) and defined in ReasoningTagProfile.cpp.  The
// Config-level convenience below resolves the provider identity first, so the
// streaming delta path can build a ReasoningAccumulator straight from Config.

/// Convenience: resolve the provider identity for @p cfg (same rule as
/// OpenAiCompatibleProvider::name()) and return its reasoning-tag convention.
/// This is the seam the streaming delta path (Conversation) uses to construct a
/// correctly-configured ReasoningAccumulator from the live Config.
[[nodiscard]] ReasoningTags reasoning_tags_for_config(const batbox::config::Config& cfg);

} // namespace batbox::inference
