#!/usr/bin/env python3
"""
tests/fixtures/fake_scrapling_server.py
----------------------------------------
Minimal fake Scrapling sidecar for integration testing.

This file is also installed as a Python module at
  $TMPDIR/batbox_test_sidecar/scrapling_server/__main__.py
so that SidecarManager can spawn it via:
  python3 -m scrapling_server --port <N>

Routes implemented (all responses are canned JSON):
  GET  /healthz   → {"status":"ok"}
  POST /fetch     → FetchResponse canned JSON
  POST /search    → SearchResponse canned JSON (empty results)
  POST /select    → SelectResponse canned JSON
  POST /shutdown  → {"shutting_down":true}  then server exits

Behaviour variants controlled by env vars:
  FAKE_SIDECAR_IGNORE_SHUTDOWN=1  — /shutdown returns 200 but does NOT exit
                                    (used to test SIGTERM fallback)
  FAKE_SIDECAR_CRASH_AFTER=N      — exit(1) after serving N requests total
                                    (used to test crash-recovery)

Port selection:
  Pass --port N on the command line (SidecarManager always does this).
  Binds on 127.0.0.1:<N>.  Prints nothing to stdout (SidecarManager does not
  read stdout — it polls /healthz instead).
"""

import argparse
import json
import os
import sys
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

# ---------------------------------------------------------------------------
# Canned response bodies
# ---------------------------------------------------------------------------

HEALTH_OK = json.dumps({"status": "ok"})

FETCH_RESPONSE = json.dumps({
    "url": "https://example.com",
    "markdown": "# Example Domain\n\nThis is a fake fetch response.",
    "status_code": 200,
    "content_type": "text/html",
    "content_length": 1234,
    "fetched_at": "2026-01-01T00:00:00Z",
    "truncated": False,
    "is_error": False,
    "error_message": "",
})

SEARCH_RESPONSE = json.dumps({
    "query": "test query",
    "engine": "ddg",
    "results": [
        {
            "title": "Fake Result 1",
            "url": "https://example.com/result1",
            "snippet": "This is a fake search result snippet.",
        }
    ],
    "is_error": False,
    "error_message": "",
})

SELECT_RESPONSE = json.dumps({
    "url": "https://example.com",
    "selector": "h1",
    "matches": ["Example Domain"],
    "count": 1,
    "is_error": False,
    "error_message": "",
})

SHUTDOWN_RESPONSE = json.dumps({"shutting_down": True})

# ---------------------------------------------------------------------------
# Shared state
# ---------------------------------------------------------------------------

_shutdown_event = threading.Event()
_request_count = 0
_request_lock = threading.Lock()

# Read variant configuration from environment.
_IGNORE_SHUTDOWN = os.environ.get("FAKE_SIDECAR_IGNORE_SHUTDOWN", "0") == "1"
_CRASH_AFTER = int(os.environ.get("FAKE_SIDECAR_CRASH_AFTER", "0"))


# ---------------------------------------------------------------------------
# Request handler
# ---------------------------------------------------------------------------

class FakeScraplingHandler(BaseHTTPRequestHandler):

    def log_message(self, fmt, *args):
        # Suppress per-request console noise; all output goes to the stderr
        # pipe that SidecarManager reads.
        pass

    def _read_body(self) -> bytes:
        length = int(self.headers.get("Content-Length", 0))
        return self.rfile.read(length) if length > 0 else b""

    def _send_json(self, status: int, body: str) -> None:
        encoded = body.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)

    def _maybe_crash(self) -> None:
        """Increment request counter and crash if FAKE_SIDECAR_CRASH_AFTER is set."""
        global _request_count
        if _CRASH_AFTER <= 0:
            return
        with _request_lock:
            _request_count += 1
            count = _request_count
        if count >= _CRASH_AFTER:
            # Exit abruptly — simulates a crash for the restart-recovery test.
            os._exit(1)  # noqa: SLF001

    def do_GET(self) -> None:
        path = self.path.split("?")[0]
        if path == "/healthz":
            self._send_json(200, HEALTH_OK)
            self._maybe_crash()
        else:
            self._send_json(404, json.dumps({"error": "not found", "detail": path, "path": path}))

    def do_POST(self) -> None:
        path = self.path.split("?")[0]
        self._read_body()  # consume the request body regardless of path

        if path == "/fetch":
            self._send_json(200, FETCH_RESPONSE)
            self._maybe_crash()

        elif path == "/search":
            self._send_json(200, SEARCH_RESPONSE)
            self._maybe_crash()

        elif path == "/select":
            self._send_json(200, SELECT_RESPONSE)
            self._maybe_crash()

        elif path == "/shutdown":
            self._send_json(200, SHUTDOWN_RESPONSE)
            self.wfile.flush()
            if not _IGNORE_SHUTDOWN:
                _shutdown_event.set()
            # If FAKE_SIDECAR_IGNORE_SHUTDOWN, return 200 but keep running —
            # SidecarManager should fall back to SIGTERM.

        else:
            self._send_json(404, json.dumps({"error": "not found", "detail": path, "path": path}))


# ---------------------------------------------------------------------------
# Entry point (also usable as a module __main__)
# ---------------------------------------------------------------------------

def run(port: int) -> None:
    server = HTTPServer(("127.0.0.1", port), FakeScraplingHandler)

    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()

    # Wait for /shutdown or forever (SidecarManager will SIGTERM us).
    _shutdown_event.wait()
    server.shutdown()
    sys.exit(0)


def main() -> None:
    parser = argparse.ArgumentParser(description="Fake Scrapling sidecar for batbox tests")
    parser.add_argument("--port", type=int, required=True,
                        help="TCP port to bind on 127.0.0.1")
    args = parser.parse_args()
    run(args.port)


if __name__ == "__main__":
    main()
