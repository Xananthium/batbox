#!/usr/bin/env python3
"""
tests/fixtures/fake_mcp_sse.py
-------------------------------
Minimal MCP-over-SSE server for integration testing SseTransport.

MCP SSE protocol:
  GET /sse      - Opens a persistent SSE stream.  The server immediately
                  emits an "endpoint" event with the POST URL, then keeps
                  the connection open.  When a response is ready it emits
                  a "message" event with the JSON-RPC response payload.
  POST /messages - Receives a JSON-RPC request or notification.  For
                  requests it synthesises a JSON-RPC response and pushes
                  it to the SSE stream (correlated by id).  Returns 202.

Uses ThreadingHTTPServer so GET /sse (blocking) and POST /messages can
be handled concurrently by different threads.

Usage (standalone):
    python3 fake_mcp_sse.py [--port PORT]

Usage from C++ doctest via subprocess:
    The test binary spawns this script, reads the port from its stdout,
    exercises SseTransport, then sends SIGTERM.

Port selection:
    Binds to 127.0.0.1 on a random ephemeral port (0) by default, or the
    value passed with --port.  Prints "READY <port>" to stdout so the test
    harness knows which port to use.
"""

import argparse
import json
import queue
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


# ---------------------------------------------------------------------------
# Per-connection SSE response queues
# ---------------------------------------------------------------------------

# Each GET /sse connection gets its own Queue.  POST /messages pushes to all
# live queues.  On disconnect the queue is removed.
_sse_queues_lock = threading.Lock()
_sse_queues: list[queue.Queue] = []


def _broadcast_response(response_json: dict):
    """Push a JSON-RPC response to all live SSE streams."""
    payload = json.dumps(response_json)
    with _sse_queues_lock:
        for q in list(_sse_queues):
            q.put(payload)


# ---------------------------------------------------------------------------
# Request handler
# ---------------------------------------------------------------------------

class FakeMcpSseHandler(BaseHTTPRequestHandler):

    def log_message(self, fmt, *args):
        # Suppress per-request console noise.
        pass

    def _read_body(self) -> str:
        length = int(self.headers.get("Content-Length", 0))
        return self.rfile.read(length).decode("utf-8") if length > 0 else ""

    def _send_sse_preamble(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.end_headers()

    def _write_sse_event(self, event: str, data: str):
        line = f"event: {event}\ndata: {data}\n\n"
        self.wfile.write(line.encode("utf-8"))
        self.wfile.flush()

    def do_GET(self):
        if self.path.rstrip("/") != "/sse":
            self.send_response(404)
            self.end_headers()
            return

        self._send_sse_preamble()

        # Register a per-connection response queue.
        my_queue: queue.Queue = queue.Queue()
        with _sse_queues_lock:
            _sse_queues.append(my_queue)

        # Derive the POST endpoint path.
        post_path = "/messages"

        try:
            # Emit the endpoint event — this unblocks SseTransport::start().
            self._write_sse_event("endpoint", post_path)

            # Keep the connection open; forward queued responses as SSE events.
            while True:
                try:
                    payload = my_queue.get(timeout=0.1)
                    self._write_sse_event("message", payload)
                except queue.Empty:
                    # Send a comment (keep-alive ping) to detect broken connections.
                    try:
                        self.wfile.write(b": ping\n\n")
                        self.wfile.flush()
                    except (BrokenPipeError, ConnectionResetError, OSError):
                        break

        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        finally:
            with _sse_queues_lock:
                try:
                    _sse_queues.remove(my_queue)
                except ValueError:
                    pass

    def do_POST(self):
        if self.path.rstrip("/") != "/messages":
            self.send_response(404)
            self.end_headers()
            return

        body = self._read_body()

        try:
            msg = json.loads(body)
        except json.JSONDecodeError:
            self.send_response(400)
            self.end_headers()
            return

        # Return 202 immediately — MCP SSE pattern.
        self.send_response(202)
        self.send_header("Content-Length", "0")
        self.end_headers()

        # If this is a request (has "id"), synthesise a response and broadcast.
        if "id" in msg and "method" in msg:
            method = msg.get("method", "")
            req_id = msg["id"]
            params = msg.get("params") or {}

            # Build a simple response payload based on the method.
            if method == "initialize":
                result = {
                    "protocolVersion": "2024-11-05",
                    "capabilities": {"tools": {}},
                    "serverInfo": {"name": "fake-mcp-sse", "version": "0.1.0"},
                }
            elif method == "ping":
                result = {}
            elif method == "tools/list":
                result = {"tools": [
                    {
                        "name": "sse_tool",
                        "description": "Echoes the message argument (SSE transport).",
                        "inputSchema": {"type": "object", "properties": {"message": {"type": "string"}}, "required": ["message"]},
                    }
                ]}
            elif method == "tools/call":
                tool_name = params.get("name", "")
                arguments = params.get("arguments", {})
                if tool_name == "sse_tool":
                    message = arguments.get("message", "")
                    result = {"content": [{"type": "text", "text": message}], "isError": False}
                else:
                    result = {"content": [{"type": "text", "text": f"unknown tool: {tool_name}"}], "isError": True}
            elif method == "echo":
                result = {"echo": params}
            else:
                # Generic OK response for any unrecognised method.
                result = {"method": method, "params": params}

            response = {
                "jsonrpc": "2.0",
                "id": req_id,
                "result": result,
            }

            # Push the response back over the SSE stream synchronously
            # (we are already on a separate thread per ThreadingHTTPServer).
            _broadcast_response(response)

        # Notifications (no "id") require no response.


# ---------------------------------------------------------------------------
# Server factory
# ---------------------------------------------------------------------------

def make_server(port: int = 0) -> ThreadingHTTPServer:
    server = ThreadingHTTPServer(("127.0.0.1", port), FakeMcpSseHandler)
    return server


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Fake MCP SSE server for batbox tests")
    parser.add_argument("--port", type=int, default=0,
                        help="Port to bind (0 = random ephemeral)")
    args = parser.parse_args()

    server = make_server(args.port)
    actual_port = server.server_address[1]

    # Signal to the test harness that we are ready.
    print(f"READY {actual_port}", flush=True)

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    server.server_close()


if __name__ == "__main__":
    main()
