#!/usr/bin/env python3
"""
fixtures/mock_model_recorder.py — OpenAI-compatible SSE mock that records
the "model" field from every /v1/chat/completions request.

Used by smoke case 29 (29_model_n_persists.sh) to verify that after a
/model N switch, the next inference request uses the newly selected model
rather than the launch-time default.

Recorded model values are written to MODEL_LOG_FILE (one per line).
The smoke case reads this file after the chat round-trip completes.

Usage:
  python3 mock_model_recorder.py [--port 8847] [--log-file /tmp/batbox-qa-model-log.txt]

Requires only Python 3 stdlib.
PID written to /tmp/batbox-qa-mock-model-recorder.pid on startup.
"""

import argparse
import http.server
import json
import os
import time


PID_FILE = "/tmp/batbox-qa-mock-model-recorder.pid"
# Shared across handler instances via class variable (set in main()).
_LOG_FILE: str = "/tmp/batbox-qa-model-log.txt"


# ---------------------------------------------------------------------------
# SSE helpers
# ---------------------------------------------------------------------------

def _make_chunk(content: str, model: str = "mock-recorder") -> str:
    payload = {
        "id": "chatcmpl-rec",
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


def _make_finish_chunk(model: str = "mock-recorder",
                       prompt_tokens: int = 5,
                       completion_tokens: int = 2) -> str:
    """Terminal chunk with finish_reason='stop' + usage block.

    Required so Conversation::run_turn fires on_usage_delta_cb_ with a
    non-zero token total (otherwise the cb is gated on usage > 0 in
    Conversation.cpp).
    """
    payload = {
        "id": "chatcmpl-rec",
        "object": "chat.completion.chunk",
        "created": int(time.time()),
        "model": model,
        "choices": [
            {
                "index": 0,
                "delta": {},
                "finish_reason": "stop",
            }
        ],
        "usage": {
            "prompt_tokens": prompt_tokens,
            "completion_tokens": completion_tokens,
            "total_tokens": prompt_tokens + completion_tokens,
        },
    }
    return f"data: {json.dumps(payload)}\n\n"


def _make_done_chunk() -> str:
    return "data: [DONE]\n\n"


# ---------------------------------------------------------------------------
# Request handler
# ---------------------------------------------------------------------------

class RecorderHandler(http.server.BaseHTTPRequestHandler):
    """Handles POST /v1/chat/completions; records the request's model field."""

    log_file: str = _LOG_FILE  # set in main()

    def log_message(self, fmt, *args):  # silence default access log
        pass

    def do_GET(self):
        if self.path in ("/health", "/v1/models"):
            body = json.dumps({"status": "ok", "models": [{"id": "mock-recorder"}]}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_response(404)
            self.end_headers()

    def do_POST(self):
        if self.path != "/v1/chat/completions":
            self.send_response(404)
            self.end_headers()
            return

        length = int(self.headers.get("Content-Length", 0))
        body_raw = self.rfile.read(length) if length else b"{}"

        try:
            req = json.loads(body_raw)
        except json.JSONDecodeError:
            req = {}

        # Record the model field to the log file (append, one model per line).
        model_field = req.get("model", "")
        try:
            with open(RecorderHandler.log_file, "a", encoding="utf-8") as lf:
                lf.write(model_field + "\n")
                lf.flush()
        except OSError:
            pass  # non-fatal — test will time out naturally

        # Stream a trivial "Hi!" response so BatBox considers the turn complete.
        chunks = [_make_chunk("Hi"), _make_chunk("!"),
                  _make_finish_chunk(), _make_done_chunk()]

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
                time.sleep(0.05)
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Recording mock LLM server — captures 'model' field of every request"
    )
    parser.add_argument("--port", type=int, default=8847,
                        help="Port to listen on (default 8847)")
    parser.add_argument("--log-file", default=_LOG_FILE,
                        help="Path to write recorded model names (one per line)")
    args = parser.parse_args()

    RecorderHandler.log_file = args.log_file

    # Truncate the log file so the test starts with a clean slate.
    try:
        open(args.log_file, "w").close()
    except OSError:
        pass

    with open(PID_FILE, "w") as fh:
        fh.write(str(os.getpid()))

    server_address = ("127.0.0.1", args.port)
    httpd = http.server.HTTPServer(server_address, RecorderHandler)

    print(
        f"mock-model-recorder: listening on http://127.0.0.1:{args.port}/v1"
        f"  log={args.log_file}  (pid={os.getpid()})",
        flush=True,
    )
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        if os.path.exists(PID_FILE):
            os.unlink(PID_FILE)
        httpd.server_close()
        print("mock-model-recorder: stopped", flush=True)


if __name__ == "__main__":
    main()
