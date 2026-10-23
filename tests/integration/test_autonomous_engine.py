"""Lifecycle integration tests; run with BATBOX_AUTONOMOUS_BIN=/path/to/batbox.

Uses HTTP fakes only for inference and the memory queue. Exercises the real
native autonomous executable, full tool schemas, transcript and filesystem IO.
"""
import copy
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time
import unittest
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


ROOT = Path(__file__).resolve().parents[2]
BINARY = Path(os.environ.get("BATBOX_AUTONOMOUS_BIN", ROOT / "build/src/batbox"))


class FakeServices:
    def __init__(self):
        self.requests = []
        self.tokenizations = []
        self.memories = []
        self.fail_memory = False
        self.token_count = None
        self.summary_handler = None
        self.recall_handler = None
        self.memory_recall_tool = False
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def send_json(self, data, status=200):
                payload = json.dumps(data).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                if self.path == "/tokenize":
                    owner.tokenizations.append(body)
                    count = owner.token_count(body) if owner.token_count else 250
                    return self.send_json({"count": count, "max_model_len": 200000, "tokens": []})
                if self.path == "/ingest":
                    return self.send_json({"status": "queued"}, 202)
                if self.path == "/remember":
                    if owner.fail_memory:
                        return self.send_json({"error": "memory offline"}, 503)
                    owner.memories.append(body)
                    return self.send_json({"status": "pending", "id": body.get("id")}, 202)
                if self.path == "/recall":
                    if owner.recall_handler:
                        return self.send_json(owner.recall_handler(body))
                    return self.send_json({"backend": "test", "results": [
                        {"source": "file:///fixture", "content": "A source-backed fixture observation"}]})
                if self.path != "/v1/chat/completions":
                    return self.send_json({"error": "unknown path"}, 404)
                owner.requests.append(body)
                if not body.get("stream"):
                    if owner.summary_handler:
                        owner.summary_handler(body)
                    return self.send_json({"choices": [{"index": 0,
                        "message": {"role": "assistant", "content": "Summary of earlier verified work."},
                        "finish_reason": "stop"}],
                        "usage": {"prompt_tokens": 1000, "completion_tokens": 8, "total_tokens": 1008}})
                chunks = [
                    {"choices": [{"index": 0, "delta": {"role": "assistant", "content": "A durable verified response."},
                                  "finish_reason": None}]},
                    {"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}],
                     "usage": {"prompt_tokens": 250, "completion_tokens": 6, "total_tokens": 256}},
                ]
                if owner.memory_recall_tool and not any(m.get("tool_call_id") == "delayed_recall"
                                                       for m in body["messages"]):
                    chunks = [{"choices": [{"index": 0, "delta": {"role": "assistant", "tool_calls": [{
                        "index": 0, "id": "delayed_recall", "type": "function", "function": {
                            "name": "MemoryRecall", "arguments": json.dumps({"query": "DELAYED_TOOL_RECALL", "limit": 1})}}]},
                        "finish_reason": None}]},
                        {"choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}]}]
                payload = "".join("data: " + json.dumps(chunk) + "\n\n" for chunk in chunks) + "data: [DONE]\n\n"
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Content-Length", str(len(payload.encode())))
                self.end_headers()
                self.wfile.write(payload.encode())

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.url = f"http://127.0.0.1:{self.server.server_address[1]}"

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()


@unittest.skipUnless(BINARY.exists(), "Build native batbox or set BATBOX_AUTONOMOUS_BIN")
class AutonomousLifecycle(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="batbox-engine-test-")
        self.directory = Path(self.temp.name)
        self.state = self.directory / "state"
        self.letters = self.directory / "letters.txt"
        self.letters.write_text("LOVE_LETTER_MARKER: remember the user's mission.")
        self.services = FakeServices()
        self.environment = {key: value for key, value in os.environ.items() if not key.startswith("BATBOX_")}
        self.environment.update({
            "HOME": str(self.directory), "BATBOX_CONFIG_DIR": str(self.directory / "config"),
            "BATBOX_API_BASE_URL": self.services.url + "/v1", "BATBOX_API_KEY": "test-local",
            "BATBOX_DEFAULT_MODEL": "test-model", "BATBOX_MODELS": "test-model",
            "BATBOX_CTX_LEN_DEFAULT": "200000", "BATBOX_MAX_TOKENS": "32",
            "BATBOX_REQUEST_TIMEOUT_SEC": "10", "BATBOX_PROVIDER_HINT": "openai",
            "BATBOX_ENABLE_THINKING": "false",
        })

    def tearDown(self):
        self.services.close()
        self.temp.cleanup()

    def engine_command(self):
        return [
            str(BINARY), "--autonomous", "--engine-state-dir", str(self.state),
            "--engine-letter-file", str(self.letters), "--engine-project-root", str(ROOT),
            "--engine-memory-url", self.services.url, "--engine-max-cycles", "1",
            "--engine-interval", "1", "--engine-workers", "0",
        ]

    def run_engine(self):
        result = subprocess.run(self.engine_command(), env=self.environment,
                                text=True, capture_output=True, timeout=30, cwd=ROOT)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def read_state(self):
        return json.loads((self.state / "state.json").read_text())

    def exercise_delayed_recall(self, tool):
        self.services.memory_recall_tool = tool
        elapsed = []
        def recall(body):
            if tool and body.get("query") != "DELAYED_TOOL_RECALL":
                return {"results": []}
            started = time.monotonic()
            time.sleep(47)  # Deliberately exceeds the former 45-second RPC timeout.
            elapsed.append(time.monotonic() - started)
            return {"results": [{"source": "fixture://delayed-recall",
                                 "content": "DELAYED_RECALL_VERIFIED_RESULT"}]}
        self.services.recall_handler = recall
        result = subprocess.run(self.engine_command(), env=self.environment, text=True,
                                capture_output=True, timeout=65, cwd=ROOT)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        messages = self.services.requests[-1]["messages"]
        self.assertTrue(any("DELAYED_RECALL_VERIFIED_RESULT" in (m.get("content") or "")
                            and (m["role"] == "tool" if tool else m["role"] == "user") for m in messages),
                        "Delayed recall did not reach the native model request after the legacy 45-second limit")
        self.assertEqual(len(elapsed), 1)
        self.assertGreaterEqual(elapsed[0], 47)
        events = [json.loads(line) for line in (self.state / "events.jsonl").read_text().splitlines()]
        self.assertFalse(any(event["event"] == "recall_failed" for event in events))
        self.assertEqual(self.read_state()["cycle"], 1)

    def test_delayed_direct_recall_exceeds_legacy_timeout(self):
        self.exercise_delayed_recall(tool=False)

    def test_delayed_native_memory_recall_tool_exceeds_legacy_timeout(self):
        self.exercise_delayed_recall(tool=True)

    def test_completed_compaction_work_refreshes_marker_before_later_chunks_finish(self):
        self.run_engine()
        state = self.read_state()
        session_path = self.state / "sessions" / (state["session_id"] + ".json")
        session = json.loads(session_path.read_text())
        session["messages"][0]["content"] = "Long verified evidence: " + "x" * 230000
        # Keep the native appendable trailing-array layout while enlarging the
        # restored history enough to require several bounded summary requests.
        header = {key: value for key, value in session.items() if key != "messages"}
        session_path.write_text(json.dumps(header)[:-1] + ',\n"messages": [\n' +
            ",\n".join(json.dumps(message) for message in session["messages"]) + "\n]\n}\n")
        self.services.token_count = lambda body: sum(len(message.get("content") or "")
                                                     for message in body["messages"])
        started = [threading.Event(), threading.Event()]
        release = [threading.Event(), threading.Event()]
        calls = []

        def summary(body):
            index = len(calls)
            calls.append(body)
            if index < 2:
                started[index].set()
                if not release[index].wait(20):
                    raise RuntimeError("Test did not release blocked summary")

        self.services.summary_handler = summary
        events_path = self.state / "events.jsonl"
        before_events = len(events_path.read_text().splitlines())
        process = subprocess.Popen(self.engine_command(), env=self.environment,
                                   text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=ROOT)
        try:
            self.assertTrue(started[0].wait(15), "First summary never reached the local fake")
            marker_path = self.state / "running.json"
            first_marker = json.loads(marker_path.read_text())
            self.assertEqual(first_marker["last_activity"], "context_progress")
            self.assertEqual(first_marker["last_operation"], "compaction_prompt_tokenized")
            pending_events = events_path.read_text()
            pending_marker = marker_path.read_text()
            # No completion has occurred while this HTTP request is blocked.
            # A timer or start notification must not masquerade as progress.
            time.sleep(1.1)
            self.assertEqual(marker_path.read_text(), pending_marker)
            self.assertEqual(events_path.read_text(), pending_events)
            current = [json.loads(line) for line in pending_events.splitlines()[before_events:]]
            self.assertFalse(any(row.get("operation") == "compaction_summary_completed" for row in current))

            release[0].set()
            self.assertTrue(started[1].wait(15), "Second summary never reached the local fake")
            second_marker = json.loads(marker_path.read_text())
            self.assertNotEqual(second_marker["updated_at"], first_marker["updated_at"])
            self.assertEqual(second_marker["last_activity"], "context_progress")
            current = [json.loads(line) for line in events_path.read_text().splitlines()[before_events:]]
            self.assertEqual(sum(row.get("operation") == "compaction_summary_completed" for row in current), 1)
            self.assertEqual(self.read_state()["phase"], "running")
        finally:
            for event in release:
                event.set()
            try:
                stdout, stderr = process.communicate(timeout=30)
            except subprocess.TimeoutExpired:
                process.kill()
                stdout, stderr = process.communicate()
                self.fail("Native engine did not finish after release: " + stdout + stderr)
        self.assertEqual(process.returncode, 0, stdout + stderr)
        current = [json.loads(line) for line in events_path.read_text().splitlines()[before_events:]]
        self.assertGreaterEqual(sum(row.get("operation") == "compaction_summary_completed" for row in current), 3)
        self.assertFalse((self.state / "running.json").exists())

    def test_thinking_uses_provider_default_or_explicit_matching_tokenizer_setting(self):
        self.environment.pop("BATBOX_ENABLE_THINKING")
        self.run_engine()
        self.assertNotIn("chat_template_kwargs", self.services.requests[-1])
        self.assertTrue(self.services.tokenizations)
        for request in self.services.tokenizations:
            self.assertNotIn("chat_template_kwargs", request)
        self.services.tokenizations.clear()
        self.environment["BATBOX_ENABLE_THINKING"] = "true"
        self.run_engine()
        self.assertEqual(self.services.requests[-1]["chat_template_kwargs"], {"enable_thinking": True})
        for request in self.services.tokenizations:
            self.assertEqual(request["chat_template_kwargs"], {"enable_thinking": True})

    def test_restart_keeps_session_stable_prefix_and_full_tools(self):
        self.run_engine()
        first = self.read_state()
        self.run_engine()
        second = self.read_state()
        self.assertEqual(first["session_id"], second["session_id"])
        self.assertEqual(second["cycle"], 2)
        requests = self.services.requests
        self.assertEqual(len(requests), 2)
        self.assertEqual(requests[0]["messages"][0], requests[1]["messages"][0])
        self.assertEqual(sum("LOVE_LETTER_MARKER" in (m.get("content") or "")
                             for m in requests[-1]["messages"]), 1)
        names = {tool["function"]["name"] for tool in requests[0]["tools"]}
        self.assertTrue({"Bash", "Read", "Write", "MemoryRemember", "MemoryRecall",
                         "SearXNGSearch", "ScraplingFetch", "CommunicationControl", "IssueUpdate"} <= names)
        self.assertEqual(requests[0]["chat_template_kwargs"], {"enable_thinking": False})
        self.assertFalse((self.state / "running.json").exists())
        self.assertEqual(len(self.services.memories), 2)
        self.assertEqual(self.services.memories[0]["kind"], "generated_observation")

    def test_inbox_reply_preserves_issue_and_acknowledges_after_save(self):
        (self.state / "inbox").mkdir(parents=True)
        message = {"id": "20261023_test", "issue_id": "issue-42", "content": "USER_INBOX_MARKER", "created_at": "now"}
        (self.state / "inbox/20261023_test.json").write_text(json.dumps(message))
        self.run_engine()
        reply = json.loads((self.state / "outbox/20261023_test.json").read_text())
        self.assertEqual(reply["issue_id"], "issue-42")
        self.assertEqual(reply["content"], "A durable verified response.")
        self.assertEqual(reply["status"], "completed")
        self.assertFalse((self.state / "inbox/20261023_test.json").exists())
        self.assertTrue((self.state / "processed/20261023_test.json").exists())
        self.assertIn("USER_INBOX_MARKER", self.services.requests[0]["messages"][-1]["content"])

    def test_unclean_exit_report_and_durable_background_memory_retry(self):
        self.run_engine()
        (self.state / "running.json").write_text(json.dumps({"pid": 999999, "phase": "running"}))
        (self.state / "crash-report.json").write_text(json.dumps({"reason": "CRASH_FIXTURE_MARKER"}))
        self.services.fail_memory = True
        self.run_engine()
        self.assertEqual(len(list((self.state / "memory-outbox").glob("*.json"))), 1)
        self.assertIn("CRASH_FIXTURE_MARKER", self.services.requests[-1]["messages"][-1]["content"])
        self.services.fail_memory = False
        self.run_engine()
        self.assertEqual(list((self.state / "memory-outbox").glob("*.json")), [])
        self.assertEqual(len(self.services.memories), 3)

    def test_interrupted_tool_call_gets_unknown_outcome_before_resume(self):
        self.run_engine()
        state = self.read_state()
        session_path = self.state / "sessions" / (state["session_id"] + ".json")
        raw_session = session_path.read_text()
        session = json.loads(raw_session)
        interrupted = copy.deepcopy(session["messages"][-1])
        interrupted.update({"id": str(uuid.uuid4()), "content": "", "tool_calls": [
            {"id": "interrupted_call", "name": "Write", "arguments": {"path": "/tmp/example", "content": "x"}}]})
        # Preserve SessionFile's appendable trailing-array wire layout.
        closing = raw_session.rfind("]\n}")
        self.assertGreater(closing, 0)
        session_path.write_text(raw_session[:closing].rstrip() + ",\n" + json.dumps(interrupted) + "\n]\n}")
        state["phase"] = "running"
        (self.state / "state.json").write_text(json.dumps(state))
        self.run_engine()
        messages = self.services.requests[-1]["messages"]
        errors = [m for m in messages if m.get("tool_call_id") == "interrupted_call"]
        self.assertEqual(len(errors), 1)
        self.assertIn("Outcome unknown", errors[0]["content"])
        self.assertEqual(self.read_state()["cycle"], 2)

    def test_bad_inbox_is_quarantined_without_starving_valid_message(self):
        (self.state / "inbox").mkdir(parents=True)
        (self.state / "inbox/000_bad.json").write_text("not json")
        (self.state / "inbox/001_valid.json").write_text(json.dumps({
            "id": "001_valid", "content": "VALID_MESSAGE_AFTER_CORRUPTION"}))
        self.run_engine()
        self.assertTrue((self.state / "rejected/000_bad.json").exists())
        self.assertTrue((self.state / "rejected/000_bad.json.error").exists())
        self.assertTrue((self.state / "outbox/001_valid.json").exists())

    def test_completed_tail_survives_crash_before_reply_checkpoint(self):
        self.run_engine()
        state = self.read_state()
        state["phase"] = "running"
        state["cycle"] = 0  # crash after durable final assistant, before cycle commit
        state["inbox"] = {"id": "crashed_reply", "issue_id": "issue-crash", "content": "original question"}
        (self.state / "state.json").write_text(json.dumps(state))
        (self.state / "running.json").write_text(json.dumps({"pid": 999999, "phase": "running"}))
        before = len(self.services.requests)
        self.run_engine()
        self.assertEqual(len(self.services.requests), before, "Must not rerun a completed action")
        reply = json.loads((self.state / "outbox/crashed_reply.json").read_text())
        self.assertEqual(reply["content"], "A durable verified response.")
        self.assertIn("crash_report", self.read_state(), "Recovery must wait until completed reply is durable")

    def test_supervisor_report_after_clean_signal_is_delivered_once(self):
        self.run_engine()
        self.assertFalse((self.state / "running.json").exists())
        (self.state / "crash-report.json").write_text(json.dumps({"reason": "WATCHDOG_MARKER"}))
        self.run_engine()
        self.assertIn("WATCHDOG_MARKER", self.services.requests[-1]["messages"][-1]["content"])
        self.run_engine()
        self.assertEqual(sum("WATCHDOG_MARKER" in (m.get("content") or "")
                             for m in self.services.requests[-1]["messages"]), 1)


if __name__ == "__main__":
    unittest.main()
