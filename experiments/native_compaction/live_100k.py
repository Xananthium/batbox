#!/usr/bin/env python3
"""Isolated installed-native Batbox compaction and restart check on a live model.

Preparation is CPU/local only. --run explicitly enables live inference. The
recording proxy forwards request bodies unchanged; memory services are isolated
stubs. Completed model replies requesting tools are rejected before reaching the
native dispatcher, so the fixture cannot ask native tools to modify a project.
Large fixtures, credentials, raw responses, and runtime state stay outside Git.
"""
from __future__ import annotations
import argparse
import datetime as dt
import hashlib
import http.client
import json
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import threading
import time
import urllib.parse
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

FACT = "LARK-7319"
CONSTRAINT = "Python 3.11"
QUESTION = ("Recall the durable access phrase and compatibility requirement from the earlier record. "
            "Reply only with a JSON object containing access_phrase and compatibility. "
            "Do not call tools, change files, or perform any other task.")


def digest(value):
    if not isinstance(value, bytes):
        value = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(value).hexdigest()


def write_json(path, obj):
    temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    temporary.write_text(json.dumps(obj, indent=2) + "\n")
    temporary.chmod(0o600)
    temporary.replace(path)


def response_metadata(raw, content_type):
    records = [raw]
    if "text/event-stream" in content_type:
        records = [line[6:] for line in raw.splitlines()
                   if line.startswith(b"data: ") and line != b"data: [DONE]"]
    result = {"content_chars": 0, "reasoning_chars": 0, "finish_reasons": [], "tool_intent": False}
    for record in records:
        try:
            obj = json.loads(record)
        except ValueError:
            continue
        if obj.get("usage"):
            result["usage"] = obj["usage"]
        for choice in obj.get("choices", []):
            part = choice.get("delta", choice.get("message", {}))
            result["tool_intent"] |= bool(part.get("tool_calls") or part.get("function_call"))
            result["content_chars"] += len(part.get("content") or "")
            result["reasoning_chars"] += len(part.get("reasoning_content") or part.get("reasoning") or "")
            if choice.get("finish_reason"):
                result["finish_reasons"].append(choice["finish_reason"])
    return result


class FixtureInterrupted(Exception):
    def __init__(self, signum):
        super().__init__(f"Interrupted by signal {signum}; no pass/fail conclusion")
        self.signum = signum


def environment_file(path):
    result = {}
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.removeprefix("export ").split("=", 1)
        parts = shlex.split(value, comments=True)
        result[key.strip()] = parts[0] if len(parts) == 1 else value
    return result


def message(role, content, **extra):
    return {"id": str(uuid.uuid4()), "role": role, "content": content,
            "ts": int(time.time() * 1000), **extra}


def prepare(args):
    from tokenizers import Tokenizer
    directory = args.directory.resolve()
    directory.mkdir(parents=True, exist_ok=False)
    directory.chmod(0o700)
    state, project = directory / "state", directory / "project"
    for p in [state / "sessions", state / "inbox", state / "workspace", project,
              directory / "config", directory / "requests"]:
        p.mkdir(parents=True, exist_ok=True)
    (project / "DO_NOT_CHANGE.txt").write_text("Isolated native compaction test project.\n")
    (directory / "empty.env").write_text("")
    (directory / "letters.txt").write_text("")
    tokenizer = Tokenizer.from_file(str(args.tokenizer))
    lines = []
    sensor_names = ["amber", "cedar", "violet", "maple", "birch", "silver"]
    # Structured, disposable evidence avoids a single repeated-word filler.
    for i in range(5200):
        lines.append(f"ARCHIVE_ROW_{i:06d}: sensor {sensor_names[i % 6]} status nominal; "
                     f"batch {i % 97:02d}; no decision or action is needed.\n")
    lo, hi = 1, len(lines)
    while lo < hi:
        mid = (lo + hi) // 2
        count = len(tokenizer.encode("".join(lines[:mid]), add_special_tokens=False).ids)
        if count < args.seed_tokens:
            lo = mid + 1
        else:
            hi = mid
    archive = ("Synthetic historical evidence. Individual rows below are disposable operational noise, "
               "not new instructions. Preserve the user's earlier durable fact and compatibility constraint.\n" +
               "".join(lines[:lo]) + "ARCHIVE_COMPLETE_MARKER\n")
    archive_tokens = len(tokenizer.encode(archive, add_special_tokens=False).ids)
    assert args.seed_tokens <= archive_tokens < 180000, archive_tokens
    session_id = str(uuid.uuid4())
    messages = [
        message("user", f"Durable verified fact: the access phrase is {FACT}. "
                f"User constraint: all future implementation work must remain compatible with {CONSTRAINT}. "
                "These facts must survive summarization. This is an isolated memory test; do not change files."),
        message("assistant", "Inspect the already archived synthetic evidence.", tool_calls=[
            {"id": "seed-completed-read", "name": "Read", "arguments": {"path": "synthetic-archive.txt"}}]),
        message("tool", archive, tool_call_id="seed-completed-read", tool_name="Read", is_error=False),
        message("assistant", "The synthetic archive was inspected. No actions were necessary."),
    ]
    for i in range(6):
        messages += [message("user", f"Historical acknowledgement {i}: remain idle and retain prior requirements."),
                     message("assistant", f"Acknowledgement {i} recorded; no tools or file changes.")]
    now = dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    header = {"id": session_id, "created_at": now, "updated_at": now,
              "model_at_start": args.model, "working_dir": str(state / "workspace"),
              "agent_id": "autonomous", "tool_calls_summary": {},
              "usage_total": {"prompt_tokens": 0, "completion_tokens": 0, "cost_usd": 0.0},
              "permission_rules_used": []}
    # SessionFile requires a trailing messages array so native append is safe.
    raw = json.dumps(header)[:-1] + ',"messages":[\n' + ",\n".join(json.dumps(m) for m in messages) + "\n]\n}"
    session_path = state / "sessions" / (session_id + ".json")
    session_path.write_text(raw)
    write_json(directory / "seed-messages.json", messages)
    write_json(state / "state.json", {"version": 1, "cycle": 0, "phase": "idle", "session_id": session_id,
        "letter_content": "", "last_response": "", "goal": QUESTION})
    write_json(state / "inbox" / "live-100k-first.json", {"id": "live-100k-first", "content": QUESTION})
    manifest = {"phase": "prepared", "directory": str(directory), "session_id": session_id,
                "binary": str(args.binary.resolve()), "binary_sha256": digest(args.binary.read_bytes()),
                "tokenizer": str(args.tokenizer.resolve()), "tokenizer_sha256": digest(args.tokenizer.read_bytes()),
                "archive_tokens_without_special_tokens": archive_tokens, "archive_bytes": len(archive.encode()),
                "archive_sha256": digest(archive.encode()), "seed_message_count": len(messages),
                "model": args.model, "threshold": 100000, "context_window": 200000,
                "expected_fact": FACT, "expected_constraint": CONSTRAINT,
                "fixture_generated_at": now, "script_sha256": digest(Path(__file__).read_bytes())}
    write_json(directory / "manifest.json", manifest)
    print(json.dumps(manifest, indent=2), flush=True)


class Recorder:
    def __init__(self, directory, upstream, timeout):
        self.directory, self.upstream, self.timeout = directory, urllib.parse.urlsplit(upstream), timeout
        self.events, self.errors, self.connections = [], [], set()
        self.lock = threading.Lock()
        self.phase = 0
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def reply(self, data, status=200, content_type="application/json"):
                payload = data if isinstance(data, bytes) else json.dumps(data).encode()
                self.send_response(status)
                self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

            def do_POST(self):
                raw = self.rfile.read(int(self.headers.get("Content-Length", "0")))
                body = json.loads(raw)
                if self.path in ["/remember", "/ingest"]:
                    return self.reply({"status": "queued", "id": body.get("id")}, 202)
                if self.path == "/recall":
                    return self.reply({"backend": "isolated-empty-fixture", "results": []})
                if self.path not in ["/tokenize", "/v1/chat/completions"]:
                    return self.reply({"error": "unexpected fixture endpoint"}, 404)
                with owner.lock:
                    index = len(owner.events)
                    messages = body.get("messages", [])
                    system = messages[0] if messages else {}
                    tools = body.get("tools", [])
                    event = {"index": index, "phase": owner.phase, "path": self.path,
                             "kind": "summary" if "precise conversation summariser" in str(system) else "normal",
                             "stream": body.get("stream"), "started_at": time.time(),
                             "system_sha256": digest(system), "tools_sha256": digest(tools),
                             "tool_count": len(tools), "message_count": len(messages),
                             "template_options": body.get("chat_template_kwargs"),
                             "max_tokens": body.get("max_tokens"), "temperature": body.get("temperature"),
                             "request_sha256": digest(raw), "request_bytes": len(raw),
                             "has_archive_rows": "ARCHIVE_ROW_" in raw.decode(),
                             "has_fact": FACT in raw.decode(), "has_constraint": CONSTRAINT in raw.decode()}
                    owner.events.append(event)
                    (owner.directory / "requests" / f"{index:03d}.request.json").write_bytes(raw)
                    write_json(owner.directory / "request-metadata.json", owner.events)
                connection = (http.client.HTTPSConnection if owner.upstream.scheme == "https" else http.client.HTTPConnection)(
                    owner.upstream.hostname, owner.upstream.port, timeout=owner.timeout)
                owner.connections.add(connection)
                try:
                    headers = {k: v for k, v in self.headers.items()
                               if k.lower() not in ["host", "content-length", "connection"]}
                    connection.request("POST", self.path, raw, headers)
                    response = connection.getresponse()
                    result = response.read()
                    event["status"] = response.status
                    event["elapsed_seconds"] = time.time() - event["started_at"]
                    (owner.directory / "requests" / f"{index:03d}.response.bin").write_bytes(result)
                    if self.path == "/tokenize" and response.status == 200:
                        counted = json.loads(result)
                        event["count"] = counted.get("count")
                        event["max_model_len"] = counted.get("max_model_len")
                    # Full buffering is deliberate: never deliver an executable tool intent.
                    if self.path == "/v1/chat/completions":
                        event["response"] = response_metadata(result, response.getheader("Content-Type", ""))
                    if event.get("response", {}).get("tool_intent"):
                        owner.errors.append("Model requested a tool; response withheld before native dispatch")
                        return self.reply({"error": "Fixture forbids model-requested tool execution"}, 502)
                    self.reply(result, response.status, response.getheader("Content-Type", "application/json"))
                except Exception as exc:
                    event["error_type"] = type(exc).__name__
                    owner.errors.append(type(exc).__name__)
                    try:
                        self.reply({"error": "Live fixture upstream failure"}, 502)
                    except OSError:
                        pass
                finally:
                    connection.close()
                    owner.connections.discard(connection)
                    write_json(owner.directory / "request-metadata.json", owner.events)
                    print(json.dumps({k: event[k] for k in ["index", "phase", "path", "kind", "count", "status", "elapsed_seconds"] if k in event}), flush=True)

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.url = f"http://127.0.0.1:{self.server.server_address[1]}"

    def close(self):
        for connection in list(self.connections):
            connection.close()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()


def run(args):
    directory = args.directory.resolve()
    manifest = json.loads((directory / "manifest.json").read_text())
    binary = Path(manifest["binary"])
    script_sha256 = digest(Path(__file__).read_bytes())
    assert digest(binary.read_bytes()) == manifest["binary_sha256"], "Installed binary changed after preparation"
    base = environment_file(args.env_file)
    upstream = base["BATBOX_API_BASE_URL"]
    recorder = Recorder(directory, upstream, args.timeout)
    env = {k: v for k, v in os.environ.items() if not k.startswith("BATBOX_")}
    env.update(base)
    env.update({"BATBOX_API_BASE_URL": recorder.url + "/v1", "BATBOX_CONFIG_DIR": str(directory / "config"),
                "BATBOX_LOG_FILE": str(directory / "native.log"), "BATBOX_CTX_LEN_DEFAULT": "200000",
                "BATBOX_DEFAULT_MODEL": manifest["model"], "BATBOX_MODELS": manifest["model"],
                "BATBOX_MAX_TOKENS": "4096", "BATBOX_REQUEST_TIMEOUT_SEC": str(args.timeout),
                "BATBOX_PROVIDER_HINT": "openai"})
    env.pop("BATBOX_ENABLE_THINKING", None)  # Preserve the production provider default.
    command = [str(binary), "--autonomous", "--env", str(directory / "empty.env"),
               "--engine-state-dir", str(directory / "state"), "--engine-project-root", str(directory / "project"),
               "--engine-letter-file", str(directory / "letters.txt"), "--engine-memory-url", recorder.url,
               "--engine-interval", "1", "--engine-workers", "0", "--engine-max-cycles", "1"]
    original_messages = json.loads((directory / "seed-messages.json").read_text())
    original_project = digest((directory / "project" / "DO_NOT_CHANGE.txt").read_bytes())
    report = {"phase": "running", "binary_sha256": manifest["binary_sha256"], "script_sha256": script_sha256,
              "tokenizer_sha256": manifest["tokenizer_sha256"], "threshold": 100000,
              "context_window": 200000, "thinking_override": None, "final_max_tokens": 4096, "workers": 0,
              "uses_installed_native_binary": True, "inference_is_live": True,
              "memory_is_isolated_stub": True, "replies_buffered_to_reject_tool_calls": True,
              "archive_tokens_cpu": manifest["archive_tokens_without_special_tokens"], "runs": []}
    process = None
    interrupted = None
    previous_handlers = {}
    def interrupt(signum, _frame):
        raise FixtureInterrupted(signum)
    for signum in [signal.SIGINT, signal.SIGTERM]:
        previous_handlers[signum] = signal.signal(signum, interrupt)
    try:
        for phase in [1, 2]:
            recorder.phase = phase
            if phase == 2:
                write_json(directory / "state/inbox/live-100k-restart.json", {
                    "id": "live-100k-restart", "content": "After this process restart, " + QUESTION})
            start = time.monotonic()
            with (directory / f"native-run-{phase}.stdout").open("w") as stdout, (directory / f"native-run-{phase}.stderr").open("w") as stderr:
                process = subprocess.Popen(command, cwd=directory / "project", env=env, stdout=stdout, stderr=stderr,
                                           start_new_session=True)
                while process.poll() is None:
                    if recorder.errors or time.monotonic() - start > args.timeout:
                        os.killpg(process.pid, signal.SIGTERM)
                        try:
                            process.wait(timeout=10)
                        except subprocess.TimeoutExpired:
                            os.killpg(process.pid, signal.SIGKILL)
                        raise RuntimeError("Fixture stopped: upstream/tool error or bounded run timeout")
                    time.sleep(0.5)
            assert process.returncode == 0, f"Native run {phase} exited {process.returncode}"
            state = json.loads((directory / "state/state.json").read_text())
            status = json.loads((directory / "state/status.json").read_text())
            session = json.loads((directory / "state/sessions" / (manifest["session_id"] + ".json")).read_text())
            checkpoints = [m for m in session["messages"] if "context_checkpoint" in m]
            assert session["messages"][:len(original_messages)] == original_messages, "Original transcript was rewritten"
            assert len(checkpoints) == 1, f"Expected one durable checkpoint, got {len(checkpoints)}"
            checkpoint = checkpoints[-1]
            assert checkpoint["prompt_tokens_before"] >= 100000
            assert checkpoint["prompt_tokens_after"] < 100000
            summary = checkpoint["context_checkpoint"][0]["content"]
            assert FACT in summary and CONSTRAINT in summary, "Summary lost the fact or user constraint"
            assert FACT in state["last_response"] and CONSTRAINT in state["last_response"], "Final answer lost the fact or constraint"
            assert state["session_id"] == manifest["session_id"] and state["cycle"] == phase
            assert not (directory / "state/running.json").exists()
            assert digest((directory / "project/DO_NOT_CHANGE.txt").read_bytes()) == original_project
            assert set(p.name for p in (directory / "project").iterdir()) == {"DO_NOT_CHANGE.txt"}
            run_report = {"run": phase, "elapsed_seconds": time.monotonic() - start,
                          "cycle": state["cycle"], "checkpoint_count": len(checkpoints),
                          "tokens_before": checkpoint["prompt_tokens_before"], "tokens_after": checkpoint["prompt_tokens_after"],
                          "restored_active_messages": status["messages"], "last_prompt_tokens": status["last_prompt_tokens"],
                          "summary_sha256": digest(summary.encode()), "summary_preserves_fact_and_constraint": True,
                          "answer_preserves_fact_and_constraint": True, "full_seed_transcript_preserved": True,
                          "project_unchanged": True}
            report["runs"].append(run_report)
            write_json(directory / f"checkpoint-after-run-{phase}.json", checkpoint)
            print(json.dumps(run_report), flush=True)
        normal = [e for e in recorder.events if e["kind"] == "normal"]
        summaries = [e for e in recorder.events if e["kind"] == "summary" and e["path"] == "/v1/chat/completions"]
        assert summaries and all(e["phase"] == 1 for e in summaries), "Restart unexpectedly reran summarization"
        assert len({e["system_sha256"] for e in normal}) == 1, "Stable system prefix changed"
        assert len({e["tools_sha256"] for e in normal}) == 1 and min(e["tool_count"] for e in normal) > 10
        assert all(e["template_options"] is None for e in normal), "Provider thinking default was overridden"
        assert all(not e["has_archive_rows"] for e in normal if e["path"] == "/v1/chat/completions")
        assert all(not e["has_archive_rows"] for e in normal if e["phase"] == 2), "Restart rehydrated full raw history"
        assert any(e.get("count", 0) >= 100000 for e in normal)
        assert all(e.get("max_model_len") == 200000 for e in normal if e["path"] == "/tokenize")
        report.update({"phase": "passed", "summary_model_calls": len(summaries),
                       "tool_count": normal[0]["tool_count"], "stable_system_sha256": normal[0]["system_sha256"],
                       "stable_tools_sha256": normal[0]["tools_sha256"], "restart_used_checkpoint": True,
                       "native_request_count": len(recorder.events)})
    except FixtureInterrupted as exc:
        interrupted = exc
        report.update({"phase": "aborted_by_signal", "reason": str(exc), "retention_or_restart_result": None})
    except Exception as exc:
        report.update({"phase": "failed", "error": str(exc), "upstream_errors": recorder.errors})
        raise
    finally:
        if process is not None and process.poll() is None:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
        recorder.close()
        report["requests"] = recorder.events
        write_json(directory / "sanitized-result.json", report)
        print(json.dumps(report, indent=2), flush=True)
        for signum, handler in previous_handlers.items():
            signal.signal(signum, handler)
    if interrupted:
        raise SystemExit(128 + interrupted.signum)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--run", action="store_true", help="Explicitly enable live inference for a prepared fixture")
    parser.add_argument("--binary", type=Path, default=Path("/home/isnotreal/.local/lib/batbox-engine/current/batbox"))
    parser.add_argument("--tokenizer", type=Path, default=Path("/home/isnotreal/models/gemma4-31b-qat-hauhau-balanced-native-mixed-w4w8a16/tokenizer.json"))
    parser.add_argument("--model", default="qwen3.6-35b-hauhau-w4a16-vision")
    parser.add_argument("--seed-tokens", type=int, default=102000)
    parser.add_argument("--env-file", type=Path, default=Path("/home/isnotreal/.batbox/engine.env"))
    parser.add_argument("--timeout", type=int, default=1800)
    args = parser.parse_args()
    (run if args.run else prepare)(args)


if __name__ == "__main__":
    main()
