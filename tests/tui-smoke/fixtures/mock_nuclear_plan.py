#!/usr/bin/env python3
"""
fixtures/mock_nuclear_plan.py — OpenAI-compatible SSE mock for PEXT3 1.5 smoke test.

Scripted flow (two request turns):
  Turn 1 (user sends any message):
    Model calls ExitPlanMode with a short plan.
    finish_reason: tool_calls

  Turn 2 (client returns ExitPlanMode tool result):
    Model streams: "done."
    finish_reason: stop

In nuclear mode, BatBox auto-approves ExitPlanMode without showing any modal.
The smoke case asserts NO modal tokens (Approve / Reject / Plan Review / Plan Approval)
appear in the TUI output.

Port:    8851  (reserved for PEXT3 1.5; avoids collisions with 8848/8849/8850)
PID:     /tmp/batbox-qa-mock-nuclear-plan.pid

Usage:
  python3 mock_nuclear_plan.py [--port 8851]

Requires only Python 3 stdlib.
"""

import argparse
import http.server
import json
import os
import time

PID_FILE = "/tmp/batbox-qa-mock-nuclear-plan.pid"
PLAN_TEXT = "## Nuclear Test Plan\n1. Do the thing automatically."


# ---------------------------------------------------------------------------
# SSE chunk builders
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
                           finish: bool = False, msg_id: str = "chatcmpl-np1") -> str:
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
# Turn 1: ExitPlanMode tool call
# ---------------------------------------------------------------------------

def _turn1_stream() -> list:
    """Turn 1: model calls ExitPlanMode with a short plan."""
    chunks = []
    msg_id = "chatcmpl-np1"
    chunks.append(_make_role_chunk(msg_id))

    exit_args = json.dumps({"plan": PLAN_TEXT})

    # Name+id delta (empty args fragment)
    chunks.append(_make_tool_call_chunk("call_np01", "ExitPlanMode", "", msg_id=msg_id))
    time.sleep(0.04)

    # Split args into two fragments for realistic streaming
    mid = len(exit_args) // 2
    chunks.append(_make_tool_call_chunk("call_np01", "", exit_args[:mid], msg_id=msg_id))
    time.sleep(0.04)
    chunks.append(_make_tool_call_chunk("call_np01", "", exit_args[mid:], finish=True, msg_id=msg_id))
    time.sleep(0.04)
    chunks.append(_make_done())
    return chunks


# ---------------------------------------------------------------------------
# Turn 2: acknowledgement after auto-approval
# ---------------------------------------------------------------------------

def _turn2_stream() -> list:
    """Turn 2: model streams a short final message after plan auto-approval."""
    chunks = []
    msg_id = "chatcmpl-np2"
    chunks.append(_make_role_chunk(msg_id))

    for word in ["done", "."]:
        chunks.append(_make_text_chunk(word, msg_id))
        time.sleep(0.03)

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
        description="Mock nuclear-plan server for PEXT3 1.5 smoke test (case 30)"
    )
    parser.add_argument("--port", type=int, default=8851)
    args = parser.parse_args()

    with open(PID_FILE, "w") as fh:
        fh.write(str(os.getpid()))

    server_address = ("127.0.0.1", args.port)
    httpd = http.server.HTTPServer(server_address, MockHandler)

    print(
        f"mock-nuclear-plan: listening on http://127.0.0.1:{args.port}/v1"
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
        print("mock-nuclear-plan: stopped", flush=True)


if __name__ == "__main__":
    main()
