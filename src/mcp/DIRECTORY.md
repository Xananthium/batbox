# src/mcp

MCP client and transport implementations: JSON-RPC framing, four transport backends, server registry with health monitoring, and the high-level McpClient.

## Files

### JsonRpc.cpp
`make_request/notification/response/error_response()` implementations; `parse_message()` discriminates by presence of id/method/result/error; `next_id()` uses atomic<int64_t>.

### McpFraming.cpp
`FrameWriter::encode()`: prepends "Content-Length: N\r\n\r\n"; `FrameReader::feed()`: buffers bytes; parses Content-Length header; extracts complete JSON frames; `frame_message()` free function.

### StdioTransport.cpp
`start()`: posix_spawn child process with redirected stdin/stdout/stderr pipes; launches FrameReader/FrameWriter pair; `request()`: writes framed JSON-RPC request, blocks for matching response id; background reader thread dispatches notifications.

### HttpTransport.cpp
`start()`: validates endpoint URL; `request()`: POSTs JSON-RPC body to the HTTP endpoint via cpr; parses response; `notify()`: POST with no response wait.

### SseTransport.cpp
`start()`: opens persistent SSE GET connection via cpr; dispatches data lines to FrameReader on background thread; `request()`: POSTs JSON-RPC body; correlates response by id.

### WsTransport.cpp
`start()`: connects IXWebSocket to ws:// or wss:// URL; dispatches inbound text frames; `request()`: sends JSON-RPC text frame, blocks on response id.

### McpServerRegistry.cpp
`load_from_config()` constructs transports from config entries; `start_all()` launches all in parallel; `start_health_monitor()` starts 1 Hz polling jthread; health state transitions fire on_status_change callbacks.

### McpClient.cpp
`initialize_all/one()` sends MCP "initialize" handshake; caches ServerCapabilities; `tools_list()` caches result; all high-level methods route through the appropriate IMcpTransport::request() call.

### CMakeLists.txt
Build rules for the mcp static library.
