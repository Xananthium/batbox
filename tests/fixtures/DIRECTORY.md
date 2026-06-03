# tests/fixtures

Test fixture scripts: fake server implementations for integration tests that run without real network services.

## Files

### fake_openai_server.py
FastAPI stub that mimics the OpenAI /v1/chat/completions endpoint; configurable responses for both streaming (SSE) and non-streaming modes; records requests for assertion in tests.

### fake_mcp_stdio.py
Fake MCP server communicating over stdio with LSP Content-Length framing; responds to tools/list and tools/call with configurable tool definitions; used by StdioTransport integration tests.

### fake_mcp_sse.py
Fake MCP server over SSE transport: GET /sse for the event stream, POST /message for client requests; used by SseTransport integration tests.

### fake_mcp_http.py
Fake MCP server over HTTP transport: single POST endpoint accepting JSON-RPC requests; returns preconfigured responses; used by HttpTransport integration tests.

### fake_mcp_ws.py
Fake MCP server over WebSocket transport using websockets library; echoes JSON-RPC responses; used by WsTransport integration tests.

### fake_scrapling_server.py
FastAPI stub implementing /healthz, /fetch, /search, /select, and /shutdown; returns configurable responses; used by SidecarManager integration tests to avoid real Scrapling dependency.

### fake_distill_server.py
S4 (DIS-980). Fake LOCAL OpenAI-compatible endpoint for the SubagentDistiller. Non-streaming `POST /v1/chat/completions`, Bearer `test-key-123`, `READY <port>` handshake. `--mode` selects the report_gold contract behaviour: `gold` (default — returns a `report_gold` tool_call with a deterministic golden line), `notool` (content/stop, no tool_calls), `wrongtool` (a non-report_gold tool_call), `error` (HTTP 500). Kept separate from fake_openai_server.py so the existing get_weather tests stay untouched.
