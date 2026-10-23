#!/usr/bin/env python3
"""Capture sampling fields from an isolated CLI talking only to a local stub.

This is a present-day behavior probe, not a reconstruction of the historical
request. No prompt, auth header, tool result, or model reasoning is recorded.
"""
import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
from datetime import datetime, timezone


FIELDS = ("model", "temperature", "top_p", "top_k", "min_p",
          "repetition_penalty", "presence_penalty", "frequency_penalty",
          "max_tokens", "thinking", "output_config", "stream")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--binary", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--model", default="qwen3.6-35b-hauhau-w4a16-vision")
    args = p.parse_args()
    requests = []
    active_case = ""

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            raw = self.rfile.read(int(self.headers.get("Content-Length", "0")))
            body = json.loads(raw)
            requests.append({"case": active_case, "path": self.path,
                             "fields": {k: body[k] for k in FIELDS if k in body}})
            reply = {"id": "msg_local_probe", "type": "message", "role": "assistant",
                     "model": args.model, "content": [{"type": "text", "text": "READY"}],
                     "stop_reason": "end_turn", "stop_sequence": None,
                     "usage": {"input_tokens": 32, "output_tokens": 1}}
            content_type = "application/json"
            if "count_tokens" in self.path:
                data = json.dumps({"input_tokens": 32}).encode()
            elif body.get("stream"):
                start = dict(reply, content=[], stop_reason=None,
                             usage={"input_tokens": 32, "output_tokens": 0})
                events = [
                    {"type": "message_start", "message": start},
                    {"type": "content_block_start", "index": 0,
                     "content_block": {"type": "text", "text": ""}},
                    {"type": "content_block_delta", "index": 0,
                     "delta": {"type": "text_delta", "text": "READY"}},
                    {"type": "content_block_stop", "index": 0},
                    {"type": "message_delta", "delta": {"stop_reason": "end_turn",
                                                           "stop_sequence": None},
                     "usage": {"output_tokens": 1}},
                    {"type": "message_stop"}]
                data = "".join(f"event: {e['type']}\ndata: {json.dumps(e)}\n\n" for e in events).encode()
                content_type = "text/event-stream"
            else:
                data = json.dumps(reply).encode()
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    outcomes = []
    try:
        with tempfile.TemporaryDirectory(prefix="batbox-cli-sampling-") as temp:
            env = {k: v for k, v in os.environ.items() if k in {"PATH", "HOME", "LANG", "TERM"}}
            env.update({"ANTHROPIC_BASE_URL": f"http://127.0.0.1:{server.server_address[1]}",
                        "ANTHROPIC_AUTH_TOKEN": "local-stub-probe",
                        "CLAUDE_CONFIG_DIR": str(Path(temp) / "config"),
                        "CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC": "1"})
            base = [str(args.binary), "--safe-mode", "--setting-sources", "",
                    "--strict-mcp-config", "--mcp-config", '{"mcpServers":{}}', "--tools", "",
                    "--no-session-persistence", "--model", args.model, "--effort", "max",
                    "--max-turns", "1", "--output-format", "json", "-p", "Reply READY only."]
            cases = [("main", []), ("builtin_agent", ["--agent", "general-purpose"])]
            for active_case, extra in cases:
                run = subprocess.run(base + extra, cwd=temp, env=env, capture_output=True,
                                     text=True, timeout=30)
                outcomes.append({"case": active_case, "exit_code": run.returncode,
                                 "response_has_READY": "READY" in run.stdout,
                                 "stderr_sha256": hashlib.sha256(run.stderr.encode()).hexdigest()})
                if run.returncode:
                    print(run.stderr[:2000])
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
    result = {"utc": datetime.now(timezone.utc).isoformat(), "cli_version": subprocess.check_output(
        [str(args.binary), "--version"], text=True).strip(),
        "cli_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(),
        "inference": "local deterministic HTTP stub; no GPU or serving backend",
        "configuration": "isolated CLI config, no user/project settings, no tools or MCP",
        "limitations": "Current direct and builtin-agent behavior only; historical request and spawned-subagent overrides remain unknown. Isolated configuration can differ from the user's original configuration.",
        "requests": requests, "outcomes": outcomes}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    assert requests and all(o["exit_code"] == 0 and o["response_has_READY"] for o in outcomes)


if __name__ == "__main__":
    main()
