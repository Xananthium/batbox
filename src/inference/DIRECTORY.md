# src/inference

OpenAI-compatible inference client implementations: HTTP client, SSE parser, tool call accumulator, pricing table, and usage accounting.

## Files

### Client.cpp
`Client::chat()` implementation: serialises ChatRequest to JSON, POSTs to BATBOX_API_BASE_URL/v1/chat/completions via cpr, parses ChatResponse from response body. `stream_chat()` implementation: POSTs with stream=true, feeds response chunks to SseParser, calls on_delta for each parsed StreamDelta, returns aggregated UsageDelta.

### ChatRequest.cpp
`ChatRequest` serialisation to JSON wire format; `tool_choice_auto/none/function()` static helpers; `WireMessage`, `WireToolCall`, `ToolDef` serialisation.

### SseParser.cpp
`SseParser::feed()` implementation: buffers raw bytes; splits on "\n\n" event boundaries; parses "data: " lines; returns `SseEvent::is_done=true` for "[DONE]" sentinels.

### ToolCallAccumulator.cpp
`accumulate()` implementation: index-based delta stitching per OpenAI streaming spec; appends partial name/arguments strings; `finalize()` parses arguments as JSON for each accumulated tool call.

### ModelPricing.cpp
Loads data/models.json at first call; builds name→pricing table; `cost()` looks up model and multiplies token counts by per-million rates. On a raw-id miss it retries the lookup under `map_to_canonical_model` (forward-declared from Provider.cpp) so provider-prefixed / tag-suffixed / mixed-case ids resolve to the same entry — fallback-only, so raw hits are never altered.

### UsageTracker.cpp
`add()`, `session_total()`, `turn_total()`, `reset_turn()`, `reset_all()` implementations; wraps two UsageDelta structs guarded by a mutex.

### Provider.cpp
S8/S9 provider abstraction. `Provider` pure-virtual interface (chat / stream_chat + name/metadata + `manages_own_context`); `OpenAiCompatibleProvider` (final) wraps and delegates to `Client` for every OpenAI-compatible endpoint, with a constructor flag for the S9 context-ownership opt-out; `ProviderRegistry::create()` factory builds the right provider from a `Config`; `should_use_responses_api()` Chat-vs-Responses routing seam (currently always false — batbox speaks only Chat Completions). The `map_to_canonical_model()` normaliser declared in Provider.hpp lives in its own leaf TU — see CanonicalModel.cpp.

### CanonicalModel.cpp
Defines `map_to_canonical_model()` (declared in Provider.hpp): the deterministic, idempotent model-id normaliser (trim → keep final `/`-segment → strip trailing `:tag` → ASCII-lowercase). Split out of Provider.cpp into its own leaf TU — it is a pure string function with no Client/Config/cpr dependency, so light consumers (ModelPricing.cpp wires it as a canonical-fallback for pricing lookups) link the symbol without dragging in the Provider/Client/HTTP chain.

### CMakeLists.txt
Build rules for the inference static library.
