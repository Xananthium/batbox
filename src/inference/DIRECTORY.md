# src/inference

OpenAI-compatible inference client implementations: HTTP client, SSE parser, tool call accumulator, pricing table, usage accounting, and the S10 reasoning-isolation organs (think/visible splitter, unified reasoning channel, provider tag-profile).

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
S8/S9 provider abstraction. `Provider` pure-virtual interface (chat / stream_chat + name/metadata + `manages_own_context`); `OpenAiCompatibleProvider` (final) wraps and delegates to `Client` for every OpenAI-compatible endpoint, with a constructor flag for the S9 context-ownership opt-out; `ProviderRegistry::create()` factory builds the right provider from a `Config`; `should_use_responses_api()` Chat-vs-Responses routing seam (currently always false — batbox speaks only Chat Completions). S10 adds `OpenAiCompatibleProvider::reasoning_tags()` and the `reasoning_tags_for_config()` convenience (both delegate to `reasoning_tags_for_provider`, defined in the ReasoningTagProfile.cpp leaf TU). The `map_to_canonical_model()` normaliser declared in Provider.hpp lives in its own leaf TU — see CanonicalModel.cpp.

### ThinkSplitter.cpp
S10 — implementation of `ThinkSplitter` (declared in ThinkSplitter.hpp): the incremental, byte-at-a-time state machine that separates streamed `content` into visible vs. reasoning text. `pending_` always holds a prefix of the marker currently scanned for (open while Visible, close while Reasoning); bytes that can no longer begin a match are emitted to the current sink, a completed marker is consumed and toggles state, and `finish()` flushes the bounded tail. Dependency-light leaf TU (std lib only), unit-testable in isolation.

### ReasoningAccumulator.cpp
S10 — implementation of `ReasoningAccumulator` (declared in ReasoningAccumulator.hpp): the unified isolated reasoning channel. `accumulate(delta)` routes `delta.reasoning_content` straight into the isolated reasoning buffer and `delta.content` through the owned `ThinkSplitter`, returning only the reasoning-free visible text; `finish()` drains the splitter tail. Sources are disjoint by construction → no double-count.

### ReasoningTagProfile.cpp
S10 — defines `reasoning_tags_for_provider()` (declared in ThinkSplitter.hpp): the per-provider inline reasoning-tag convention, a pure function of the canonical provider name (default `<think>`/`</think>`; `anthropic` → `<thinking>`; openai/groq/mistral/together → `none()`). Leaf TU depending only on ThinkSplitter.hpp — links without the Provider/Client/cpr chain, in the same spirit as CanonicalModel.cpp.

### CanonicalModel.cpp
Defines `map_to_canonical_model()` (declared in Provider.hpp): the deterministic, idempotent model-id normaliser (trim → keep final `/`-segment → strip trailing `:tag` → ASCII-lowercase). Split out of Provider.cpp into its own leaf TU — it is a pure string function with no Client/Config/cpr dependency, so light consumers (ModelPricing.cpp wires it as a canonical-fallback for pricing lookups) link the symbol without dragging in the Provider/Client/HTTP chain.

### CMakeLists.txt
Build rules for the inference static library.
