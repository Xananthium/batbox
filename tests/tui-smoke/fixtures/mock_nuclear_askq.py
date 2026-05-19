#!/usr/bin/env python3
"""
fixtures/mock_nuclear_askq.py — Stateful Python mock server for smoke case 31.

Scripted flow:
  Turn 1 (any user message):
    Model calls AskUserQuestion with a single question:
      header: "Config Choice"
      question: "Which configuration style?"
      multi_select: false
      labels: ["Minimal", "Standard", "Advanced"]
    finish_reason: tool_calls

  Turn 2 (client returns AskUserQuestion tool result):
    Model emits a short acknowledgement message: "done."
    finish_reason: stop

PEXT3 1.6 assertion gate:
  BatBox is launched with --nuclear.  make_askq_prompt_fn(true, ...) installs a
  zero-capture closure that returns {} without posting any QuestionShow event.
  The smoke case asserts that no QuestionCard tokens appear on screen.

Port:    8852 (reserved for case 31)
PID:     /tmp/batbox-qa-mock-nuclear-askq.pid

Usage:
  python3 mock_nuclear_askq.py [--port 8852]

Requires only Python 3 stdlib.
"""

import argparse
import http.server
import json
import os
import time

PID_FILE = "/tmp/batbox-qa-mock-nuclear-askq.pid"

# ---------------------------------------------------------------------------
# SSE chunk builders (same helpers as mock_plan_with_cards.py)
# ---------------------------------------------------------------------------

def _make_role_chunk(msg_id: str) -> str:
    payload = {
        "id": msg_id,
        "object": "chat.completion.chunk",
        "created": int(time.time()),
        "model": "mock-lm",
        "choices": [{"index": 0, "delta": {"role": "assistant"}, "finish_reason": None}],
    }
    return "data: " + json.dumps(payload) + "\n\n"


def _make_text_chunk(content: str, msg_id: str) -> str:
    payload = {
        "id": msg_id,
        "object": "chat.completion.chunk",
        "created": int(time.time()),
        "model": "mock-lm",
        "choices": [{"index": 0, "delta": {"content": content}, "finish_reason": None}],
    }
    return "data: " + json.dumps(payload) + "\n\n"


def _make_tool_call_chunk(call_id: str, name: str, args_frag: str,
                           finish: bool = False, msg_id: str = "chatcmpl-naq1") -> str:
    delta = {
        "tool_calls": [
            {
                "index": 0,
                "id": call_id,
                "type": "function",
                "function": {"name": name, "arguments": args_frag},
            }
        ]
    }
    payload = {
        "id": msg_id,
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


def _make_stop_chunk(msg_id: str) -> str:
    payload = {
        "id": msg_id,
        "object": "chat.completion.chunk",
        "created": int(time.time()),
        "model": "mock-lm",
        "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}],
    }
    return "data: " + json.dumps(payload) + "\n\n"


def _make_done() -> str:
    return "data: [DONE]\n\n"


# ---------------------------------------------------------------------------
# Turn 1: AskUserQuestion — "Which configuration style?"
# ---------------------------------------------------------------------------

def _turn1_stream() -> list:
    chunks = []
    msg_id = "chatcmpl-naq1"
    chunks.append(_make_role_chunk(msg_id))

    args = json.dumps({
        "questions": [
            {
                "header": "Config Choice",
                "question": "Which configuration style?",
                "options": [
                    {"label": "Minimal"},
                    {"label": "Standard"},
                    {"label": "Advanced"},
                ]
            }
        ]
    })

    # Emit name chunk first (empty args), then args in two parts
    chunks.append(_make_tool_call_chunk("call_naq01", "AskUserQuestion", "", msg_id=msg_id))
    time.sleep(0.04)
    mid = len(args) // 2
    chunks.append(_make_tool_call_chunk("call_naq01", "", args[:mid], msg_id=msg_id))
    time.sleep(0.04)
    chunks.append(_make_tool_call_chunk("call_naq01", "", args[mid:], finish=True, msg_id=msg_id))
    time.sleep(0.04)
    chunks.append(_make_done())
    return chunks


# ---------------------------------------------------------------------------
# Turn 2: acknowledgement after tool result returns
# ---------------------------------------------------------------------------

def _turn2_stream() -> list:
    chunks = []
    msg_id = "chatcmpl-naq2"
    chunks.append(_make_role_chunk(msg_id))
    chunks.append(_make_text_chunk("done.", msg_id))
    chunks.append(_make_stop_chunk(msg_id))
    chunks.append(_make_done())
    return chunks


# ---------------------------------------------------------------------------
# HTTP handler
# ---------------------------------------------------------------------------

class MockHandler(http.server.BaseHTTPRequestHandler):
    request_count: int = 0  # class-level counter; incremented per POST

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
        self.rfile.read(length) if length else b"{}"

        MockHandler.request_count += 1
        req_no = MockHandler.request_count

        if req_no == 1:
            chunks = _turn1_stream()
        else:
            chunks = _turn2_stream()

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
    parser = argparse.ArgumentParser(
        description="Mock nuclear-askq server for smoke case 31"
    )
    parser.add_argument("--port", type=int, default=8852)
    args = parser.parse_args()

    with open(PID_FILE, "w") as fh:
        fh.write(str(os.getpid()))

    server_address = ("127.0.0.1", args.port)
    httpd = http.server.HTTPServer(server_address, MockHandler)

    print(
        f"mock-nuclear-askq: listening on http://127.0.0.1:{args.port}/v1"
        f" (pid={os.getpid()})",
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
        print("mock-nuclear-askq: stopped", flush=True)


if __name__ == "__main__":
    main()
