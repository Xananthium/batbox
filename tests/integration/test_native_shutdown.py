"""Native SIGTERM during active worker HTTP; uses loopback fixtures only."""
import gzip
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


ROOT = Path(__file__).resolve().parents[2]
BINARY = Path(os.environ.get("BATBOX_AUTONOMOUS_BIN", ROOT / "build/src/batbox"))


class HangingServices:
    def __init__(self, directory, mode):
        self.directory, self.mode = directory, mode
        self.condition = threading.Condition()
        self.held = set()
        self.restarted = set()
        self.release = threading.Event()
        self.errors = []
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def reply(self, payload, content_type="application/json", status=200):
                raw = payload if isinstance(payload, bytes) else json.dumps(payload).encode()
                self.send_response(status)
                self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)

            def stream(self, delta, finish="stop"):
                chunks = [{"choices": [{"index": 0, "delta": delta, "finish_reason": None}]},
                    {"choices": [{"index": 0, "delta": {}, "finish_reason": finish}],
                     "usage": {"prompt_tokens": 512, "completion_tokens": 8, "total_tokens": 520}}]
                raw = ("".join("data: " + json.dumps(c) + "\n\n" for c in chunks) + "data: [DONE]\n\n").encode()
                self.reply(raw, "text/event-stream")

            def hang(self, player):
                with owner.condition:
                    owner.held.add(player)
                    owner.condition.notify_all()
                # The client must abort an established connection even when
                # no response headers or bytes arrive from the server.
                owner.release.wait(30)

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                text = "\n".join(m.get("content") or "" for m in body.get("messages", []))
                match = re.search(r"Your durable agent handle is ([a-f0-9-]+)", text)
                player = match[1] if match else "coordinator"
                summary = "precise conversation summariser" in text
                if self.path == "/tokenize":
                    # Force the real compactor without allocating a huge test
                    # transcript. This is cancellation coverage, not model QA.
                    count = 100100 if owner.mode == "summary" and player != "coordinator" and not summary else 512
                    return self.reply({"count": count, "max_model_len": 200000, "tokens": []})
                if self.path == "/recall":
                    return self.reply({"results": []})
                if self.path == "/remember" and owner.mode == "service":
                    return self.hang(body["content"])
                if self.path in ("/remember", "/ingest"):
                    return self.reply({"status": "queued"}, status=202)
                if self.path != "/v1/chat/completions":
                    return self.reply({"error": "Unexpected path"}, status=404)
                if owner.mode:
                    if owner.mode == "service" and player != "coordinator":
                        return self.stream({"role": "assistant", "tool_calls": [{
                            "index": 0, "id": "remember_" + player, "type": "function",
                            "function": {"name": "MemoryRemember", "arguments": json.dumps({
                                "content": player, "source": "fixture://shutdown"})}}]}, "tool_calls")
                    return self.hang(player)
                with owner.condition:
                    owner.restarted.add(player)
                    owner.condition.notify_all()
                if player == "coordinator":
                    # Wait for durable worker completion before letting the
                    # coordinator's max-cycles stop the restarted process.
                    deadline = time.monotonic() + 8
                    while time.monotonic() < deadline:
                        manifests = [json.loads(p.read_text()) for p in (owner.directory / "state/workers").glob("*.json")]
                        if len(manifests) == 3 and all(m.get("completed_turns", 0) >= 1 for m in manifests):
                            break
                        time.sleep(.02)
                    else:
                        owner.errors.append("Restarted workers did not durably finish")
                return self.stream({"role": "assistant", "content": "Completed local shutdown fixture."})

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.url = f"http://127.0.0.1:{self.server.server_address[1]}"

    def close(self):
        self.release.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()


def transcript(directory, session_id):
    path = directory / "state/workers/sessions" / (session_id + ".json")
    raw = path.read_bytes() if path.exists() else gzip.decompress(path.with_suffix(".json.gz").read_bytes())
    return json.loads(raw)["messages"]


@unittest.skipUnless(BINARY.exists(), "Build native batbox or set BATBOX_AUTONOMOUS_BIN")
class NativeShutdown(unittest.TestCase):
    def exercise(self, mode):
        with tempfile.TemporaryDirectory(prefix="batbox-shutdown-") as temporary:
            directory = Path(temporary)
            (directory / "empty.env").write_text("")
            (directory / "letters.txt").write_text("")
            backend = HangingServices(directory, mode)
            env = {k: v for k, v in os.environ.items() if not k.startswith("BATBOX_")}
            env.update({"BATBOX_CONFIG_DIR": str(directory / "config"),
                "BATBOX_LOG_FILE": str(directory / "native.log"),
                "BATBOX_API_BASE_URL": backend.url + "/v1", "BATBOX_API_KEY": "local-test",
                "BATBOX_DEFAULT_MODEL": "local-test", "BATBOX_MODELS": "local-test",
                "BATBOX_REQUEST_TIMEOUT_SEC": "120", "BATBOX_MAX_TOKENS": "64",
                "BATBOX_CTX_LEN_DEFAULT": "200000", "BATBOX_PROVIDER_HINT": "openai"})
            command = [str(BINARY), "--autonomous", "--env", str(directory / "empty.env"),
                "--engine-state-dir", str(directory / "state"), "--engine-project-root", str(directory),
                "--engine-letter-file", str(directory / "letters.txt"), "--engine-memory-url", backend.url,
                "--engine-workers", "3", "--engine-interval", "1", "--engine-max-cycles", "1",
                "--prompt", "Exercise the local shutdown fixture."]
            process = None
            try:
                with (directory / "stdout").open("w") as out, (directory / "stderr").open("w") as err:
                    process = subprocess.Popen(command, cwd=directory, env=env, stdout=out, stderr=err)
                    with backend.condition:
                        ready = backend.condition.wait_for(lambda: len(backend.held) == 4, timeout=12)
                    self.assertTrue(ready, f"{mode}: four native conversations did not reach the blocking fixture")
                    started = time.monotonic()
                    process.send_signal(signal.SIGTERM)
                    try:
                        code = process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        self.fail(f"{mode}: SIGTERM failed to interrupt worker HTTP within 5 seconds")
                    self.assertEqual(code, 0)
                    self.assertLess(time.monotonic() - started, 5)
                    self.assertFalse((directory / "state/running.json").exists())
                    self.assertEqual(json.loads((directory / "state/status.json").read_text())["phase"], "stopped")
                    manifests = {p.stem: json.loads(p.read_text()) for p in (directory / "state/workers").glob("*.json")}
                    self.assertEqual(len(manifests), 3)
                    for manifest in manifests.values():
                        self.assertEqual(manifest["status"], "interrupted")
                        self.assertTrue(manifest["recoverable"])
                    original = {k: transcript(directory, m["session_id"]) for k, m in manifests.items()}

                    backend.mode = None
                    process = subprocess.Popen(command, cwd=directory, env=env, stdout=out, stderr=err)
                    self.assertEqual(process.wait(timeout=15), 0)
                    self.assertFalse(backend.errors, backend.errors)
                    restored = {p.stem: json.loads(p.read_text()) for p in (directory / "state/workers").glob("*.json")}
                    self.assertEqual(set(restored), set(manifests))
                    self.assertEqual(backend.restarted, set(manifests) | {"coordinator"})
                    for key, manifest in restored.items():
                        self.assertEqual(manifest["session_id"], manifests[key]["session_id"])
                        self.assertTrue(manifest["recoverable"])
                        self.assertGreaterEqual(manifest["completed_turns"], 1)
                        self.assertEqual(transcript(directory, manifest["session_id"])[:len(original[key])], original[key])
            finally:
                if process is not None and process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
                backend.close()

    def test_streaming_requests_cancel_and_worker_identity_survives(self):
        self.exercise("stream")

    def test_summary_requests_cancel_and_worker_identity_survives(self):
        self.exercise("summary")

    def test_service_requests_cancel_and_worker_identity_survives(self):
        self.exercise("service")


if __name__ == "__main__":
    unittest.main()
