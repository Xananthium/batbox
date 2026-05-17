#!/usr/bin/env python3
"""
fixtures/mock_plan_with_cards.py — OpenAI-compatible SSE mock that drives the
full /plan + AskUserQuestion + ExitPlanMode approve flow for TUI-ASKQ-T6.

Scripted flow (four request turns):
  Turn 1 (user sends /plan prompt):
    Model calls AskUserQuestion with:
      questions: [{question: "Which framework?",
                   options: [{label:"Flask"},{label:"FastAPI"},{label:"Django"}]}]
    finish_reason: tool_calls

  Turn 2 (client returns AskUserQuestion tool result with FastAPI):
    Model calls AskUserQuestion with:
      questions: [{question: "DB?",
                   options: [{label:"Postgres"},{label:"SQLite"}]}]
    finish_reason: tool_calls

  Turn 3 (client returns AskUserQuestion tool result with Postgres):
    Model streams plan markdown:
      "# Plan\\n1. Scaffold FastAPI\\n2. Wire Postgres\\n3. Tests"
    then calls ExitPlanMode with that same plan text.
    finish_reason: tool_calls

  Turn 4 (client returns ExitPlanMode tool result with approved=true):
    Model streams: "starting now."
    finish_reason: stop

Port:    8850  (reserved for TUI-ASKQ-T6; avoids collisions with 8848/8849)
PID:     /tmp/batbox-qa-mock-plan-with-cards.pid

Usage:
  python3 mock_plan_with_cards.py [--port 8850]

Requires only Python 3 stdlib.
"""

import argparse
import http.server
import json
import os
import time

PID_FILE = "/tmp/batbox-qa-mock-plan-with-cards.pid"

PLAN_TEXT = "# Plan\n1. Scaffold FastAPI\n2. Wire Postgres\n3. Tests"

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
                           finish: bool = False, msg_id: str = "chatcmpl-pwc1") -> str:
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
# Turn 1: AskUserQuestion — "Which framework?"
# ---------------------------------------------------------------------------

def _turn1_stream() -> list:
    """
    First user turn: model calls AskUserQuestion with framework choices.
    options use the schema: questions array with options as objects with 'label'.
    """
    chunks = []
    msg_id = "chatcmpl-pwc1"
    chunks.append(_make_role_chunk(msg_id))

    # Build AskUserQuestion args for Q1
    args = json.dumps({
        "questions": [
            {
                "question": "Which framework?",
                "options": [
                    {"label": "Flask"},
                    {"label": "FastAPI"},
                    {"label": "Django"},
                ]
            }
        ]
    })

    # Emit name chunk first (empty args), then args in two parts
    chunks.append(_make_tool_call_chunk("call_pwc01", "AskUserQuestion", "", msg_id=msg_id))
    time.sleep(0.04)
    mid = len(args) // 2
    chunks.append(_make_tool_call_chunk("call_pwc01", "", args[:mid], msg_id=msg_id))
    time.sleep(0.04)
    chunks.append(_make_tool_call_chunk("call_pwc01", "", args[mid:], finish=True, msg_id=msg_id))
    time.sleep(0.04)
    chunks.append(_make_done())
    return chunks


# ---------------------------------------------------------------------------
# Turn 2: AskUserQuestion — "DB?"
# ---------------------------------------------------------------------------

def _turn2_stream() -> list:
    """
    After client returns tool result with FastAPI selection:
    model calls AskUserQuestion with DB choices.
    """
    chunks = []
    msg_id = "chatcmpl-pwc2"
    chunks.append(_make_role_chunk(msg_id))

    args = json.dumps({
        "questions": [
            {
                "question": "DB?",
                "options": [
                    {"label": "Postgres"},
                    {"label": "SQLite"},
                ]
            }
        ]
    })

    chunks.append(_make_tool_call_chunk("call_pwc02", "AskUserQuestion", "", msg_id=msg_id))
    time.sleep(0.04)
    mid = len(args) // 2
    chunks.append(_make_tool_call_chunk("call_pwc02", "", args[:mid], msg_id=msg_id))
    time.sleep(0.04)
    chunks.append(_make_tool_call_chunk("call_pwc02", "", args[mid:], finish=True, msg_id=msg_id))
    time.sleep(0.04)
    chunks.append(_make_done())
    return chunks


# ---------------------------------------------------------------------------
# Turn 3: plan markdown + ExitPlanMode
# ---------------------------------------------------------------------------

def _turn3_stream() -> list:
    """
    After client returns tool result with Postgres selection:
    model streams plan text then calls ExitPlanMode.
    """
    chunks = []
    msg_id = "chatcmpl-pwc3"
    chunks.append(_make_role_chunk(msg_id))

    # Stream the plan text tokens
    plan_tokens = ["# Plan\n", "1. Scaffold FastAPI\n", "2. Wire Postgres\n", "3. Tests"]
    for token in plan_tokens:
        chunks.append(_make_text_chunk(token, msg_id))
        time.sleep(0.04)

    # Now emit ExitPlanMode tool call
    exit_args = json.dumps({"plan": PLAN_TEXT})
    chunks.append(_make_tool_call_chunk("call_pwc03", "ExitPlanMode", "", msg_id=msg_id))
    time.sleep(0.04)
    mid = len(exit_args) // 2
    chunks.append(_make_tool_call_chunk("call_pwc03", "", exit_args[:mid], msg_id=msg_id))
    time.sleep(0.04)
    chunks.append(_make_tool_call_chunk("call_pwc03", "", exit_args[mid:], finish=True, msg_id=msg_id))
    time.sleep(0.04)
    chunks.append(_make_done())
    return chunks


# ---------------------------------------------------------------------------
# Turn 4: final acknowledgement after approval
# ---------------------------------------------------------------------------

def _turn4_stream() -> list:
    """
    After client returns ExitPlanMode tool result (approved=true):
    model emits a short final message.
    """
    chunks = []
    msg_id = "chatcmpl-pwc4"
    chunks.append(_make_role_chunk(msg_id))

    words = ["starting", " now", "."]
    for w in words:
        chunks.append(_make_text_chunk(w, msg_id))
        time.sleep(0.04)

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
        elif req_no == 2:
            chunks = _turn2_stream()
        elif req_no == 3:
            chunks = _turn3_stream()
        else:
            # Turn 4 and any subsequent fallback
            chunks = _turn4_stream()

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
        description="Mock plan-with-cards server for TUI-ASKQ-T6"
    )
    parser.add_argument("--port", type=int, default=8850)
    args = parser.parse_args()

    with open(PID_FILE, "w") as fh:
        fh.write(str(os.getpid()))

    server_address = ("127.0.0.1", args.port)
    httpd = http.server.HTTPServer(server_address, MockHandler)

    print(
        f"mock-plan-with-cards: listening on http://127.0.0.1:{args.port}/v1"
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
        print("mock-plan-with-cards: stopped", flush=True)


if __name__ == "__main__":
    main()
