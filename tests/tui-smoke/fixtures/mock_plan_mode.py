#!/usr/bin/env python3
"""
fixtures/mock_plan_mode.py — OpenAI-compatible SSE mock that simulates the
plan-mode double-render scenario for TUI-PLAN-T3 regression tests.

Flow:
  1. First POST → streams plan text content ("# Plan\nstep 1\nstep 2") then
     finish_reason="tool_calls" with an ExitPlanMode tool call.
  2. Second POST (with tool result in messages) → streams a brief follow-up
     reply then finish_reason="stop".

This exercises the exact code path where streaming_buffer_ could retain the
plan text after the tool-call MessageAppended event fires, causing double-render
at StreamDone of the second turn.

Port:    8849  (reserved for TUI-PLAN-T3)
PID:     /tmp/batbox-qa-mock-plan-mode.pid

Usage:
  python3 mock_plan_mode.py [--port 8849]

Requires only Python 3 stdlib.
"""

import argparse
import http.server
import json
import os
import time

PID_FILE = "/tmp/batbox-qa-mock-plan-mode.pid"

# ---------------------------------------------------------------------------
# SSE helpers
# ---------------------------------------------------------------------------

def _make_text_chunk(content: str, msg_id: str = "chatcmpl-pm1") -> str:
    payload = {
        "id": msg_id,
        "object": "chat.completion.chunk",
        "created": int(time.time()),
        "model": "mock-lm",
        "choices": [{"index": 0, "delta": {"content": content}, "finish_reason": None}],
    }
    return "data: " + json.dumps(payload) + "\n\n"


def _make_tool_call_chunk(index: int, call_id: str, name: str, args_frag: str,
                           finish: bool = False, msg_id: str = "chatcmpl-pm1") -> str:
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


def _make_done() -> str:
    return "data: [DONE]\n\n"


# ---------------------------------------------------------------------------
# Turn 1: plan content + ExitPlanMode tool call
# ---------------------------------------------------------------------------

def _plan_stream() -> list:
    """
    Stream the plan text then call ExitPlanMode.
    This is the trigger for the double-render bug: streaming_buffer_ holds the
    plan text when the MessageAppended("assistant") event fires.
    """
    chunks = []

    # Role header
    role_payload = {
        "id": "chatcmpl-pm1",
        "object": "chat.completion.chunk",
        "created": int(time.time()),
        "model": "mock-lm",
        "choices": [{"index": 0, "delta": {"role": "assistant"}, "finish_reason": None}],
    }
    chunks.append("data: " + json.dumps(role_payload) + "\n\n")

    # Stream the plan text content
    plan_tokens = ["# ", "Plan\n", "step 1\n", "step 2"]
    for token in plan_tokens:
        chunks.append(_make_text_chunk(token))
        time.sleep(0.04)

    # Now emit the ExitPlanMode tool call with finish_reason=tool_calls
    chunks.append(_make_tool_call_chunk(0, "call_pm01", "ExitPlanMode", ""))
    time.sleep(0.04)
    chunks.append(_make_tool_call_chunk(
        0, "call_pm01", "", '{"plan": "# Plan\\nstep 1\\nstep 2"}', finish=True
    ))
    time.sleep(0.04)
    chunks.append(_make_done())
    return chunks


# ---------------------------------------------------------------------------
# Turn 2: brief follow-up reply after tool result
# ---------------------------------------------------------------------------

def _followup_stream() -> list:
    """Plain text follow-up after the tool result is returned."""
    words = ["Plan", " submitted."]
    chunks = []
    role_payload = {
        "id": "chatcmpl-pm2",
        "object": "chat.completion.chunk",
        "created": int(time.time()),
        "model": "mock-lm",
        "choices": [{"index": 0, "delta": {"role": "assistant"}, "finish_reason": None}],
    }
    chunks.append("data: " + json.dumps(role_payload) + "\n\n")

    for w in words:
        chunks.append(_make_text_chunk(w, msg_id="chatcmpl-pm2"))
        time.sleep(0.04)

    finish_payload = {
        "id": "chatcmpl-pm2",
        "object": "chat.completion.chunk",
        "created": int(time.time()),
        "model": "mock-lm",
        "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}],
    }
    chunks.append("data: " + json.dumps(finish_payload) + "\n\n")
    chunks.append(_make_done())
    return chunks


# ---------------------------------------------------------------------------
# HTTP handler
# ---------------------------------------------------------------------------

class MockHandler(http.server.BaseHTTPRequestHandler):
    request_count: int = 0

    def log_message(self, fmt, *args):
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

        # Turn 1 → plan + tool call; Turn 2+ → follow-up text
        chunks = _plan_stream() if req_no == 1 else _followup_stream()

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
    parser = argparse.ArgumentParser(description="Mock plan-mode server for TUI-PLAN-T3")
    parser.add_argument("--port", type=int, default=8849)
    args = parser.parse_args()

    with open(PID_FILE, "w") as fh:
        fh.write(str(os.getpid()))

    server_address = ("127.0.0.1", args.port)
    httpd = http.server.HTTPServer(server_address, MockHandler)

    print(
        f"mock-plan-mode: listening on http://127.0.0.1:{args.port}/v1 (pid={os.getpid()})",
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
        print("mock-plan-mode: stopped", flush=True)


if __name__ == "__main__":
    main()
