#!/usr/bin/env python3
"""
fixtures/mock_tool_call.py — OpenAI-compatible SSE mock that returns a
tool_calls response for TUI-FLOW-T2 smoke tests.

Flow:
  1. First POST → returns a streaming response with a single tool call:
       tool: Read, argument: {"path": "manifest.json"}
     finish_reason: "tool_calls"
  2. Second POST (with tool result in messages) → returns a plain text reply:
       "Done. I read manifest.json."
     finish_reason: "stop"

Port:    8848  (reserved for TUI-FLOW-T2)
PID:     /tmp/batbox-qa-mock-tool-call.pid

Usage:
  python3 mock_tool_call.py [--port 8848]

Requires only Python 3 stdlib.
"""

import argparse
import http.server
import json
import os
import time

PID_FILE = "/tmp/batbox-qa-mock-tool-call.pid"

# ---------------------------------------------------------------------------
# SSE helpers
# ---------------------------------------------------------------------------

def _make_text_chunk(content: str) -> str:
    payload = {
        "id": "chatcmpl-tc1",
        "object": "chat.completion.chunk",
        "created": int(time.time()),
        "model": "mock-lm",
        "choices": [{"index": 0, "delta": {"content": content}, "finish_reason": None}],
    }
    return "data: " + json.dumps(payload) + "\n\n"


def _make_tool_call_chunk(index: int, call_id: str, name: str, args_frag: str,
                           finish: bool = False) -> str:
    """Produce a tool_calls delta SSE chunk."""
    delta = {
        "tool_calls": [
            {
                "index": index,
                "id": call_id,
                "type": "function",
                "function": {"name": name, "arguments": args_frag},
            }
        ]
    }
    payload = {
        "id": "chatcmpl-tc1",
        "object": "chat.completion.chunk",
        "created": int(time.time()),
        "model": "mock-lm",
        "choices": [
            {
                "index": 0,
                "delta": delta,
                "finish_reason": "tool_calls" if finish else None,
            }
        ],
    }
    return "data: " + json.dumps(payload) + "\n\n"


def _make_done() -> str:
    return "data: [DONE]\n\n"


# ---------------------------------------------------------------------------
# Tool-call sequence (first request)
# ---------------------------------------------------------------------------

def _tool_call_stream() -> list:
    """Return SSE chunks that deliver a single Read tool call."""
    chunks = []
    # Role header
    role_payload = {
        "id": "chatcmpl-tc1",
        "object": "chat.completion.chunk",
        "created": int(time.time()),
        "model": "mock-lm",
        "choices": [{"index": 0, "delta": {"role": "assistant"}, "finish_reason": None}],
    }
    chunks.append("data: " + json.dumps(role_payload) + "\n\n")

    # Tool call name chunk
    chunks.append(_make_tool_call_chunk(0, "call_abc1", "Read", ""))
    time.sleep(0.05)
    # Arguments (split across two chunks to exercise accumulator)
    chunks.append(_make_tool_call_chunk(0, "call_abc1", "", '{"path": "mani'))
    time.sleep(0.05)
    chunks.append(_make_tool_call_chunk(0, "call_abc1", "", 'fest.json"}', finish=True))
    time.sleep(0.05)
    chunks.append(_make_done())
    return chunks


# ---------------------------------------------------------------------------
# Plain text reply (second request — after tool result returned)
# ---------------------------------------------------------------------------

def _text_reply_stream() -> list:
    """Return SSE chunks for the follow-up text reply."""
    words = ["Done.", " I", " read", " manifest.json."]
    chunks = []
    for w in words:
        chunks.append(_make_text_chunk(w))
        time.sleep(0.05)
    # finish_reason: stop
    finish_payload = {
        "id": "chatcmpl-tc2",
        "object": "chat.completion.chunk",
        "created": int(time.time()),
        "model": "mock-lm",
        "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}],
    }
    chunks.append("data: " + json.dumps(finish_payload) + "\n\n")
    chunks.append(_make_done())
    return chunks


# ---------------------------------------------------------------------------
# Request handler
# ---------------------------------------------------------------------------

class MockHandler(http.server.BaseHTTPRequestHandler):
    request_count: int = 0  # class-level counter

    def log_message(self, fmt, *args):  # silence access log
        pass

    def do_GET(self):
        if self.path in ("/health", "/v1/models"):
            body = json.dumps({"status": "ok", "models": [{"id": "mock-lm"}]}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_response(404)
            self.end_headers()

    def do_POST(self):
        if self.path not in ("/v1/chat/completions",):
            self.send_response(404)
            self.end_headers()
            return

        length = int(self.headers.get("Content-Length", 0))
        body_raw = self.rfile.read(length) if length else b"{}"

        # Increment global counter under the class
        MockHandler.request_count += 1
        req_no = MockHandler.request_count

        # First request → tool call; subsequent → text reply.
        chunks = _tool_call_stream() if req_no == 1 else _text_reply_stream()

        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()

        try:
            for chunk in chunks:
                data = chunk.encode("utf-8")
                self.wfile.write(f"{len(data):x}\r\n".encode())
                self.wfile.write(data)
                self.wfile.write(b"\r\n")
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            try:
                self.wfile.write(b"0\r\n\r\n")
                self.wfile.flush()
            except Exception:
                pass


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Mock tool-call server for TUI-FLOW-T2")
    parser.add_argument("--port", type=int, default=8848)
    args = parser.parse_args()

    with open(PID_FILE, "w") as fh:
        fh.write(str(os.getpid()))

    server_address = ("127.0.0.1", args.port)
    httpd = http.server.HTTPServer(server_address, MockHandler)

    print(f"mock-tool-call: listening on http://127.0.0.1:{args.port}/v1 (pid={os.getpid()})",
          flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        if os.path.exists(PID_FILE):
            os.unlink(PID_FILE)
        httpd.server_close()
        print("mock-tool-call: stopped", flush=True)


if __name__ == "__main__":
    main()
