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
                    return self.send_json({"count": 250, "max_model_len": 200000, "tokens": []})
                if self.path == "/ingest":
                    return self.send_json({"status": "queued"}, 202)
                if self.path == "/remember":
                    if owner.fail_memory:
                        return self.send_json({"error": "memory offline"}, 503)
                    owner.memories.append(body)
                    return self.send_json({"status": "pending", "id": body.get("id")}, 202)
                if self.path == "/recall":
                    return self.send_json({"backend": "test", "results": [
                        {"source": "file:///fixture", "content": "A source-backed fixture observation"}]})
                if self.path != "/v1/chat/completions":
                    return self.send_json({"error": "unknown path"}, 404)
                owner.requests.append(body)
                chunks = [
                    {"choices": [{"index": 0, "delta": {"role": "assistant", "content": "A durable verified response."},
                                  "finish_reason": None}]},
                    {"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}],
                     "usage": {"prompt_tokens": 250, "completion_tokens": 6, "total_tokens": 256}},
                ]
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

    def run_engine(self):
        result = subprocess.run([
            str(BINARY), "--autonomous", "--engine-state-dir", str(self.state),
            "--engine-letter-file", str(self.letters), "--engine-project-root", str(ROOT),
            "--engine-memory-url", self.services.url, "--engine-max-cycles", "1",
            "--engine-interval", "1", "--engine-workers", "0",
        ], env=self.environment, text=True, capture_output=True, timeout=30, cwd=ROOT)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def read_state(self):
        return json.loads((self.state / "state.json").read_text())

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
