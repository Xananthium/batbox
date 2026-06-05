#!/usr/bin/env python3
"""
tests/fixtures/fake_distill_server.py
-------------------------------------
OpenAI-compatible HTTP server for hermetically testing the S4 SubagentDistiller
(DIS-980).  It is a sibling of fake_openai_server.py, kept separate so the
existing client/conversation tests (which assert the get_weather tool_call) are
untouched, while this fixture controls the report_gold contract the distiller
depends on.

The distiller issues a NON-streaming POST /v1/chat/completions whose `tools`
array contains report_gold and whose tool_choice pins it.  This server answers
according to its --mode so each robustness path can be exercised:

    --mode gold       (default) — return a report_gold tool_call carrying a
                                   deterministic golden line (follow_up_ok=True,
                                   confidence=0.91).  The happy path.
    --mode goldnofollowup — like gold but report_gold reports follow_up_ok=False
                                   (the subagent declares it is done).  Drives the
                                   DIS-1007 FOLLOW_UP_OK-CANCEL (close) path.
    --mode notool     — return a normal stop response with content and NO
                                   tool_calls (model never called report_gold).
    --mode wrongtool  — return a tool_call for a DIFFERENT tool (not report_gold).
    --mode error      — always return HTTP 500 (endpoint reachable but failing).

The "unreachable endpoint" path needs no server: the test points the distiller
at a dead port.

Authentication: requires `Authorization: Bearer test-key-123` (401 otherwise),
matching fake_openai_server.py so the same test config works.

Port selection: binds 127.0.0.1 on an ephemeral port (0) by default and prints
"READY <port>" to stdout so the C++ harness can parse it (identical handshake to
fake_openai_server.py).
"""

import argparse
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer


EXPECTED_BEARER = "Bearer test-key-123"

# The deterministic golden line the gold-mode server reports.  The test asserts
# the distilled ToolResult body equals exactly this.
GOLD_ANSWER = "DISTILLED_GOLD_LINE"

ERROR_401_BODY = json.dumps({
    "error": {"message": "Invalid API key", "type": "invalid_request_error",
              "code": "invalid_api_key"}
})

ERROR_500_BODY = json.dumps({
    "error": {"message": "Internal server error (simulated)",
              "type": "server_error", "code": "internal_error"}
})


def report_gold_response() -> dict:
    """A non-streaming response whose assistant message calls report_gold."""
    return {
        "id": "chatcmpl-distill-gold",
        "object": "chat.completion",
        "created": 1716000100,
        "model": "fake-distill-model",
        "choices": [{
            "index": 0,
            "message": {
                "role": "assistant",
                "content": None,
                "tool_calls": [{
                    "id": "call_gold001",
                    "type": "function",
                    "function": {
                        "name": "report_gold",
                        "arguments": json.dumps({
                            "answer": GOLD_ANSWER,
                            "confidence": 0.91,
                            "follow_up_ok": True,
                        }),
                    },
                }],
            },
            "finish_reason": "tool_calls",
        }],
        "usage": {"prompt_tokens": 50, "completion_tokens": 12, "total_tokens": 62},
    }


def report_gold_no_followup_response() -> dict:
    """report_gold with follow_up_ok=False (subagent declares it is done).

    Used by DIS-1007 to drive the FOLLOW_UP_OK-CANCEL path: an investigation was
    predicted by shape, but the subagent's confirm-after signal says do NOT keep
    the window warm.  Identical to gold mode except follow_up_ok is False.
    """
    return {
        "id": "chatcmpl-distill-gold-nofollowup",
        "object": "chat.completion",
        "created": 1716000103,
        "model": "fake-distill-model",
        "choices": [{
            "index": 0,
            "message": {
                "role": "assistant",
                "content": None,
                "tool_calls": [{
                    "id": "call_gold002",
                    "type": "function",
                    "function": {
                        "name": "report_gold",
                        "arguments": json.dumps({
                            "answer": GOLD_ANSWER,
                            "confidence": 0.91,
                            "follow_up_ok": False,
                        }),
                    },
                }],
            },
            "finish_reason": "tool_calls",
        }],
        "usage": {"prompt_tokens": 50, "completion_tokens": 12, "total_tokens": 62},
    }


def wrong_tool_response() -> dict:
    """A tool_call for a tool that is NOT report_gold (distiller must fall back)."""
    return {
        "id": "chatcmpl-distill-wrong",
        "object": "chat.completion",
        "created": 1716000101,
        "model": "fake-distill-model",
        "choices": [{
            "index": 0,
            "message": {
                "role": "assistant",
                "content": None,
                "tool_calls": [{
                    "id": "call_wrong001",
                    "type": "function",
                    "function": {
                        "name": "not_report_gold",
                        "arguments": json.dumps({"whatever": 1}),
                    },
                }],
            },
            "finish_reason": "tool_calls",
        }],
        "usage": {"prompt_tokens": 50, "completion_tokens": 8, "total_tokens": 58},
    }


def no_tool_response() -> dict:
    """A normal stop response with content and no tool_calls (no report_gold)."""
    return {
        "id": "chatcmpl-distill-notool",
        "object": "chat.completion",
        "created": 1716000102,
        "model": "fake-distill-model",
        "choices": [{
            "index": 0,
            "message": {"role": "assistant",
                        "content": "I read it but I am not calling the tool."},
            "finish_reason": "stop",
        }],
        "usage": {"prompt_tokens": 50, "completion_tokens": 10, "total_tokens": 60},
    }


def make_handler(mode: str):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            pass  # quiet

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
            if self.headers.get("Authorization", "") != EXPECTED_BEARER:
                self._send_json(401, json.loads(ERROR_401_BODY))
                return False
            return True

        def do_POST(self):
            path = self.path.rstrip("/")
            if path != "/v1/chat/completions":
                self._send_json(404, {"error": {"message": f"unknown path {self.path}"}})
                return
            if not self._check_auth():
                return
            self._read_body()

            if mode == "error":
                self._send_json(500, json.loads(ERROR_500_BODY))
            elif mode == "notool":
                self._send_json(200, no_tool_response())
            elif mode == "wrongtool":
                self._send_json(200, wrong_tool_response())
            elif mode == "goldnofollowup":
                self._send_json(200, report_gold_no_followup_response())
            else:  # gold (default)
                self._send_json(200, report_gold_response())

    return Handler


def main():
    parser = argparse.ArgumentParser(description="Fake distill server for batbox tests")
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--mode",
                        choices=["gold", "goldnofollowup", "notool", "wrongtool", "error"],
                        default="gold")
    args = parser.parse_args()

    server = HTTPServer(("127.0.0.1", args.port), make_handler(args.mode))
    actual_port = server.server_address[1]
    print(f"READY {actual_port}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    server.server_close()


if __name__ == "__main__":
    main()
