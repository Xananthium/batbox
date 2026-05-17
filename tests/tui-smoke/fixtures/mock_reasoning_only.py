#!/usr/bin/env python3
"""
fixtures/mock_reasoning_only.py — OpenAI-compatible SSE mock that supports
multiple failure modes for TUI smoke tests.

Modes (--mode flag):
  reasoning-only  (default)
      Emits N reasoning_content chunks then closes cleanly.
      Reproduces TUI-T12 bug trigger: server closes after reasoning, no [DONE],
      no finish_reason, no content.  After TUI-T12 fix batbox exits non-zero
      with "stream ended without content" message.

  error-event
      Emits a single SSE event: error with a context-overflow message then
      closes cleanly.  After TUI-T17 fix batbox exits non-zero with
      "server: <message>" — the exact server text reaches stderr.

  fatal-error
      Same as error-event but uses event: fatal_error.  Both names are handled
      by TUI-T17's branch (`ev.event == "error" || ev.event == "fatal_error"`).

  inline-error
      Emits a plain data-only event (no event: name) whose JSON payload is
      {"error":{"message":"...","type":"context_overflow"}}.  Tests the
      secondary inline-error branch added in TUI-T17.

Port:    8828  (8824 = mock_lmstudio.py, 8826 reserved — both may be in use)
PID:     /tmp/batbox-qa-mock-reasoning-only.pid

Usage:
  python3 mock_reasoning_only.py [--port 8828] [--chunks 7]
                                 [--mode {reasoning-only,error-event,fatal-error,inline-error}]

Requires only Python 3 stdlib (http.server, json, time, argparse, os, sys).
"""

import argparse
import http.server
import json
import os
import time

PID_FILE = "/tmp/batbox-qa-mock-reasoning-only.pid"

# ---------------------------------------------------------------------------
# Mode constants
# ---------------------------------------------------------------------------

MODE_REASONING_ONLY = "reasoning-only"
MODE_ERROR_EVENT    = "error-event"
MODE_FATAL_ERROR    = "fatal-error"
MODE_INLINE_ERROR   = "inline-error"

_VALID_MODES = (
    MODE_REASONING_ONLY,
    MODE_ERROR_EVENT,
    MODE_FATAL_ERROR,
    MODE_INLINE_ERROR,
)

# ---------------------------------------------------------------------------
# Shared error message used by all error modes so smoke cases can grep for it.
# ---------------------------------------------------------------------------

_ERROR_MESSAGE = (
    "The number of tokens to keep from the initial prompt is greater than "
    "the context length. Try to load the model with a larger context length, "
    "or provide a shorter input"
)

# ---------------------------------------------------------------------------
# Reasoning-only SSE stream  (original TUI-T14 behaviour — preserved exactly)
# ---------------------------------------------------------------------------

_REASONING_WORDS = [
    "Let me",
    " think",
    " carefully",
    " about",
    " this",
    " question",
    "...",
]


def _stream_reasoning_only(handler, num_chunks: int) -> None:
    """
    Write an SSE stream of pure reasoning_content deltas, then close cleanly.

    Deliberately emits:
      - NO [DONE] sentinel
      - NO finish_reason chunk
      - NO content delta

    This is the exact server behaviour that causes batbox (pre-T12) to return
    Ok with empty content, exiting 0 with no output.
    """
    handler.send_response(200)
    handler.send_header("Content-Type", "text/event-stream")
    handler.send_header("Cache-Control", "no-cache")
    handler.end_headers()

    words = (_REASONING_WORDS * ((num_chunks // len(_REASONING_WORDS)) + 1))[:num_chunks]

    for word in words:
        payload = {
            "id": "rsn-mock-1",
            "object": "chat.completion.chunk",
            "created": int(time.time()),
            "model": "mock-reasoning-only",
            "choices": [
                {
                    "index": 0,
                    "delta": {"reasoning_content": word},
                    "finish_reason": None,
                }
            ],
        }
        line = "data: {}\n\n".format(json.dumps(payload))
        try:
            handler.wfile.write(line.encode("utf-8"))
            handler.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            return
        time.sleep(0.05)

    # Close cleanly: NO [DONE], NO finish_reason — just let the handler return.
    # The HTTP server will close the TCP connection when do_POST returns.


# ---------------------------------------------------------------------------
# Error-event SSE stream  (TUI-T19 — tests TUI-T17's error-event handler)
# ---------------------------------------------------------------------------

def _stream_error_event(handler, event_name: str) -> None:
    """
    Emit a single SSE event with the given event_name ("error" or
    "fatal_error") carrying a context-overflow error payload, then close.

    This reproduces the exact server behaviour that TUI-T17 handles:
      event: error
      data: {"error":{"message":"<context overflow text>"}}

    After TUI-T17: batbox surfaces "server: <message>" on stderr and exits 1.
    Before TUI-T17: write_cb silently skips the event; T12's generic guard
    fires with a misleading BATBOX_MAX_TOKENS suggestion.
    """
    handler.send_response(200)
    handler.send_header("Content-Type", "text/event-stream")
    handler.send_header("Cache-Control", "no-cache")
    handler.end_headers()

    payload = json.dumps({"error": {"message": _ERROR_MESSAGE}})
    sse_event = "event: {}\ndata: {}\n\n".format(event_name, payload)

    try:
        handler.wfile.write(sse_event.encode("utf-8"))
        handler.wfile.flush()
    except (BrokenPipeError, ConnectionResetError):
        return

    # Close cleanly — no [DONE].


# ---------------------------------------------------------------------------
# Inline-error SSE stream  (TUI-T19 — tests TUI-T17's secondary inline path)
# ---------------------------------------------------------------------------

def _stream_inline_error(handler) -> None:
    """
    Emit a plain data: event (no event: name) whose JSON payload has a
    top-level "error" field.  Tests the secondary branch in TUI-T17's write_cb
    that handles servers which embed the error in a normal data event.

      data: {"error":{"message":"...","type":"context_overflow"}}

    After TUI-T17: same result as error-event — "server: <message>" on stderr.
    """
    handler.send_response(200)
    handler.send_header("Content-Type", "text/event-stream")
    handler.send_header("Cache-Control", "no-cache")
    handler.end_headers()

    payload = json.dumps({
        "error": {
            "message": _ERROR_MESSAGE,
            "type": "context_overflow",
        }
    })
    sse_data = "data: {}\n\n".format(payload)

    try:
        handler.wfile.write(sse_data.encode("utf-8"))
        handler.wfile.flush()
    except (BrokenPipeError, ConnectionResetError):
        return

    # Close cleanly — no [DONE].


# ---------------------------------------------------------------------------
# Request handler
# ---------------------------------------------------------------------------

class ReasoningOnlyHandler(http.server.BaseHTTPRequestHandler):
    """Serves mock SSE streams for all POST /v1/chat/completions."""

    # Set at startup via class attributes.
    num_chunks: int = 7
    mode: str = MODE_REASONING_ONLY

    def log_message(self, fmt, *args):  # silence access log noise
        pass

    def do_GET(self):
        """Minimal health-check so the harness can verify readiness."""
        if self.path in ("/health", "/v1/models"):
            body = json.dumps({"status": "ok", "models": [{"id": "mock-reasoning-only"}]}).encode()
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

        # Consume request body to keep the connection clean.
        length = int(self.headers.get("Content-Length", 0))
        if length:
            self.rfile.read(length)

        m = self.__class__.mode
        if m == MODE_REASONING_ONLY:
            _stream_reasoning_only(self, self.__class__.num_chunks)
        elif m == MODE_ERROR_EVENT:
            _stream_error_event(self, "error")
        elif m == MODE_FATAL_ERROR:
            _stream_error_event(self, "fatal_error")
        elif m == MODE_INLINE_ERROR:
            _stream_inline_error(self)
        else:
            # Should never reach here — argparse validates the mode.
            self.send_response(500)
            self.end_headers()


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Mock server that emits SSE streams for BatBox TUI smoke tests."
    )
    parser.add_argument(
        "--port", type=int, default=8828,
        help="Port to listen on (default 8828)"
    )
    parser.add_argument(
        "--chunks", type=int, default=7,
        help="Number of reasoning_content chunks to emit in reasoning-only mode (default 7)"
    )
    parser.add_argument(
        "--mode",
        choices=list(_VALID_MODES),
        default=MODE_REASONING_ONLY,
        help=(
            "SSE stream mode to serve (default: reasoning-only). "
            "reasoning-only: emit reasoning chunks then close (TUI-T14 fixture). "
            "error-event: emit event:error with context-overflow then close (TUI-T19). "
            "fatal-error: same but event:fatal_error (TUI-T19). "
            "inline-error: emit data-only error payload (TUI-T19)."
        ),
    )
    args = parser.parse_args()

    ReasoningOnlyHandler.num_chunks = args.chunks
    ReasoningOnlyHandler.mode = args.mode

    # Write PID file so the harness can stop us cleanly.
    with open(PID_FILE, "w") as fh:
        fh.write(str(os.getpid()))

    server_address = ("127.0.0.1", args.port)
    httpd = http.server.HTTPServer(server_address, ReasoningOnlyHandler)

    print(
        "mock-reasoning-only: listening on http://127.0.0.1:{}/v1  "
        "(pid={}, mode={})".format(args.port, os.getpid(), args.mode),
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
        print("mock-reasoning-only: stopped", flush=True)


if __name__ == "__main__":
    main()
