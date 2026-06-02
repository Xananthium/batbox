# include/batbox/inference

OpenAI-compatible inference client headers: request/response types, streaming SSE parser, tool call accumulator, usage tracking, and the provider abstraction (S8/S9) that wraps the client behind a polymorphic interface.

## Files

### ChatRequest.hpp
Wire format for the /v1/chat/completions request body.

- `ChatRequest::tool_choice_auto()` — static; returns the "auto" tool_choice value
- `ChatRequest::tool_choice_none()` — static; returns the "none" tool_choice value
- `ChatRequest::tool_choice_function(name)` — static; returns a forced-function tool_choice for a specific tool name

### ChatResponse.hpp
Wire format for the /v1/chat/completions response body (streaming and non-streaming).

- `StreamDelta` — carries partial content, partial tool_calls, finish_reason, and usage for one SSE event
- `ChatResponse` — full non-streaming response: id, model, content, tool_calls, finish_reason, usage

### Client.hpp
HTTP client wrapping cpr for the inference endpoint.

- `Client::Client(cfg)` — constructs from ApiConfig (base_url, api_key, timeout); wires cpr session headers
- `Client::chat(req) -> Result<ChatResponse>` — POSTs to /v1/chat/completions with req serialised as JSON; returns parsed ChatResponse or HTTP/parse error
- `Client::stream_chat(req, on_delta, ct) -> Result<UsageDelta>` — POSTs with stream=true; calls on_delta(StreamDelta) for each SSE event; returns aggregated usage on stream end; cooperative cancellation via ct

### ModelPricing.hpp
Static model cost lookup table.

- `ModelPricing::cost(model, prompt_tokens, completion_tokens) -> double` — looks up model in the pricing table (loaded from data/models.json); on a raw miss, retries under `map_to_canonical_model` so prefixed/tagged/mixed-case ids resolve; returns USD cost; returns 0.0 for unknown models
- `ModelPricing::reset_for_testing()` — clears the cached pricing table; used in unit tests to force reload

### Provider.hpp
S8/S9 provider abstraction — a polymorphic seam *around* the existing `Client` (composition, not rewrite; all HTTP/retry/quirk handling stays in `Client`).

- `ProviderMetadata` — static provider identity: canonical `name`, `base_url`, human-readable `description`
- `Provider` — pure-virtual interface; method surface mirrors `Client` so call-sites migrate without signature churn
  - `Provider::chat(req) -> Result<ChatResponse>` — non-streaming completion (pure virtual)
  - `Provider::stream_chat(req, on_delta, ct) -> Result<UsageDelta>` — SSE streaming completion (pure virtual)
  - `Provider::name() / metadata()` — provider identity accessors (pure virtual)
  - `Provider::manages_own_context() -> bool` — S9 hook; default `false` (batbox owns the window and runs compaction); a backend that owns its own window overrides to `true` so batbox compaction stands down
- `OpenAiCompatibleProvider` — the one concrete `Provider` (final) for every OpenAI-compatible endpoint; owns a `Client` and delegates all HTTP to it; ctor flag `manages_own_context` exposes the S9 opt-out
- `ProviderRegistry::create(cfg, manages_own_context=false) -> unique_ptr<Provider>` — factory; resolves the provider from `Config` (today always an `OpenAiCompatibleProvider`); the single seam where a future non-compatible provider branches in
- `map_to_canonical_model(raw) -> std::string` — deterministic, idempotent model-id normaliser (trim → keep last `/`-segment → strip trailing `:tag` → lowercase); used for pricing lookup / display / capability gating
- `should_use_responses_api(provider_name, model) -> bool` — Chat-Completions vs Responses-API routing seam; always `false` today (batbox speaks only Chat Completions)

### SseParser.hpp
Streaming SSE (Server-Sent Events) line parser.

- `SseParser::feed(bytes) -> Result<vector<SseEvent>>` — ingests raw bytes from the response body stream; returns all complete SSE events parsed from the buffer; buffers partial events across calls
- `SseParser::reset()` — clears the internal line buffer; call between requests
- `SseParser::buffered_bytes() -> size_t` — returns count of bytes currently in the incomplete-event buffer

### ToolCallAccumulator.hpp
Accumulates streaming tool_call deltas into complete ToolCall objects.

- `ToolCallAccumulator::accumulate(delta)` — merges one StreamDelta's tool_calls into the running accumulator; handles index-based delta stitching per OpenAI streaming spec
- `ToolCallAccumulator::finalize() -> Result<vector<ToolCall>>` — returns the complete ToolCall list after streaming ends; returns Err when any call has a JSON parse error in arguments

### UsageTracker.hpp
Per-session and per-turn token and cost accumulator.

- `UsageTracker::add(delta)` — adds a UsageDelta to both the session and turn totals
- `UsageTracker::session_total() -> UsageDelta` — returns cumulative usage for the entire session
- `UsageTracker::turn_total() -> UsageDelta` — returns usage for the current turn only
- `UsageTracker::reset_turn()` — zeroes the turn total; call at the start of each new turn
- `UsageTracker::reset_all()` — zeroes both session and turn totals; call on /clear
