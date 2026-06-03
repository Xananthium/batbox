#!/usr/bin/env python3
"""
tests/fixtures/ollama_proxy.py
-------------------------------
A thin, COUNTING, temperature-pinning reverse proxy in front of a real local
ollama daemon (OpenAI-compatible at http://127.0.0.1:11434/v1), used by
tests/integration/test_selection_heuristic_ollama.cpp (DIS-1017) to re-prove the
DIS-1007 AC2/AC3 behaviour against a REAL model instead of the fake server.

Why a proxy (and why it is honest, not a stub):
  * The REAL model still does every bit of the generation — this process only
    FORWARDS bytes to ollama.  It rewrites exactly two things on the request:
      - forces `temperature: 0` (the determinism strategy the spec mandates), and
      - injects `stream_options.include_usage=true` on streamed calls so ollama
        reports token `usage` in the final SSE chunk (it does for free on
        non-streamed calls).
  * It COUNTS POST /v1/chat/completions requests and ACCUMULATES token usage, and
    exposes both on GET /__stats.  That count is the structural lever for the
    AC2 "no-re-engulf" assertion: the SELECTOR's first-turn distill is exactly
    one request on the selector-proxy; an interrogation of the warm window must
    NOT add another request there (it hits the warm-proxy instead).  Token usage
    on /__stats satisfies A-AC5 (and seeds DIS-1012 Child B).

Topology (mirrors the two-fake-server topology of the hermetic test):
  selector cfg.distill.*  -> proxy A (this script)  -> ollama   [first-turn gold]
  supervisor cfg.distill.*-> proxy B (this script)  -> ollama   [warm conversation
                                                                  + interrogations]
Both proxies forward to the same ollama, so the MODEL is held constant; the two
ports just let the test attribute requests to the selector vs the warm path.

Lifecycle: this is a long-running service (like ollama), NOT the fork/READY
fixture's one-shot — but it uses the SAME "READY <port>" stdout handshake so the
C++ FakeServer RAII (fork + read READY) can spawn and reap it identically.

Env:
  OLLAMA_BASE        upstream base (default "http://127.0.0.1:11434")
  PROXY_FORCE_TEMP   forced temperature (default "0"; empty string disables)

Routes:
  POST /v1/chat/completions   forward (counted; temp forced; usage captured;
                              non-stream JSON or SSE passthrough)
  GET  /v1/models             forward (uncounted; used by the startup guard)
  GET  /__stats               {"chat_requests", "prompt_tokens",
                              "completion_tokens", "total_tokens", "last_usage"}
  *                           forwarded verbatim (uncounted)
"""

import argparse
import json
import os
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit
import http.client


def _upstream_base() -> str:
    return os.environ.get("OLLAMA_BASE", "http://127.0.0.1:11434").rstrip("/")


def _forced_temp():
    raw = os.environ.get("PROXY_FORCE_TEMP", "0")
    if raw == "":
        return None
    try:
        return float(raw)
    except ValueError:
        return 0.0


class Stats:
    """Thread-safe request + token counters (ThreadingHTTPServer → concurrent)."""

    def __init__(self):
        self._lock = threading.Lock()
        self.chat_requests = 0
        self.prompt_tokens = 0
        self.completion_tokens = 0
        self.total_tokens = 0
        self.last_usage = None

    def count_request(self):
        with self._lock:
            self.chat_requests += 1

    def add_usage(self, usage: dict):
        if not isinstance(usage, dict):
            return
        with self._lock:
            self.prompt_tokens += int(usage.get("prompt_tokens", 0) or 0)
            self.completion_tokens += int(usage.get("completion_tokens", 0) or 0)
            self.total_tokens += int(usage.get("total_tokens", 0) or 0)
            self.last_usage = usage

    def snapshot(self) -> dict:
        with self._lock:
            return {
                "chat_requests": self.chat_requests,
                "prompt_tokens": self.prompt_tokens,
                "completion_tokens": self.completion_tokens,
                "total_tokens": self.total_tokens,
                "last_usage": self.last_usage,
            }


def _debug_dump(method, path, req_body, resp_json):
    """Optional wire-debug: dump request temp + last user msg + response tool args.
    Enabled only when PROXY_DEBUG_LOG is set.  Used to diagnose organ-vs-probe
    signal divergence (DIS-1017)."""
    logpath = os.environ.get("PROXY_DEBUG_LOG", "")
    if not logpath:
        return
    try:
        rb = json.loads(req_body.decode("utf-8")) if req_body else {}
        msgs = rb.get("messages", [])
        last = msgs[-1].get("content", "")[:200] if msgs else ""
        tc = (resp_json.get("choices", [{}])[0].get("message", {}) or {}).get("tool_calls")
        targs = tc[0]["function"]["arguments"] if tc else None
        with open(logpath, "a") as f:
            f.write(json.dumps({
                "temperature": rb.get("temperature"),
                "model": rb.get("model"),
                "tool_choice": rb.get("tool_choice"),
                "tools": rb.get("tools"),
                "last_user_content": last,
                "resp_tool_args": targs,
            }) + "\n")
    except Exception:
        pass


def make_handler(stats: Stats):
    base = _upstream_base()
    up = urlsplit(base)
    up_host = up.hostname
    up_port = up.port or (443 if up.scheme == "https" else 80)
    up_prefix = up.path.rstrip("/")  # usually "" for ".../11434"; "/v1" lives in the route

    forced_temp = _forced_temp()

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            pass  # quiet

        # --- helpers ---------------------------------------------------------
        def _read_body(self) -> bytes:
            length = int(self.headers.get("Content-Length", 0) or 0)
            return self.rfile.read(length) if length > 0 else b""

        def _send_json(self, status: int, obj):
            payload = json.dumps(obj).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        def _forward_headers(self) -> dict:
            # Pass auth through; drop hop-by-hop / length / encoding headers we
            # recompute or that would corrupt the relayed stream.
            drop = {"host", "content-length", "accept-encoding", "connection"}
            out = {}
            for k, v in self.headers.items():
                if k.lower() in drop:
                    continue
                out[k] = v
            out["Accept-Encoding"] = "identity"  # no gzip → we can sniff usage
            return out

        def _upstream(self):
            if up.scheme == "https":
                return http.client.HTTPSConnection(up_host, up_port, timeout=120)
            return http.client.HTTPConnection(up_host, up_port, timeout=120)

        # --- GET -------------------------------------------------------------
        def do_GET(self):
            if self.path.rstrip("/") == "/__stats":
                self._send_json(200, stats.snapshot())
                return
            self._proxy("GET", self.path, b"", count=False, allow_stream=False)

        # --- POST ------------------------------------------------------------
        def do_POST(self):
            path = self.path
            body = self._read_body()
            is_chat = path.rstrip("/").endswith("/chat/completions")

            stream = False
            if is_chat and body:
                try:
                    obj = json.loads(body.decode("utf-8"))
                    if forced_temp is not None:
                        obj["temperature"] = forced_temp
                    stream = bool(obj.get("stream", False))
                    if stream:
                        so = obj.get("stream_options") or {}
                        so["include_usage"] = True
                        obj["stream_options"] = so
                    body = json.dumps(obj).encode("utf-8")
                except (ValueError, UnicodeDecodeError):
                    pass  # not JSON we understand — relay verbatim

            self._proxy("POST", path, body, count=is_chat, allow_stream=stream)

        # --- core relay ------------------------------------------------------
        def _proxy(self, method: str, path: str, body: bytes,
                   count: bool, allow_stream: bool):
            if count:
                stats.count_request()
            try:
                conn = self._upstream()
                conn.request(method, up_prefix + path, body=body,
                             headers=self._forward_headers())
                resp = conn.getresponse()
            except Exception as e:  # upstream unreachable → 502 (test fails closed)
                self._send_json(502, {"error": {"message": f"proxy upstream error: {e}"}})
                return

            ctype = resp.getheader("Content-Type", "")
            is_sse = "text/event-stream" in ctype or (allow_stream and resp.status == 200)

            if is_sse:
                self._relay_sse(resp)
            else:
                data = resp.read()
                # capture usage from a non-stream chat JSON
                if count and resp.status == 200:
                    try:
                        jr = json.loads(data.decode("utf-8"))
                        stats.add_usage(jr.get("usage"))
                        _debug_dump(method, path, body, jr)
                    except (ValueError, UnicodeDecodeError, AttributeError):
                        pass
                self.send_response(resp.status)
                self.send_header("Content-Type", ctype or "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                if method != "HEAD":
                    self.wfile.write(data)
            try:
                conn.close()
            except Exception:
                pass

        def _relay_sse(self, resp):
            # Stream the SSE body through unchanged, sniffing each `data:` line
            # for a token `usage` block (ollama emits it in the final chunk when
            # stream_options.include_usage=true).
            self.send_response(resp.status)
            self.send_header("Content-Type",
                             resp.getheader("Content-Type", "text/event-stream"))
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "close")
            self.end_headers()
            buf = b""
            while True:
                chunk = resp.read(1024)
                if not chunk:
                    break
                try:
                    self.wfile.write(chunk)
                    self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError):
                    break
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    s = line.strip()
                    if s.startswith(b"data:"):
                        payload = s[5:].strip()
                        if payload and payload != b"[DONE]":
                            try:
                                u = json.loads(payload.decode("utf-8")).get("usage")
                                if u:
                                    stats.add_usage(u)
                            except (ValueError, UnicodeDecodeError, AttributeError):
                                pass

    return Handler


def main():
    parser = argparse.ArgumentParser(description="Counting temp-0 ollama proxy for batbox tests")
    parser.add_argument("--port", type=int, default=0)
    args = parser.parse_args()

    stats = Stats()
    server = ThreadingHTTPServer(("127.0.0.1", args.port), make_handler(stats))
    actual_port = server.server_address[1]
    print(f"READY {actual_port}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    server.server_close()


if __name__ == "__main__":
    main()
