#!/usr/bin/env python3
# tests/fixtures/fake_mcp_http.py
# ---------------------------------------------------------------------------
# Minimal fake MCP HTTP server for integration testing HttpTransport.
#
# Listens on 127.0.0.1 on an ephemeral port.
# Prints "READY <port>" to stdout so the C++ test can discover the port.
#
# Endpoints:
#
#   POST /          — standard MCP JSON-RPC endpoint
#                     • initialize         → returns capabilities
#                     • ping               → returns {}
#                     • tools/list         → returns []
#                     • auth/check         → echoes back Authorization header value
#                     • close/connection   → sets a flag that makes next healthy
#                                           check fail (for healthy() test)
#                     • unknown method     → returns JSON-RPC MethodNotFound error
#
#   GET  /          — health probe (HEAD also supported via BaseHTTPServer)
#   HEAD /          — health probe
#
# Streamable-http variant:
#   If the request body contains "streamable": true (in the test params),
#   the server responds with Content-Type: text/event-stream and wraps
#   the JSON-RPC response in an SSE envelope.
#
# Authorization:
#   If the server is started with --require-auth, it rejects requests that
#   lack Authorization: Bearer test-token with 401.
#
# Usage (direct):
#   python3 fake_mcp_http.py [--require-auth] [--streamable-default]
#
# The test binary injects these flags via subprocess args.
# ---------------------------------------------------------------------------

import argparse
import json
import sys
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import urlparse

# ---------------------------------------------------------------------------
# State shared across requests
# ---------------------------------------------------------------------------
_connection_closed = False
_require_auth = False
_streamable_default = False


def _json_rpc_response(req_id, result):
    return {"jsonrpc": "2.0", "id": req_id, "result": result}


def _json_rpc_error(req_id, code, message):
    return {"jsonrpc": "2.0", "id": req_id, "error": {"code": code, "message": message}}


def _sse_wrap(obj):
    """Wrap a JSON object as an SSE data event."""
    return f"data: {json.dumps(obj)}\n\n"


def _handle_rpc(body_bytes, auth_header):
    global _connection_closed

    try:
        req = json.loads(body_bytes)
    except Exception:
        return None, _json_rpc_error(None, -32700, "Parse error"), False

    req_id  = req.get("id")
    method  = req.get("method", "")
    params  = req.get("params") or {}

    # Authorization check
    if _require_auth:
        if auth_header != "Bearer test-token":
            return None, None, False  # signals 401 to caller

    # Detect streamable request
    use_sse = _streamable_default or (
        isinstance(params, dict) and params.get("streamable", False)
    )

    if method == "initialize":
        caps = {"protocolVersion": "2024-11-05", "capabilities": {"tools": {}}, "serverInfo": {"name": "fake-mcp-http", "version": "0.1.0"}}
        if use_sse:
            caps["capabilities"]["streamable-http"] = True
        return req_id, _json_rpc_response(req_id, caps), use_sse

    if method == "ping":
        return req_id, _json_rpc_response(req_id, {}), use_sse

    if method == "tools/list":
        return req_id, _json_rpc_response(req_id, {"tools": [
            {
                "name": "http_tool",
                "description": "Echoes the message argument (HTTP transport).",
                "inputSchema": {"type": "object", "properties": {"message": {"type": "string"}}, "required": ["message"]},
            }
        ]}), use_sse

    if method == "tools/call":
        params = params or {}
        tool_name = params.get("name", "")
        arguments = params.get("arguments", {})
        if tool_name == "http_tool":
            message = arguments.get("message", "")
            result = {"content": [{"type": "text", "text": message}], "isError": False}
        else:
            result = {"content": [{"type": "text", "text": f"unknown tool: {tool_name}"}], "isError": True}
        return req_id, _json_rpc_response(req_id, result), use_sse

    if method == "auth/check":
        return req_id, _json_rpc_response(req_id, {"auth": auth_header}), use_sse

    if method == "close/connection":
        _connection_closed = True
        return req_id, _json_rpc_response(req_id, {"closed": True}), use_sse

    # Notification: no id in request
    if req_id is None:
        return None, None, False

    # Unknown method
    return req_id, _json_rpc_error(req_id, -32601, f"Method not found: {method}"), use_sse


class McpHttpHandler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        # Suppress default noisy HTTP logs to stderr.
        pass

    def do_HEAD(self):
        global _connection_closed
        if _connection_closed:
            self.send_response(503)
            self.end_headers()
        else:
            self.send_response(200)
            self.end_headers()

    def do_GET(self):
        global _connection_closed
        if _connection_closed:
            self.send_response(503)
            self.send_header("Content-Length", "0")
            self.end_headers()
        else:
            self.send_response(200)
            self.send_header("Content-Length", "0")
            self.end_headers()

    def do_POST(self):
        global _connection_closed

        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length) if length > 0 else b""
        auth_header = self.headers.get("Authorization", "")

        req_id, resp_obj, use_sse = _handle_rpc(body, auth_header)

        # Auth failure
        if resp_obj is None and req_id is None and _require_auth and auth_header != "Bearer test-token":
            self.send_response(401)
            body_out = b'{"error":"Unauthorized"}'
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body_out)))
            self.end_headers()
            self.wfile.write(body_out)
            return

        # Notification (no response body)
        if resp_obj is None:
            self.send_response(202)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        if use_sse:
            sse_body = _sse_wrap(resp_obj).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Content-Length", str(len(sse_body)))
            self.end_headers()
            self.wfile.write(sse_body)
        else:
            body_out = json.dumps(resp_obj).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body_out)))
            self.end_headers()
            self.wfile.write(body_out)


def main():
    global _require_auth, _streamable_default

    parser = argparse.ArgumentParser(description="Fake MCP HTTP server for tests")
    parser.add_argument("--require-auth", action="store_true",
                        help="Require Authorization: Bearer test-token")
    parser.add_argument("--streamable-default", action="store_true",
                        help="Always respond with text/event-stream")
    args = parser.parse_args()

    _require_auth = args.require_auth
    _streamable_default = args.streamable_default

    server = HTTPServer(("127.0.0.1", 0), McpHttpHandler)
    port = server.server_address[1]

    # Signal readiness to the C++ test harness.
    print(f"READY {port}", flush=True)

    server.serve_forever()


if __name__ == "__main__":
    main()
