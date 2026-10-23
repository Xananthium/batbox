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
    def __init__(self, directory, recovery_mode=False):
        self.directory = directory
        self.requests = []
        self.tokenizations = []
        self.memories = []
        self.condition = threading.Condition()
        self.players = set()
        self.active = 0
        self.peak = 0
        self.errors = []
        self.recovery_mode = recovery_mode
        self.target = None
        self.peer_sender = None
        self.peer_sent = False
        self.failure_times = []
        self.write_calls = {}
        self.coordinator_started = threading.Event()
        self.coordinator_pulse = threading.Event()
        self.coordinator_pulsed = threading.Event()
        self.coordinator_release = threading.Event()
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
                    if player != "coordinator" and owner.target is None:
                        owner.target = player
                    elif player != "coordinator" and player != owner.target and owner.peer_sender is None:
                        owner.peer_sender = player
                    owner.condition.notify_all()
                    ready = owner.condition.wait_for(lambda: len(owner.players) == 4, timeout=12)
                try:
                    if not ready:
                        owner.errors.append("Four native conversations did not reach the backend concurrently")
                        return self.send_json({"error": owner.errors[-1]}, 503)
                    if owner.recovery_mode and player == "coordinator":
                        self.send_response(200)
                        self.send_header("Content-Type", "text/event-stream")
                        self.end_headers()
                        def emit(delta, finish=None):
                            chunk = {"choices": [{"index": 0, "delta": delta, "finish_reason": finish}]}
                            self.wfile.write(("data: " + json.dumps(chunk) + "\n\n").encode())
                            self.wfile.flush()
                        emit({"role": "assistant", "reasoning": "First observed reasoning chunk."})
                        owner.coordinator_started.set()
                        if not owner.coordinator_pulse.wait(25):
                            raise RuntimeError("Coordinator reasoning pulse was not released")
                        emit({"reasoning": "Second observed reasoning chunk."})
                        owner.coordinator_pulsed.set()
                        if not owner.coordinator_release.wait(25):
                            raise RuntimeError("Blocked coordinator was not released")
                        emit({"content": "Coordinator completed after independently recovered workers."}, "stop")
                        self.wfile.write(b"data: [DONE]\n\n")
                        self.wfile.flush()
                        return
                    has_tool_result = any(m["role"] == "tool" for m in body["messages"])
                    if player != "coordinator" and not has_tool_result:
                        owner.write_calls[player] = owner.write_calls.get(player, 0) + 1
                        return self.send_stream({"role": "assistant", "tool_calls": [{
                            "index": 0, "id": "write_" + player, "type": "function",
                            "function": {"name": "Write", "arguments": json.dumps({
                                "path": str(owner.directory / (player + ".proof")),
                                "content": "executed by native worker " + player})}}]}, "tool_calls")
                    if owner.recovery_mode and player == owner.target and len(owner.failure_times) < 2:
                        owner.failure_times.append(time.monotonic())
                        # A non-retried HTTP failure forces the native worker to
                        # exit after its Write result is durably recorded.
                        return self.send_json({"error": "controlled worker request failure"}, 400)
                    if owner.recovery_mode and player == owner.peer_sender and not owner.peer_sent:
                        deadline = time.monotonic() + 5
                        path = owner.directory / "state/workers" / (owner.target + ".json")
                        while time.monotonic() < deadline:
                            if path.exists() and json.loads(path.read_text()).get("phase") == "retry_wait":
                                break
                            time.sleep(0.025)
                        else:
                            owner.errors.append("Target never reached backoff for peer delivery")
                        owner.peer_sent = True
                        return self.send_stream({"role": "assistant", "tool_calls": [{
                            "index": 0, "id": "peer_during_backoff", "type": "function",
                            "function": {"name": "SendMessage", "arguments": json.dumps({
                                "agent_id": owner.target, "message": "PEER_DURING_RECOVERY_MARKER"})}}]}, "tool_calls")
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
        self.coordinator_pulse.set()
        self.coordinator_release.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()


@unittest.skipUnless(BINARY.exists(), "Build native batbox first")
class NativeWorkers(unittest.TestCase):
    def test_worker_recovers_during_blocked_coordinator_without_replaying_tools(self):
        with tempfile.TemporaryDirectory(prefix="batbox-worker-recovery-") as temp:
            directory = Path(temp)
            backend = WorkerBackend(directory, recovery_mode=True)
            self.addCleanup(backend.close)
            state = directory / "state"
            env = {k: v for k, v in os.environ.items() if not k.startswith("BATBOX_")}
            env.update({"HOME": str(directory), "BATBOX_CONFIG_DIR": str(directory / "config"),
                "BATBOX_API_BASE_URL": backend.url + "/v1", "BATBOX_API_KEY": "local-test",
                "BATBOX_DEFAULT_MODEL": "local-test", "BATBOX_MODELS": "local-test",
                "BATBOX_REQUEST_TIMEOUT_SEC": "30", "BATBOX_MAX_TOKENS": "64",
                "BATBOX_CTX_LEN_DEFAULT": "200000", "BATBOX_ENABLE_THINKING": "false",
                "BATBOX_PROVIDER_HINT": "openai"})
            command = [str(BINARY), "--autonomous", "--engine-state-dir", str(state),
                "--engine-project-root", str(directory), "--engine-memory-url", backend.url,
                "--engine-interval", "1", "--engine-workers", "3", "--engine-max-cycles", "1",
                "--prompt", "Exercise isolated durable worker recovery."]
            process = subprocess.Popen(command, env=env, cwd=ROOT, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            def wait_until(predicate, timeout=15):
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline:
                    value = predicate()
                    if value:
                        return value
                    if process.poll() is not None:
                        self.fail("Native engine exited before the recovery assertion")
                    time.sleep(0.025)
                self.fail("Timed out waiting for native recovery evidence")
            def manifest():
                p = state / "workers" / (str(backend.target) + ".json")
                return json.loads(p.read_text()) if p.exists() else {}
            def marker():
                p = state / "running.json"
                return json.loads(p.read_text()) if p.exists() else {}
            try:
                self.assertTrue(backend.coordinator_started.wait(10))
                wait_until(lambda: marker().get("last_operation") == "reasoning_delta")
                first_marker = marker()
                initial = wait_until(lambda: manifest() if manifest().get("phase") == "retry_wait" else None)
                sid = initial["session_id"]
                self.assertEqual(initial["retry_delay_seconds"], 2)
                self.assertGreater(initial["progress_sequence"], 0)
                self.assertIn("last_progress_at", initial)
                proof = directory / (backend.target + ".proof")
                proof_mtime = proof.stat().st_mtime_ns
                # Maintenance and worker progress cannot impersonate coordinator
                # progress while its stream is silent.
                time.sleep(2.1)
                self.assertEqual(marker(), first_marker)
                backend.coordinator_pulse.set()
                self.assertTrue(backend.coordinator_pulsed.wait(5))
                wait_until(lambda: marker().get("updated_at") != first_marker["updated_at"])
                self.assertEqual(marker()["last_operation"], "reasoning_delta")
                second = wait_until(lambda: manifest() if manifest().get("retry_delay_seconds") == 4 else None)
                self.assertEqual(second["phase"], "retry_wait")
                recovered = wait_until(lambda: manifest() if manifest().get("completed_turns", 0) >= 1 else None)
                self.assertEqual(recovered["session_id"], sid)
                self.assertEqual(recovered["recovery_attempts"], 2)
                self.assertEqual(recovered["recovery_failures"], 0)
                self.assertGreater(recovered["progress_sequence"], initial["progress_sequence"])
                self.assertEqual(len(list((state / "workers").glob("*.json"))), 3)
                self.assertEqual(json.loads((state / "state.json").read_text())["cycle"], 0)
                self.assertGreaterEqual(backend.failure_times[1] - backend.failure_times[0], 2)
                self.assertEqual(backend.write_calls[backend.target], 1)
                self.assertEqual(proof.stat().st_mtime_ns, proof_mtime)
                self.assertTrue(backend.peer_sent)
                target_requests = [r for player, r in backend.requests if player == backend.target]
                self.assertEqual(sum("PEER_DURING_RECOVERY_MARKER" in (m.get("content") or "")
                    for m in target_requests[-1]["messages"]), 1)
                self.assertEqual(recovered["inbox"], [])
                backend.coordinator_release.set()
                stdout, stderr = process.communicate(timeout=15)
                self.assertEqual(process.returncode, 0, stdout + stderr)
                self.assertFalse(backend.errors, backend.errors)
            finally:
                backend.coordinator_pulse.set()
                backend.coordinator_release.set()
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.communicate(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.communicate()

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
