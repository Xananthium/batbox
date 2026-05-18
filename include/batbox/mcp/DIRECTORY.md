# include/batbox/mcp

Model Context Protocol (MCP) client headers: JSON-RPC framing, transport interface, four transport implementations, server registry with health monitoring, and the high-level McpClient.

## Files

### JsonRpc.hpp
JSON-RPC 2.0 message builders and parser.

- `make_request(method, params) -> JsonRpcRequest` — constructs a JSON-RPC request with auto-incremented id
- `make_notification(method, params) -> JsonRpcNotification` — constructs a notification (no id, no response expected)
- `make_response(id, result) -> JsonRpcResponse` — constructs a success response
- `make_error_response(id, code, message) -> JsonRpcResponse` — constructs an error response using errc constants
- `parse_message(j) -> Result<AnyMessage, string>` — parses a JSON object into the correct message variant (request/notification/response/error)
- `next_id() -> int64_t` — returns the next monotonically-increasing request id; thread-safe

### IMcpTransport.hpp
Pure-virtual transport interface all MCP transports implement.

- `IMcpTransport::start(ct)` — starts the transport (spawns process, opens connection); cooperative cancellation via ct
- `IMcpTransport::stop()` — tears down the connection gracefully
- `IMcpTransport::healthy() -> bool` — returns true when the transport is connected and responsive
- `IMcpTransport::request(method, params, ct) -> Result<Json>` — sends a JSON-RPC request and blocks for the response
- `IMcpTransport::notify(method, params)` — sends a JSON-RPC notification (fire-and-forget)
- `IMcpTransport::on_notification(handler)` — registers a callback invoked for inbound server-pushed notifications

### HttpTransport.hpp
HTTP/HTTPS MCP transport using cpr for request/response over plain HTTP.

- `HttpTransport` — implements IMcpTransport; posts JSON-RPC requests to the configured HTTP endpoint
- `HttpTransport::set_streamable_http(enabled)` — enables or disables streamable HTTP mode (chunked responses)
- `HttpTransport::streamable_http() -> bool` — returns whether streamable HTTP is enabled

### SseTransport.hpp
Server-Sent Events MCP transport: GET stream for server→client, POST for client→server.

- `SseTransport` — implements IMcpTransport; opens a persistent SSE GET connection; dispatches inbound notifications via the on_notification callback; sends requests via POST to the endpoint URL

### StdioTransport.hpp
stdio MCP transport: spawns a child process and communicates over stdin/stdout with LSP Content-Length framing.

- `StdioTransport` — implements IMcpTransport; uses posix_spawn to launch the configured command; FrameReader/FrameWriter handle Content-Length framing; reads from child stdout in a background thread

### WsTransport.hpp
WebSocket MCP transport using IXWebSocket.

- `WsTransport` — implements IMcpTransport; connects to the configured ws:// or wss:// URL; dispatches inbound messages via the on_notification callback; sends requests as JSON-RPC text frames

### McpFraming.hpp
LSP Content-Length framing for stdio MCP transport.

- `FrameWriter::encode(json_payload) -> string` — prepends "Content-Length: N\r\n\r\n" header to json_payload; returns the complete framed message ready to write to stdin
- `FrameReader::feed(chunk) -> vector<string>` — ingests raw bytes; returns any complete JSON-RPC messages parsed from the Content-Length frames; buffers incomplete frames across calls
- `FrameReader::errors() -> vector<string>` — returns framing errors encountered since last clear
- `FrameReader::clear_errors()` — clears the error list
- `FrameReader::reset()` — resets the internal buffer and frame parser state
- `FrameReader::pending_bytes() -> size_t` — returns byte count in the incomplete-frame buffer
- `frame_message(json_string) -> string` — free function; equivalent to FrameWriter::encode; convenience for one-off framing

### McpServerRegistry.hpp
Registry and health monitor for all configured MCP servers.

- `McpServerRegistry::load_from_config(servers)` — constructs transport objects from McpServerConfig entries and registers them
- `McpServerRegistry::add_transport(name, transport)` — registers a pre-constructed transport under a name
- `McpServerRegistry::start_all(ct)` — calls start() on all registered transports concurrently; logs per-server startup results
- `McpServerRegistry::restart(name, ct) -> Result<void>` — stops and restarts the named server's transport
- `McpServerRegistry::stop_all()` — calls stop() on all transports; waits for all to complete
- `McpServerRegistry::on_status_change(callback)` — registers a callback invoked with (server_name, HealthEvent) on every health state transition
- `McpServerRegistry::start_health_monitor()` — starts a background jthread that polls transport healthy() at 1 Hz
- `McpServerRegistry::stop_health_monitor()` — requests and joins the health monitor thread
- `McpServerRegistry::get(name) -> IMcpTransport*` — returns pointer to named transport; nullptr if not found
- `McpServerRegistry::server_names() -> vector<string>` — lists all registered server names
- `McpServerRegistry::size() -> size_t` — returns count of registered servers
- `McpServerRegistry::count_failed_servers() -> size_t` — returns count of servers with Failed or Unknown health state

### McpClient.hpp
High-level MCP JSON-RPC method client built on top of the registry.

- `McpClient::McpClient(registry)` — constructs referencing a live McpServerRegistry
- `McpClient::initialize_all(ct)` — sends the MCP "initialize" request to all servers; populates capabilities
- `McpClient::initialize_one(server, ct)` — initializes a single named server
- `McpClient::tools_list(server, ct, force_refresh) -> Result<Json>` — calls tools/list on server; caches the result; force_refresh bypasses cache
- `McpClient::tools_call(server, tool_name, args, ct) -> Result<Json>` — calls tools/call with tool_name and args on server
- `McpClient::resources_list(server, ct) -> Result<Json>` — calls resources/list on server
- `McpClient::resources_read(server, uri, ct) -> Result<Json>` — calls resources/read for the given URI
- `McpClient::prompts_list(server, ct) -> Result<Json>` — calls prompts/list on server
- `McpClient::prompts_get(server, prompt_name, prompt_args, ct) -> Result<Json>` — calls prompts/get with name and arguments
- `McpClient::capabilities(server) -> optional<ServerCapabilities>` — returns cached capabilities for server; nullopt before initialization
- `McpClient::initialized_servers() -> vector<string>` — lists server names that completed initialization successfully
