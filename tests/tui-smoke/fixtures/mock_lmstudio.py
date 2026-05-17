#!/usr/bin/env python3
"""
fixtures/mock_lmstudio.py — minimal OpenAI-compatible SSE mock server.

Listens on http://127.0.0.1:<PORT>/v1 and serves deterministic
chat-completion streams for offline BatBox testing.

Response selection:
  1. Hash the incoming request's messages list to a hex string.
  2. Look for fixtures/transcripts/<hash>.jsonl — replay that file line-by-line
     as SSE chunks.
  3. If no fixture file found, stream a fallback "hi" response.

PID is written to /tmp/batbox-qa-mock-llm.pid on startup.

Usage:
  python3 mock_lmstudio.py [--port 8824] [--fixtures-dir ./transcripts]

Requires only Python 3 stdlib (http.server, json, hashlib, threading, etc.).
"""

import argparse
import hashlib
import http.server
import json
import os
import sys
import time


PID_FILE = "/tmp/batbox-qa-mock-llm.pid"


# ---------------------------------------------------------------------------
# SSE helpers
# ---------------------------------------------------------------------------

def _make_chunk(content: str, model: str = "mock-lm") -> str:
    """Return a single SSE data line for an OpenAI streaming chunk."""
    payload = {
        "id": "chatcmpl-mock",
        "object": "chat.completion.chunk",
        "created": int(time.time()),
        "model": model,
        "choices": [
            {
                "index": 0,
                "delta": {"content": content},
                "finish_reason": None,
            }
        ],
    }
    return f"data: {json.dumps(payload)}\n\n"


def _make_done_chunk() -> str:
    """Return the final [DONE] SSE line."""
    return "data: [DONE]\n\n"


def _fallback_stream() -> list:
    """Default response when no fixture file matches: stream 'Hi!'."""
    words = ["Hi", "!"]
    chunks = []
    for word in words:
        chunks.append(_make_chunk(word))
    chunks.append(_make_done_chunk())
    return chunks


def _fixture_stream(fixture_path: str) -> list:
    """
    Read a .jsonl fixture file.
    Each line is either:
      - A raw SSE data line (starts with 'data: ')
      - A JSON object with a 'content' key (simplified format)
    Returns a list of SSE data strings ready to write.
    """
    chunks = []
    try:
        with open(fixture_path, "r", encoding="utf-8") as fh:
            for line in fh:
                line = line.rstrip("\n")
                if not line:
                    continue
                if line.startswith("data: "):
                    # Already in SSE format — pass through as-is
                    chunks.append(line + "\n\n")
                else:
                    # Try JSON object with content key
                    try:
                        obj = json.loads(line)
                        content = obj.get("content", "")
                        if content:
                            chunks.append(_make_chunk(content))
                    except json.JSONDecodeError:
                        # Unknown format — skip
                        pass
        chunks.append(_make_done_chunk())
    except OSError:
        chunks = _fallback_stream()
    return chunks


def _request_hash(messages: list) -> str:
    """Hash the messages list to a short hex string for fixture lookup."""
    canonical = json.dumps(messages, sort_keys=True, ensure_ascii=True)
    return hashlib.sha256(canonical.encode()).hexdigest()[:16]


# ---------------------------------------------------------------------------
# Request handler
# ---------------------------------------------------------------------------

class MockHandler(http.server.BaseHTTPRequestHandler):
    """Handles POST /v1/chat/completions with streaming SSE response."""

    fixtures_dir: str = "."  # set at startup

    def log_message(self, fmt, *args):  # silence default access log
        pass

    def do_GET(self):
        """Health check endpoint."""
        if self.path == "/health" or self.path == "/v1/models":
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

        try:
            req = json.loads(body_raw)
        except json.JSONDecodeError:
            req = {}

        messages = req.get("messages", [])
        req_hash = _request_hash(messages)
        fixture_path = os.path.join(self.fixtures_dir, f"{req_hash}.jsonl")

        if os.path.exists(fixture_path):
            chunks = _fixture_stream(fixture_path)
        else:
            chunks = _fallback_stream()

        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()

        try:
            for chunk in chunks:
                data = chunk.encode("utf-8")
                # Write HTTP chunked encoding frame
                self.wfile.write(f"{len(data):x}\r\n".encode())
                self.wfile.write(data)
                self.wfile.write(b"\r\n")
                self.wfile.flush()
                # Small delay to simulate real streaming (≈10 tokens/s)
                time.sleep(0.05)
            # Terminal chunked frame
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Mock LM Studio server for BatBox tests")
    parser.add_argument("--port", type=int, default=8824, help="Port to listen on (default 8824)")
    parser.add_argument(
        "--fixtures-dir",
        default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "transcripts"),
        help="Directory containing .jsonl fixture files",
    )
    args = parser.parse_args()

    MockHandler.fixtures_dir = args.fixtures_dir

    # Write PID file
    with open(PID_FILE, "w") as fh:
        fh.write(str(os.getpid()))

    server_address = ("127.0.0.1", args.port)
    httpd = http.server.HTTPServer(server_address, MockHandler)

    print(f"mock-llm: listening on http://127.0.0.1:{args.port}/v1  (pid={os.getpid()})", flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        if os.path.exists(PID_FILE):
            os.unlink(PID_FILE)
        httpd.server_close()
        print("mock-llm: stopped", flush=True)


if __name__ == "__main__":
    main()
