#!/usr/bin/env python3
"""
tests/fixtures/fake_openai_server.py
-------------------------------------
OpenAI-compatible HTTP server for integration testing
Client::chat() (non-streaming) and Client::stream_chat() (SSE streaming).

Usage (standalone):
    python3 fake_openai_server.py [--port PORT]

Usage from C++ doctest via subprocess:
    The test binary spawns this script, reads the port from its stdout,
    exercises Client::chat() / Client::stream_chat(), then sends SIGTERM.

Routes implemented:
    POST /v1/chat/completions             — non-streaming or streaming (stream=true).
                                           When streaming + has tools + no tool-role
                                           messages: emits tool_calls stream.
                                           When streaming + has tool-role messages:
                                           emits normal stop stream (loop second leg).
    POST /v1/chat/completions/stream      — 100-chunk SSE stream then [DONE].
    POST /v1/chat/completions/stream-cancel — slow 50ms/chunk SSE (for cancel test).
    POST /v1/chat/completions/stream-429  — 429 on first call; streams on 2nd+.
    POST /v1/chat/completions/stream-500-after-start — 2 SSE chunks then 500 body.
    POST /v1/chat/completions/stream-tool-calls — SSE stream emitting tool_calls deltas.
    GET  /v1/models                       — list available models.
    POST /v1/embeddings                   — fixed-vector embedding stub.
    POST /v1/chat/completions/error       — always returns 500.
    POST /v1/chat/completions/autherr     — always returns 401.

Authentication:
    Validates that the Authorization header equals "Bearer test-key-123".
    Returns 401 when the header is missing or wrong.
    Routes that end in /autherr bypass auth intentionally (to test auth-error paths).

Port selection:
    Binds to 127.0.0.1 on a random ephemeral port (0) by default, or the
    value passed with --port.  Prints "READY <port>" to stdout so the test
    harness knows which port to use.
"""

import argparse
import json
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer


EXPECTED_BEARER = "Bearer test-key-123"

# ---------------------------------------------------------------------------
# Hard-coded non-streaming response bodies (CPP 4.5)
# ---------------------------------------------------------------------------

CHAT_RESPONSE_OK = {
    "id": "chatcmpl-test-abc123",
    "object": "chat.completion",
    "created": 1716000000,
    "model": "gpt-4o",
    "choices": [
        {
            "index": 0,
            "message": {
                "role": "assistant",
                "content": "Hello from fake server!"
            },
            "finish_reason": "stop"
        }
    ],
    "usage": {
        "prompt_tokens": 10,
        "completion_tokens": 6,
        "total_tokens": 16
    }
}

CHAT_RESPONSE_TOOL_CALLS = {
    "id": "chatcmpl-test-tools456",
    "object": "chat.completion",
    "created": 1716000001,
    "model": "gpt-4o",
    "choices": [
        {
            "index": 0,
            "message": {
                "role": "assistant",
                "content": None,
                "tool_calls": [
                    {
                        "id": "call_abc001",
                        "type": "function",
                        "function": {
                            "name": "get_weather",
                            "arguments": "{\"location\":\"London\"}"
                        }
                    }
                ]
            },
            "finish_reason": "tool_calls"
        }
    ],
    "usage": {
        "prompt_tokens": 25,
        "completion_tokens": 15,
        "total_tokens": 40
    }
}

ERROR_500_BODY = json.dumps({
    "error": {
        "message": "Internal server error (simulated)",
        "type": "server_error",
        "code": "internal_error"
    }
})

ERROR_401_BODY = json.dumps({
    "error": {
        "message": "Invalid API key",
        "type": "invalid_request_error",
        "code": "invalid_api_key"
    }
})

ERROR_429_BODY = json.dumps({
    "error": {
        "message": "Rate limit exceeded (simulated)",
        "type": "requests",
        "code": "rate_limit_exceeded"
    }
})

# ---------------------------------------------------------------------------
# /v1/models response (CPP 4.8)
# ---------------------------------------------------------------------------

MODELS_LIST_RESPONSE = {
    "object": "list",
    "data": [
        {
            "id": "gpt-4o",
            "object": "model",
            "created": 1715700000,
            "owned_by": "openai"
        },
        {
            "id": "gpt-3.5-turbo",
            "object": "model",
            "created": 1677610602,
            "owned_by": "openai"
        }
    ]
}

# ---------------------------------------------------------------------------
# /v1/embeddings fixed-vector response (CPP 4.8)
# ---------------------------------------------------------------------------

EMBEDDINGS_RESPONSE = {
    "object": "list",
    "data": [
        {
            "object": "embedding",
            "index": 0,
            "embedding": [0.1, 0.2, 0.3, 0.4, 0.5]
        }
    ],
    "model": "text-embedding-ada-002",
    "usage": {
        "prompt_tokens": 8,
        "total_tokens": 8
    }
}

# ---------------------------------------------------------------------------
# Streaming SSE helpers
# ---------------------------------------------------------------------------

def make_stream_chunk(seq: int, total: int) -> str:
    """One content-bearing SSE chunk (1..total)."""
    payload = {
        "id": "chatcmpl-stream-test",
        "object": "chat.completion.chunk",
        "created": 1716000002,
        "model": "gpt-4o",
        "choices": [
            {
                "index": 0,
                "delta": {"content": f"token {seq}"},
                "finish_reason": None
            }
        ]
    }
    return f"data: {json.dumps(payload)}\n\n"


def make_stream_final_chunk() -> str:
    """Terminal SSE chunk with finish_reason and usage."""
    payload = {
        "id": "chatcmpl-stream-test",
        "object": "chat.completion.chunk",
        "created": 1716000002,
        "model": "gpt-4o",
        "choices": [
            {
                "index": 0,
                "delta": {},
                "finish_reason": "stop"
            }
        ],
        "usage": {
            "prompt_tokens": 5,
            "completion_tokens": 100,
            "total_tokens": 105
        }
    }
    return f"data: {json.dumps(payload)}\n\n"


def make_done_sentinel() -> str:
    return "data: [DONE]\n\n"


def make_tool_call_stream_chunks() -> list:
    """
    Build the SSE event sequence for a deterministic streaming tool_calls response.

    The OpenAI streaming format for tool_calls emits:
      chunk 1: delta with role="assistant", tool_calls[0].index=0, id="call_tc001",
               function.name="get_weather", function.arguments="" (first fragment)
      chunk 2: delta with tool_calls[0].index=0, function.arguments='{"location"'
      chunk 3: delta with tool_calls[0].index=0, function.arguments=':"Paris"}'
      chunk 4: delta with finish_reason="tool_calls", usage
      [DONE]
    """
    base = {
        "id": "chatcmpl-stream-toolcalls",
        "object": "chat.completion.chunk",
        "created": 1716000010,
        "model": "gpt-4o",
    }

    chunk1 = dict(base)
    chunk1["choices"] = [
        {
            "index": 0,
            "delta": {
                "role": "assistant",
                "content": None,
                "tool_calls": [
                    {
                        "index": 0,
                        "id": "call_tc001",
                        "type": "function",
                        "function": {
                            "name": "get_weather",
                            "arguments": ""
                        }
                    }
                ]
            },
            "finish_reason": None
        }
    ]

    chunk2 = dict(base)
    chunk2["choices"] = [
        {
            "index": 0,
            "delta": {
                "tool_calls": [
                    {
                        "index": 0,
                        "function": {
                            "arguments": '{"location"'
                        }
                    }
                ]
            },
            "finish_reason": None
        }
    ]

    chunk3 = dict(base)
    chunk3["choices"] = [
        {
            "index": 0,
            "delta": {
                "tool_calls": [
                    {
                        "index": 0,
                        "function": {
                            "arguments": ':"Paris"}'
                        }
                    }
                ]
            },
            "finish_reason": None
        }
    ]

    chunk4 = dict(base)
    chunk4["choices"] = [
        {
            "index": 0,
            "delta": {},
            "finish_reason": "tool_calls"
        }
    ]
    chunk4["usage"] = {
        "prompt_tokens": 20,
        "completion_tokens": 10,
        "total_tokens": 30
    }

    return [chunk1, chunk2, chunk3, chunk4]


def make_two_tool_call_stream_chunks() -> list:
    """
    Build the SSE event sequence for TWO parallel tool calls in one assistant turn.

    Emits:
      chunk 1: first tool call id="call_p001", name="tool_alpha", args=""
      chunk 2: first tool call args='{"x":1}'
      chunk 3: second tool call id="call_p002", name="tool_beta", args=""
      chunk 4: second tool call args='{"y":2}'
      chunk 5: finish_reason="tool_calls" + usage
      [DONE]
    """
    base = {
        "id": "chatcmpl-stream-twotoolcalls",
        "object": "chat.completion.chunk",
        "created": 1716000020,
        "model": "gpt-4o",
    }

    chunk1 = dict(base)
    chunk1["choices"] = [
        {
            "index": 0,
            "delta": {
                "role": "assistant",
                "content": None,
                "tool_calls": [
                    {
                        "index": 0,
                        "id": "call_p001",
                        "type": "function",
                        "function": {"name": "tool_alpha", "arguments": ""}
                    }
                ]
            },
            "finish_reason": None
        }
    ]

    chunk2 = dict(base)
    chunk2["choices"] = [
        {
            "index": 0,
            "delta": {
                "tool_calls": [{"index": 0, "function": {"arguments": '{"x":1}'}}]
            },
            "finish_reason": None
        }
    ]

    chunk3 = dict(base)
    chunk3["choices"] = [
        {
            "index": 0,
            "delta": {
                "tool_calls": [
                    {
                        "index": 1,
                        "id": "call_p002",
                        "type": "function",
                        "function": {"name": "tool_beta", "arguments": ""}
                    }
                ]
            },
            "finish_reason": None
        }
    ]

    chunk4 = dict(base)
    chunk4["choices"] = [
        {
            "index": 0,
            "delta": {
                "tool_calls": [{"index": 1, "function": {"arguments": '{"y":2}'}}]
            },
            "finish_reason": None
        }
    ]

    chunk5 = dict(base)
    chunk5["choices"] = [
        {"index": 0, "delta": {}, "finish_reason": "tool_calls"}
    ]
    chunk5["usage"] = {
        "prompt_tokens": 25,
        "completion_tokens": 15,
        "total_tokens": 40
    }

    return [chunk1, chunk2, chunk3, chunk4, chunk5]


def _request_has_tool_role_messages(req_body: dict) -> bool:
    """Return True when any message in the request has role == 'tool'."""
    for msg in req_body.get("messages", []):
        if msg.get("role") == "tool":
            return True
    return False


# ---------------------------------------------------------------------------
# Thread-safe retry counter for stream-429 path
# ---------------------------------------------------------------------------
_stream_429_calls = {}
_stream_429_lock = threading.Lock()


# ---------------------------------------------------------------------------
# Request handler
# ---------------------------------------------------------------------------

class FakeOpenAIHandler(BaseHTTPRequestHandler):

    def log_message(self, fmt, *args):
        # Suppress per-request console noise; errors still reach stderr.
        pass

    def _read_body(self):
        length = int(self.headers.get("Content-Length", 0))
        return self.rfile.read(length).decode("utf-8") if length > 0 else ""

    def _send_json(self, status: int, body):
        payload = json.dumps(body).encode() if isinstance(body, dict) else body.encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def _check_auth(self) -> bool:
        auth = self.headers.get("Authorization", "")
        if auth != EXPECTED_BEARER:
            self._send_json(401, json.loads(ERROR_401_BODY))
            return False
        return True

    def _send_sse_stream(self, n_chunks: int, delay_sec: float = 0.0):
        """
        Send n_chunks content SSE events followed by a usage/finish event
        and the [DONE] terminator.  delay_sec is sleep between chunks.
        """
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("X-Accel-Buffering", "no")
        self.end_headers()

        try:
            for i in range(1, n_chunks + 1):
                chunk = make_stream_chunk(i, n_chunks).encode()
                self.wfile.write(chunk)
                self.wfile.flush()
                if delay_sec > 0:
                    time.sleep(delay_sec)

            # Final chunk with finish_reason + usage
            final = make_stream_final_chunk().encode()
            self.wfile.write(final)
            self.wfile.flush()

            # [DONE] sentinel
            done = make_done_sentinel().encode()
            self.wfile.write(done)
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            # Client disconnected (e.g. cancellation test) — not an error.
            pass

    def _send_tool_calls_sse_stream(self):
        """
        Send a deterministic streaming tool_calls response (single call).
        Emits 4 chunks covering id + name, two argument fragments, and the
        finish_reason="tool_calls" terminal chunk, then [DONE].
        """
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("X-Accel-Buffering", "no")
        self.end_headers()

        try:
            for chunk_payload in make_tool_call_stream_chunks():
                line = f"data: {json.dumps(chunk_payload)}\n\n".encode()
                self.wfile.write(line)
                self.wfile.flush()

            done = make_done_sentinel().encode()
            self.wfile.write(done)
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass

    def _send_two_tool_calls_sse_stream(self):
        """
        Send a streaming response with TWO parallel tool calls.
        Used for the AC2 (two parallel calls) test path.
        """
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("X-Accel-Buffering", "no")
        self.end_headers()

        try:
            for chunk_payload in make_two_tool_call_stream_chunks():
                line = f"data: {json.dumps(chunk_payload)}\n\n".encode()
                self.wfile.write(line)
                self.wfile.flush()

            done = make_done_sentinel().encode()
            self.wfile.write(done)
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass

    # -----------------------------------------------------------------------
    # GET handler — /v1/models
    # -----------------------------------------------------------------------

    def do_GET(self):
        path = self.path.rstrip("/")

        if path == "/v1/models":
            if not self._check_auth():
                return
            self._send_json(200, MODELS_LIST_RESPONSE)
        else:
            self._send_json(404, {"error": {"message": f"unknown path {self.path}"}})

    # -----------------------------------------------------------------------
    # POST handler
    # -----------------------------------------------------------------------

    def do_POST(self):
        path = self.path.rstrip("/")

        # ------------------------------------------------------------------
        # Non-streaming paths (CPP 4.5)
        # ------------------------------------------------------------------

        if path == "/v1/chat/completions":
            if not self._check_auth():
                return
            body = self._read_body()
            try:
                req = json.loads(body)
            except json.JSONDecodeError:
                self._send_json(400, {"error": {"message": "bad JSON"}})
                return

            # If stream=true, route to the appropriate streaming branch.
            if req.get("stream"):
                has_tools = bool(req.get("tools"))
                has_tool_results = _request_has_tool_role_messages(req)

                if has_tools and not has_tool_results:
                    # First leg of the tool-call loop: model should emit tool_calls.
                    self._send_tool_calls_sse_stream()
                else:
                    # No tools configured, or second leg (tool results present):
                    # emit a normal stop stream.
                    self._send_sse_stream(n_chunks=100)
                return

            # Route to tool-calls response when the request asks for a tool call.
            if req.get("tools"):
                self._send_json(200, CHAT_RESPONSE_TOOL_CALLS)
            else:
                self._send_json(200, CHAT_RESPONSE_OK)

        elif path == "/v1/chat/completions/error":
            if not self._check_auth():
                return
            self._read_body()
            self.send_response(500)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(ERROR_500_BODY.encode())))
            self.end_headers()
            self.wfile.write(ERROR_500_BODY.encode())

        elif path == "/v1/chat/completions/autherr":
            # Always refuse regardless of what the client sends.
            self._read_body()
            self._send_json(401, json.loads(ERROR_401_BODY))

        # ------------------------------------------------------------------
        # Streaming paths (CPP 4.6)
        # ------------------------------------------------------------------

        elif path == "/v1/chat/completions/stream":
            # Fast 100-chunk stream with no artificial delay.
            if not self._check_auth():
                return
            self._read_body()
            self._send_sse_stream(n_chunks=100, delay_sec=0.0)

        elif path == "/v1/chat/completions/stream-cancel":
            # Slow stream (50ms per chunk) so the cancel test has time to fire.
            if not self._check_auth():
                return
            self._read_body()
            self._send_sse_stream(n_chunks=100, delay_sec=0.05)

        elif path == "/v1/chat/completions/stream-429":
            # First call per client connection returns 429; subsequent calls stream.
            if not self._check_auth():
                return
            self._read_body()

            client_key = self.client_address[0]
            with _stream_429_lock:
                call_count = _stream_429_calls.get(client_key, 0) + 1
                _stream_429_calls[client_key] = call_count

            if call_count == 1:
                # First attempt → 429 with Retry-After header.
                payload = ERROR_429_BODY.encode()
                self.send_response(429)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.send_header("Retry-After", "1")
                self.end_headers()
                self.wfile.write(payload)
            else:
                # Subsequent attempts → successful stream.
                self._send_sse_stream(n_chunks=5, delay_sec=0.0)

        elif path == "/v1/chat/completions/stream-500-after-start":
            # Send 2 SSE content chunks, then close with a 500-style body.
            # In HTTP/1.1 with chunked transfer, the status line is sent first.
            # We send 200 OK with the SSE content-type, emit 2 real chunks,
            # then abandon the connection — the C++ side sees 200 status but
            # the curl write callback will return an error after the last chunk.
            if not self._check_auth():
                return
            self._read_body()

            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()

            try:
                # 2 real content chunks
                for i in range(1, 3):
                    chunk = make_stream_chunk(i, 100).encode()
                    self.wfile.write(chunk)
                    self.wfile.flush()
                    time.sleep(0.01)

                # Then send an SSE error event that the client can detect.
                error_event = (
                    'data: {"error":{"message":"server error mid-stream",'
                    '"type":"server_error","code":"internal_error"}}\n\n'
                ).encode()
                self.wfile.write(error_event)
                self.wfile.flush()
                # Close without sending [DONE]; the partial stream should surface as error.
            except (BrokenPipeError, ConnectionResetError):
                pass

        # ------------------------------------------------------------------
        # Streaming tool_calls path (CPP 4.8 + CPP 3.7)
        # ------------------------------------------------------------------

        elif path == "/v1/chat/completions/stream-tool-calls":
            if not self._check_auth():
                return
            self._read_body()
            self._send_tool_calls_sse_stream()

        elif path == "/v1/chat/completions/stream-two-tool-calls":
            # Two parallel tool calls in one assistant turn (AC2 test).
            if not self._check_auth():
                return
            self._read_body()
            self._send_two_tool_calls_sse_stream()

        # ------------------------------------------------------------------
        # Embeddings stub (CPP 4.8)
        # ------------------------------------------------------------------

        elif path == "/v1/embeddings":
            if not self._check_auth():
                return
            self._read_body()
            self._send_json(200, EMBEDDINGS_RESPONSE)

        else:
            self._send_json(404, {"error": {"message": f"unknown path {self.path}"}})


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def make_server(port: int = 0) -> HTTPServer:
    server = HTTPServer(("127.0.0.1", port), FakeOpenAIHandler)
    return server


def main():
    parser = argparse.ArgumentParser(description="Fake OpenAI server for batbox tests")
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
