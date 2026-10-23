"""Real native worker tools, four concurrent HTTP calls and durable recovery.

Only the model and memory HTTP services are fakes. All worker threads,
permissions, tools, files, tokenization and journals use the production binary.
"""
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


ROOT = Path(__file__).resolve().parents[2]
BINARY = Path(os.environ.get("BATBOX_AUTONOMOUS_BIN", ROOT / "build/src/batbox"))


class WorkerBackend:
    def __init__(self, directory):
        self.directory = directory
        self.requests = []
        self.tokenizations = []
        self.memories = []
        self.condition = threading.Condition()
        self.players = set()
        self.active = 0
        self.peak = 0
        self.errors = []
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def send_json(self, payload, status=200):
                raw = json.dumps(payload).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)

            def send_stream(self, delta, finish="stop"):
                chunks = [
                    {"choices": [{"index": 0, "delta": delta, "finish_reason": None}]},
                    {"choices": [{"index": 0, "delta": {}, "finish_reason": finish}],
                     "usage": {"prompt_tokens": 512, "completion_tokens": 16, "total_tokens": 528}},
                ]
                raw = ("".join("data: " + json.dumps(c) + "\n\n" for c in chunks) + "data: [DONE]\n\n").encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                if self.path == "/tokenize":
                    owner.tokenizations.append(body)
                    return self.send_json({"count": 512, "max_model_len": 200000, "tokens": []})
                if self.path == "/recall":
                    return self.send_json({"results": []})
                if self.path in ("/remember", "/ingest"):
                    if self.path == "/remember": owner.memories.append(body)
                    return self.send_json({"status": "queued"}, 202)
                if self.path != "/v1/chat/completions":
                    return self.send_json({"error": "unexpected path"}, 404)
                text = "\n".join(m.get("content") or "" for m in body["messages"] if m["role"] == "user")
                match = re.search(r"Your durable agent handle is ([a-f0-9-]+)", text)
                player = match[1] if match else "coordinator"
                with owner.condition:
                    owner.requests.append((player, body))
                    owner.players.add(player)
                    owner.active += 1
                    owner.peak = max(owner.peak, owner.active)
                    owner.condition.notify_all()
                    ready = owner.condition.wait_for(lambda: len(owner.players) == 4, timeout=12)
                try:
                    if not ready:
                        owner.errors.append("Four native conversations did not reach the backend concurrently")
                        return self.send_json({"error": owner.errors[-1]}, 503)
                    has_tool_result = any(m["role"] == "tool" for m in body["messages"])
                    if player != "coordinator" and not has_tool_result:
                        return self.send_stream({"role": "assistant", "tool_calls": [{
                            "index": 0, "id": "write_" + player, "type": "function",
                            "function": {"name": "Write", "arguments": json.dumps({
                                "path": str(owner.directory / (player + ".proof")),
                                "content": "executed by native worker " + player})}}]}, "tool_calls")
                    has_memory_result = any(m.get("tool_call_id") == "remember_" + player for m in body["messages"])
                    if player != "coordinator" and not has_memory_result:
                        return self.send_stream({"role": "assistant", "tool_calls": [{
                            "index": 0, "id": "remember_" + player, "type": "function",
                            "function": {"name": "MemoryRemember", "arguments": json.dumps({
                                "content": "Worker verified original evidence.",
                                "source": "https://evidence.example/" + player,
                                "metadata": {"verification": "checked fixture"},
                                "captured_at": "2026-10-23T00:00:00Z"})}}]}, "tool_calls")
                    if player == "coordinator":
                        # Do not let the coordinator shut down the process until
                        # all worker results have reached their own checkpoints.
                        deadline = time.monotonic() + 12
                        while time.monotonic() < deadline:
                            paths = list((owner.directory / "state/workers").glob("*.json"))
                            completed = [json.loads(p.read_text()).get("completed_turns", 0) for p in paths]
                            if len(completed) == 3 and min(completed) >= owner.required_turns:
                                break
                            time.sleep(0.02)
                        else:
                            owner.errors.append("Workers failed to durably finish their native tools")
                    return self.send_stream({"role": "assistant", "content": "Verified native result for " + player})
                finally:
                    with owner.condition:
                        owner.active -= 1

        self.required_turns = 1
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.url = "http://127.0.0.1:" + str(self.server.server_address[1])

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()


@unittest.skipUnless(BINARY.exists(), "Build native batbox first")
class NativeWorkers(unittest.TestCase):
    def test_four_connections_tools_common_prefix_and_durable_inbox_restart(self):
        with tempfile.TemporaryDirectory(prefix="batbox-native-workers-") as temp:
            directory = Path(temp)
            backend = WorkerBackend(directory)
            self.addCleanup(backend.close)
            state = directory / "state"
            env = {k: v for k, v in os.environ.items() if not k.startswith("BATBOX_")}
            env.update({"HOME": str(directory), "BATBOX_CONFIG_DIR": str(directory / "config"),
                "BATBOX_API_BASE_URL": backend.url + "/v1", "BATBOX_API_KEY": "local-test",
                "BATBOX_DEFAULT_MODEL": "local-test", "BATBOX_MODELS": "local-test",
                "BATBOX_REQUEST_TIMEOUT_SEC": "20", "BATBOX_MAX_TOKENS": "64",
                "BATBOX_CTX_LEN_DEFAULT": "200000",
                "BATBOX_ENABLE_THINKING": "false",
                "BATBOX_TEMPERATURE": "0.6", "BATBOX_TOP_P": "0.9",
                "BATBOX_PROVIDER_HINT": "openai"})
            command = [str(BINARY), "--autonomous", "--engine-state-dir", str(state),
                "--engine-project-root", str(directory), "--engine-memory-url", backend.url,
                "--engine-interval", "1", "--engine-workers", "3", "--engine-max-cycles", "1",
                "--prompt", "Verify the isolated native worker fixture; do not change other files."]

            def run():
                process = subprocess.run(command, env=env, cwd=ROOT, text=True,
                    capture_output=True, timeout=35)
                self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
                self.assertFalse(backend.errors, backend.errors)

            run()
            self.assertGreaterEqual(backend.peak, 4)
            manifests = {p.stem: json.loads(p.read_text()) for p in (state / "workers").glob("*.json")}
            self.assertEqual(len(manifests), 3)
            for agent_id, manifest in manifests.items():
                self.assertEqual((directory / (agent_id + ".proof")).read_text(), "executed by native worker " + agent_id)
                self.assertTrue(manifest["recoverable"])
                self.assertTrue(manifest["continuous"])
                self.assertEqual(manifest["prompt_tokens"], 512)
                self.assertEqual(manifest["exact_compaction_tokens"], 100000)
                observed = [m for m in backend.memories if m.get("source") == "https://evidence.example/" + agent_id]
                self.assertEqual(len(observed), 1)
                self.assertEqual(observed[0]["kind"], "generated_observation")
                self.assertEqual(observed[0]["metadata"]["verification"], "checked fixture")
                self.assertEqual(observed[0]["metadata"]["recorded_in"], "transcript://batbox/" + manifest["session_id"])
                self.assertEqual(observed[0]["captured_at"], "2026-10-23T00:00:00Z")
            first_requests = backend.requests[:]
            common = first_requests[0][1]["messages"][0]
            tools = first_requests[0][1]["tools"]
            names = {t["function"]["name"] for t in tools}
            self.assertTrue({"Bash", "Write", "Task", "MemoryRecall", "ScraplingFetch", "IssueUpdate"} <= names)
            for player, request in first_requests:
                self.assertEqual(request["messages"][0], common)
                self.assertEqual(request["tools"], tools)
                self.assertEqual(request["chat_template_kwargs"], {"enable_thinking": False})
                self.assertEqual(request["temperature"], 0.6)
                self.assertEqual(request["top_p"], 0.9)
            self.assertGreaterEqual(len(backend.tokenizations), len(first_requests))

            # Acknowledged but unconsumed peer messages survive restart. Stable
            # agent/session IDs and recorded tool results survive with them.
            receiver = next(iter(manifests))
            pending = "[worker-message:restart-fixture]\nDURABLE_PEER_MESSAGE_MARKER"
            manifest = manifests[receiver]
            manifest["inbox"].append(pending)
            (state / "workers" / (receiver + ".json")).write_text(json.dumps(manifest))
            proof_mtimes = {p.name: p.stat().st_mtime_ns for p in directory.glob("*.proof")}
            with backend.condition:
                backend.players.clear()
                backend.required_turns = 2
            before = len(backend.requests)
            run()
            after = {p.stem: json.loads(p.read_text()) for p in (state / "workers").glob("*.json")}
            self.assertEqual(set(after), set(manifests))
            for agent_id in manifests:
                self.assertEqual(after[agent_id]["session_id"], manifests[agent_id]["session_id"])
                self.assertGreaterEqual(after[agent_id]["completed_turns"], 2)
            self.assertEqual(after[receiver]["inbox"], [])
            self.assertEqual({p.name: p.stat().st_mtime_ns for p in directory.glob("*.proof")}, proof_mtimes,
                "Previously recorded tool calls must not be replayed on restart")
            receiver_turns = [r for player, r in backend.requests[before:] if player == receiver]
            self.assertTrue(receiver_turns)
            for _, request in backend.requests[before:]:
                self.assertEqual(request["messages"][0], common)
                self.assertEqual(request["tools"], tools)
                self.assertEqual(request["chat_template_kwargs"], {"enable_thinking": False})
            self.assertEqual(sum("DURABLE_PEER_MESSAGE_MARKER" in (m.get("content") or "")
                for m in receiver_turns[0]["messages"]), 1)


if __name__ == "__main__":
    unittest.main()
