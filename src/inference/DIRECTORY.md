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
Loads data/models.json at first call; builds name→pricing table; `cost()` looks up model and multiplies token counts by per-million rates.

### UsageTracker.cpp
`add()`, `session_total()`, `turn_total()`, `reset_turn()`, `reset_all()` implementations; wraps two UsageDelta structs guarded by a mutex.

### CMakeLists.txt
Build rules for the inference static library.
