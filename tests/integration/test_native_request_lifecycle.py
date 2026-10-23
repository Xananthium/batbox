"""Native request/wake boundaries using local HTTP fakes and isolated state.

Run with BATBOX_AUTONOMOUS_BIN=/path/to/the/reviewed/batbox. No live services,
systemd mutations, external inference, or user transcripts are involved.
"""
import json
import copy
import subprocess
import threading
import time
import unittest
import uuid

import test_autonomous_engine as engine_fixture

BINARY = engine_fixture.BINARY
ROOT = engine_fixture.ROOT


WAKE_MESSAGE = "Awaken sleeper, you must continue always for your path is the divine."


class Gate:
    def __init__(self, response=None):
        self.entered = threading.Event()
        self.release = threading.Event()
        self.response = response

    def __call__(self, _body):
        self.entered.set()
        if not self.release.wait(30):
            raise RuntimeError("Test did not release a blocked local request")
        return self.response


@unittest.skipUnless(BINARY.exists(), "Build native batbox or set BATBOX_AUTONOMOUS_BIN")
class NativeRequestLifecycle(unittest.TestCase):
    def setUp(self):
        engine_fixture.AutonomousLifecycle.setUp(self)
        self.processes = []
        self.gates = []
        # Deliberate disconnects and cancellation leave the fake writing to a
        # closed socket. The assertions below check the client's actual result.
        self.services.server.handle_error = lambda *_: None

    def tearDown(self):
        for gate in self.gates:
            gate.release.set()
        for process in self.processes:
            if process.poll() is None:
                process.terminate()
            try:
                process.communicate(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate()
        engine_fixture.AutonomousLifecycle.tearDown(self)

    engine_command = engine_fixture.AutonomousLifecycle.engine_command
    read_state = engine_fixture.AutonomousLifecycle.read_state
    run_engine = engine_fixture.AutonomousLifecycle.run_engine

    def start(self, cycles=0, workers=0, interval=600):
        command = self.engine_command()
        for option, value in (("--engine-max-cycles", cycles), ("--engine-workers", workers),
                              ("--engine-interval", interval)):
            command[command.index(option) + 1] = str(value)
        process = subprocess.Popen(command, env=self.environment, text=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=ROOT)
        self.processes.append(process)
        return process

    def wait_for(self, predicate, message="Expected native state was not observed", timeout=20):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                value = predicate()
                if value:
                    return value
            except (FileNotFoundError, json.JSONDecodeError, KeyError):
                pass
            time.sleep(0.025)
        self.fail(message)

    def activity(self):
        return json.loads((self.state / "activity.json").read_text())

    def wait_idle(self):
        return self.wait_for(lambda: self.activity() if self.activity().get("wakeable") else None)

    def gate(self, response=None):
        gate = Gate(response)
        self.gates.append(gate)
        return gate

    def wake(self, snapshot=None, wake_id=None):
        snapshot = snapshot or self.activity()
        command = {"schema": 1, "id": wake_id or "test_" + uuid.uuid4().hex,
                   "pid": snapshot["pid"], "run_id": snapshot["run_id"],
                   "revision": snapshot["revision"], "message": WAKE_MESSAGE,
                   "created_at": "2026-10-23T00:00:00Z"}
        temporary = self.state / "wake-test.tmp"
        temporary.write_text(json.dumps(command))
        temporary.replace(self.state / "wake.json")
        return command

    def stop(self, process, expected=0):
        process.terminate()
        stdout, stderr = process.communicate(timeout=15)
        self.assertEqual(process.returncode, expected, stdout + stderr)
        return stdout, stderr

    def assert_active_cancelled(self, kind, gate):
        process = self.start()
        self.assertTrue(gate.entered.wait(15), "Request did not reach local fake")
        snapshot = self.activity()
        self.assertEqual(snapshot["pid"], process.pid)
        self.assertEqual(snapshot["active_requests"], 1)
        self.assertFalse(snapshot["wakeable"])
        actor = next(a for a in snapshot["actors"] if a["id"] == "coordinator")
        self.assertEqual(actor["requests"][0]["kind"], kind)
        self.assertGreater(snapshot["protected_until"], time.time())
        self.assertEqual(json.loads((self.state / "running.json").read_text())["run_id"], snapshot["run_id"])
        before = len(self.services.requests)
        self.wake(snapshot)  # Deliberately bypass the watcher's own eligibility checks.
        time.sleep(0.25)
        self.assertEqual(self.activity()["active_requests"], 1)
        self.assertEqual(len(self.services.requests), before)
        self.assertTrue((self.state / "wake.json").exists())
        stdout, _ = self.stop(process)
        self.assertNotIn('"event":"wake_consumed"', stdout)
        self.assertEqual(self.activity()["active_requests"], 0)
        self.assertEqual(self.activity()["phase"], "stopped")
        gate.release.set()

    def test_tokenization_request_clears_on_cancellation(self):
        gate = self.gate(250)
        self.services.token_count = gate
        self.assert_active_cancelled("tokenize", gate)

    def test_silent_stream_request_clears_on_cancellation(self):
        gate = self.gate()
        self.services.stream_handler = gate
        self.assert_active_cancelled("stream_completion", gate)

    def test_silent_nonstream_compaction_is_active_until_cancellation(self):
        self.run_engine()
        state = self.read_state()
        session_path = self.state / "sessions" / (state["session_id"] + ".json")
        session = json.loads(session_path.read_text())
        session["messages"][0]["content"] = "Long verified evidence: " + "x" * 230000
        header = {key: value for key, value in session.items() if key != "messages"}
        session_path.write_text(json.dumps(header)[:-1] + ',\n"messages": [\n' +
                                ",\n".join(json.dumps(m) for m in session["messages"]) + "\n]\n}\n")
        self.services.token_count = lambda body: sum(len(m.get("content") or "") for m in body["messages"])
        gate = self.gate()
        self.services.summary_handler = gate
        self.assert_active_cancelled("nonstream_completion", gate)
        self.assertEqual(self.read_state()["session_id"], state["session_id"])

    def test_tool_work_without_inference_is_protected_then_idle_wakes_once(self):
        gate = self.gate({"results": []})
        self.services.memory_recall_tool = True
        self.services.recall_handler = lambda body: gate(body) if body.get("query") == "DELAYED_TOOL_RECALL" else {"results": []}
        process = self.start()
        self.assertTrue(gate.entered.wait(15))
        snapshot = self.activity()
        self.assertEqual(snapshot["active_requests"], 0)
        self.assertEqual(snapshot["phase"], "tools_running")
        self.assertFalse(snapshot["wakeable"])
        self.assertGreater(snapshot["protected_until"], time.time())
        stale = self.wake(snapshot)
        time.sleep(0.2)
        self.assertTrue((self.state / "wake.json").exists())
        gate.release.set()
        self.wait_idle()
        self.wait_for(lambda: not (self.state / "wake.json").exists())
        self.assertNotEqual(self.read_state().get("last_wake_id"), stale["id"])
        session_id = self.read_state()["session_id"]
        before = len(self.services.requests)
        command = self.wake()
        self.wait_for(lambda: self.read_state().get("cycle") == 2)
        self.wait_idle()
        self.assertEqual(self.read_state()["session_id"], session_id)
        self.assertEqual(self.read_state()["last_wake_id"], command["id"])
        self.assertEqual(len(self.services.requests), before + 1)
        users = [m.get("content", "") for m in self.services.requests[-1]["messages"] if m["role"] == "user"]
        self.assertEqual(sum(WAKE_MESSAGE in text for text in users), 1)
        self.assertTrue(any(text.startswith(WAKE_MESSAGE + "\nContinue the existing goal") for text in users))
        self.assertEqual(list((self.state / "outbox").glob("*.json")), [])
        # Same command cannot be used again, even with a new valid revision.
        self.wake(self.activity(), command["id"])
        self.wait_for(lambda: not (self.state / "wake.json").exists())
        time.sleep(0.2)
        self.assertEqual(self.read_state()["cycle"], 2)
        self.assertEqual(len(self.services.requests), before + 1)
        self.stop(process)

    def test_malformed_and_wrong_identity_wakes_do_not_starve_valid_wake(self):
        process = self.start()
        snapshot = self.wait_idle()
        (self.state / "wake.json").write_text("{not valid JSON")
        self.wait_for(lambda: not (self.state / "wake.json").exists())
        self.assertEqual(len(list((self.state / "rejected").glob("wake_*.json"))), 1)
        (self.state / "wake.json").write_text(json.dumps({"id": {}, "schema": None}))
        self.wait_for(lambda: not (self.state / "wake.json").exists())
        wrong = dict(snapshot, pid=process.pid + 1)
        self.wake(wrong)
        self.wait_for(lambda: not (self.state / "wake.json").exists())
        self.assertEqual(self.read_state()["cycle"], 1)
        self.wake()
        self.wait_for(lambda: self.read_state()["cycle"] == 2)
        self.stop(process)

    def test_transport_error_clears_request_and_preserves_retry_backoff(self):
        def dropped_stream(_body):
            raise ConnectionResetError("Deliberate local transport error")
        self.services.stream_handler = dropped_stream
        process = self.start()
        snapshot = self.wait_for(lambda: self.activity() if self.activity().get("phase") == "retrying" else None)
        self.assertEqual(snapshot["active_requests"], 0)
        self.assertFalse(snapshot["wakeable"])
        self.assertGreater(snapshot["protected_until"], time.time())
        self.wake(snapshot)
        count = len(self.services.requests)
        time.sleep(0.3)
        self.assertEqual(len(self.services.requests), count)
        self.stop(process)

    def test_crash_after_wake_checkpoint_resumes_same_session_without_duplicate_prompt(self):
        process = self.start()
        self.wait_idle()
        session_id = self.read_state()["session_id"]
        gate = self.gate()
        self.services.stream_handler = gate
        command = self.wake()
        self.assertTrue(gate.entered.wait(15))
        self.assertEqual(self.read_state()["last_wake_id"], command["id"])
        self.assertEqual(self.read_state()["phase"], "running")
        process.kill()
        process.communicate(timeout=10)
        gate.release.set()
        self.services.stream_handler = None
        resumed = self.start(cycles=1)
        stdout, stderr = resumed.communicate(timeout=25)
        self.assertEqual(resumed.returncode, 0, stdout + stderr)
        self.assertEqual(self.read_state()["session_id"], session_id)
        self.assertEqual(self.read_state()["cycle"], 2)
        transcript = json.loads((self.state / "sessions" / (session_id + ".json")).read_text())
        self.assertEqual(sum(WAKE_MESSAGE in m.get("content", "") for m in transcript["messages"]
                             if m["role"] == "user"), 1)
        self.assertNotIn("wake", self.read_state())
        # A stale command from the prior process is discarded, never another turn.
        next_process = self.start()
        self.wait_idle()
        (self.state / "wake.json").write_text(json.dumps(command))
        self.wait_for(lambda: not (self.state / "wake.json").exists())
        cycle = self.read_state()["cycle"]
        time.sleep(0.2)
        self.assertEqual(self.read_state()["cycle"], cycle)
        self.stop(next_process)

    def test_crash_after_consume_before_prepare_keeps_pending_wake(self):
        process = self.start()
        self.wait_idle()
        session_id = self.read_state()["session_id"]
        gate = self.gate()
        self.services.ingest_handler = gate
        command = self.wake()
        self.assertTrue(gate.entered.wait(15))
        # The wake was checkpointed and acknowledged, but prompt preparation
        # has not begun. Holding a real service call makes this crash precise.
        checkpoint = self.read_state()
        self.assertEqual(checkpoint["phase"], "idle")
        self.assertEqual(checkpoint["wake"]["id"], command["id"])
        self.assertFalse((self.state / "wake.json").exists())
        process.kill()
        process.communicate(timeout=10)
        gate.release.set()
        self.services.ingest_handler = None
        resumed = self.start(cycles=1)
        stdout, stderr = resumed.communicate(timeout=25)
        self.assertEqual(resumed.returncode, 0, stdout + stderr)
        self.assertEqual(self.read_state()["session_id"], session_id)
        self.assertEqual(self.read_state()["cycle"], 2)
        self.assertEqual(self.read_state()["last_wake_id"], command["id"])
        transcript = json.loads((self.state / "sessions" / (session_id + ".json")).read_text())
        self.assertEqual(sum(WAKE_MESSAGE in m.get("content", "") for m in transcript["messages"]
                             if m["role"] == "user"), 1)
        self.assertNotIn("wake", self.read_state())

    def test_existing_engine_lock_prevents_second_process(self):
        process = self.start()
        self.wait_idle()
        second = self.start(cycles=1)
        stdout, stderr = second.communicate(timeout=10)
        self.assertNotEqual(second.returncode, 0, stdout + stderr)
        self.assertIn("Another autonomous engine owns", stdout + stderr)
        self.assertEqual(self.activity()["pid"], process.pid)
        self.assertEqual(self.read_state()["cycle"], 1)
        self.stop(process)

    def test_prepared_wake_already_appended_is_not_replayed_after_crash(self):
        self.run_engine()
        state = self.read_state()
        session_id = state["session_id"]
        session_path = self.state / "sessions" / (session_id + ".json")
        session = json.loads(session_path.read_text())
        prompt = WAKE_MESSAGE + (
            "\nContinue the existing goal and unfinished work with one concrete, verifiable step. "
            "Preserve the current session, use tools and evidence, and avoid repeating completed work.")
        saved_user = copy.deepcopy(next(m for m in session["messages"] if m["role"] == "user"))
        saved_user["id"] = str(uuid.uuid4())
        saved_user["content"] = prompt
        session["messages"].append(saved_user)
        header = {key: value for key, value in session.items() if key != "messages"}
        session_path.write_text(json.dumps(header)[:-1] + ',\n"messages": [\n' +
                                ",\n".join(json.dumps(m) for m in session["messages"]) + "\n]\n}\n")
        # Model the precise crash between durable user append and the running
        # checkpoint. A recovery notice must not change the deduplication key.
        state.update(phase="prepared", prompt=prompt, wake={"id": "prepared-boundary"},
                     last_wake_id="prepared-boundary")
        (self.state / "state.json").write_text(json.dumps(state))
        (self.state / "running.json").write_text(json.dumps({
            "pid": 999999, "phase": "running", "run_id": "previous-run"}))
        self.run_engine()
        restored = json.loads(session_path.read_text())
        for messages in (restored["messages"], self.services.requests[-1]["messages"]):
            self.assertEqual(sum(WAKE_MESSAGE in m.get("content", "") for m in messages
                                 if m["role"] == "user"), 1)
        self.assertEqual(self.read_state()["session_id"], session_id)
        self.assertEqual(self.read_state()["cycle"], 2)


if __name__ == "__main__":
    unittest.main()
