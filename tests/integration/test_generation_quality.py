"""Real autonomous executable against local, deterministic quality fixtures."""

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
LOOP = "our " * 4096
GOOD = "Verified the saved evidence and completed one useful check."


class QualityServices:
    def __init__(self):
        self.requests = []
        self.request_times = []
        self.memories = []
        self.responses = []
        self.recall = {"results": []}
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
                    return self.send_json({"count": 250, "max_model_len": 200000, "tokens": []})
                if self.path == "/ingest":
                    return self.send_json({"status": "queued"}, 202)
                if self.path == "/remember":
                    owner.memories.append(body)
                    return self.send_json({"status": "pending", "id": body.get("id")}, 202)
                if self.path == "/recall":
                    return self.send_json(owner.recall)
                if self.path != "/v1/chat/completions":
                    return self.send_json({"error": "unknown path"}, 404)
                owner.requests.append(body)
                owner.request_times.append(time.monotonic())
                response = owner.responses.pop(0) if owner.responses else (GOOD, "stop")
                if isinstance(response, dict):
                    delta, finish = response, "tool_calls"
                else:
                    content, finish = response
                    delta = {"role": "assistant", "content": content}
                chunks = [{"choices": [{"index": 0, "delta": delta, "finish_reason": None}]},
                    {"choices": [{"index": 0, "delta": {}, "finish_reason": finish}],
                     "usage": {"prompt_tokens": 250, "completion_tokens": 4096, "total_tokens": 4346}}]
                payload = ("".join("data: " + json.dumps(chunk) + "\n\n" for chunk in chunks)
                           + "data: [DONE]\n\n").encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.url = f"http://127.0.0.1:{self.server.server_address[1]}"

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()


@unittest.skipUnless(BINARY.exists(), "Build native batbox or set BATBOX_AUTONOMOUS_BIN")
class GenerationRecovery(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="batbox-quality-test-")
        self.directory = Path(self.temp.name)
        self.state = self.directory / "state"
        self.letters = self.directory / "letters.txt"
        self.letters.write_text("Continue useful, verified work.")
        self.services = QualityServices()
        self.environment = {k: v for k, v in os.environ.items() if not k.startswith("BATBOX_")}
        self.environment.update({
            "HOME": str(self.directory), "BATBOX_CONFIG_DIR": str(self.directory / "config"),
            "BATBOX_API_BASE_URL": self.services.url + "/v1", "BATBOX_API_KEY": "test-local",
            "BATBOX_DEFAULT_MODEL": "test-model", "BATBOX_MODELS": "test-model",
            "BATBOX_CTX_LEN_DEFAULT": "200000", "BATBOX_MAX_TOKENS": "4096",
            "BATBOX_REQUEST_TIMEOUT_SEC": "10", "BATBOX_PROVIDER_HINT": "openai",
            "BATBOX_ENABLE_THINKING": "false",
        })

    def tearDown(self):
        self.services.close()
        self.temp.cleanup()

    def run_engine(self):
        command = [str(BINARY), "--autonomous", "--engine-state-dir", str(self.state),
            "--engine-letter-file", str(self.letters), "--engine-project-root", str(self.directory),
            "--engine-memory-url", self.services.url, "--engine-max-cycles", "1",
            "--engine-interval", "1", "--engine-workers", "0"]
        result = subprocess.run(command, env=self.environment, text=True, capture_output=True,
                                timeout=45, cwd=self.directory)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def checkpoint(self):
        return json.loads((self.state / "state.json").read_text())

    def transcript_path(self):
        return self.state / "sessions" / (self.checkpoint()["session_id"] + ".json")

    def events(self):
        return [json.loads(line) for line in (self.state / "events.jsonl").read_text().splitlines()]

    def assert_no_loop_in_model_content(self, request):
        self.assertFalse(any("our " * 128 in (m.get("content") or "") for m in request["messages"]))

    def test_rejected_final_is_durable_retries_with_backoff_and_never_enters_memory(self):
        self.services.responses = [(LOOP, "length"), (GOOD, "stop")]
        self.run_engine()
        self.assertEqual(self.checkpoint()["cycle"], 1)
        self.assertEqual(len(self.services.requests), 2)
        self.assertGreaterEqual(self.services.request_times[1] - self.services.request_times[0], 9.0)
        self.assert_no_loop_in_model_content(self.services.requests[1])
        raw = json.loads(self.transcript_path().read_text())["messages"]
        rejected = [m for m in raw if m.get("generation_rejected") == "degenerate_repetition"]
        self.assertEqual(len(rejected), 1)
        self.assertEqual(rejected[0]["role"], "assistant")
        self.assertEqual(rejected[0]["content"], LOOP)
        self.assertEqual(rejected[0]["generation_quality"]["repeated_tokens"], 4096)
        self.assertEqual([m["content"] for m in self.services.memories if m["kind"] == "generated_observation"], [GOOD])
        self.assertEqual(list((self.state / "memory-outbox").glob("*.json")), [])
        failures = [e for e in self.events() if e["event"] == "cycle_failed"]
        self.assertEqual(len(failures), 1)
        self.assertIn("degenerate_generation", failures[0]["error"])
        self.assertEqual(len([e for e in self.events() if e["event"] == "cycle_completed"]), 1)
        # A later process reconstructs a filtered view from the preserved raw file.
        self.run_engine()
        self.assert_no_loop_in_model_content(self.services.requests[-1])
        restored_raw = json.loads(self.transcript_path().read_text())["messages"]
        self.assertEqual(next(m for m in restored_raw if m["id"] == rejected[0]["id"]), rejected[0])

    def test_length_ended_legitimate_prose_and_fenced_repeated_data_are_accepted(self):
        long_prose = "\n".join(f"Verified measurement {i}: completed with evidence." for i in range(1500))
        for content in (long_prose, "```text\n" + LOOP + "\n```"):
            with self.subTest(content=content[:30]):
                before = len(self.services.requests)
                self.services.responses = [(content, "length")]
                self.run_engine()
                self.assertEqual(len(self.services.requests), before + 1)
                self.assertEqual(self.services.memories[-1]["content"], content)
        self.assertFalse(any(e["event"] == "cycle_failed" for e in self.events()))
        self.assertEqual(self.checkpoint()["cycle"], 2)

    def test_pending_legacy_checkpoint_filters_repetition_without_erasing_raw_history(self):
        self.run_engine()
        path = self.transcript_path()
        session = json.loads(path.read_text())
        state = self.checkpoint()
        template = session["messages"][-1]
        pending_user = dict(template, id=str(uuid.uuid4()), role="user", content="Continue the unfinished check.")
        old_bad = dict(template, id=str(uuid.uuid4()), role="assistant", content=LOOP)
        # Simulate the production failure: the recent suffix of a durable
        # compaction checkpoint still contains an unmarked repetitive final.
        checkpoint = dict(template, id=str(uuid.uuid4()), role="system", content="Context checkpoint.",
            context_checkpoint=session["messages"] + [pending_user, old_bad], compaction_count=5,
            prompt_tokens_after=500)
        session["messages"].extend([pending_user, old_bad, checkpoint])
        original_messages = copy.deepcopy(session["messages"])
        header = {k: v for k, v in session.items() if k != "messages"}
        path.write_text(json.dumps(header)[:-1] + ',\n"messages": [\n' +
            ",\n".join(json.dumps(m) for m in original_messages) + "\n]\n}\n")
        state.update(phase="running", prompt=pending_user["content"])
        (self.state / "state.json").write_text(json.dumps(state))
        self.services.requests.clear()
        self.services.memories.clear()
        self.run_engine()
        self.assertEqual(len(self.services.requests), 1, "Legacy bad final was treated as completed work")
        self.assert_no_loop_in_model_content(self.services.requests[0])
        self.assertTrue(any("Generation recovery" in (m.get("content") or "")
                            for m in self.services.requests[0]["messages"]))
        saved = json.loads(path.read_text())["messages"]
        self.assertEqual(saved[:len(original_messages)], original_messages)
        self.assertEqual(saved[-1]["content"], GOOD)
        self.assertEqual(self.checkpoint()["cycle"], 2)
        self.assertEqual([m["content"] for m in self.services.memories if m["kind"] == "generated_observation"], [GOOD])

    def test_pending_generated_memory_is_quarantined_and_recall_copy_is_filtered(self):
        pending = self.state / "memory-outbox"
        pending.mkdir(parents=True)
        bad = {"id": "old-generated", "kind": "generated_observation", "content": LOOP}
        supplied_receipt = {"version": 1, "full_content_accepted": True}
        # Neither a top-level flag nor metadata supplied in a pending queue
        # record may waive revalidation of its full generated content.
        bad["metadata"] = {"batbox_generation_quality": supplied_receipt}
        bad["batbox_generation_quality"] = supplied_receipt
        source = {"id": "source-data", "kind": "tool_observation", "content": LOOP}
        (pending / "a-bad.json").write_text(json.dumps(bad))
        (pending / "b-source.json").write_text(json.dumps(source))
        historical = {"id": "historical-unmarked", "kind": "generated_observation", "content": LOOP}
        self.services.recall = {"results": [historical, {"id": "good-source", "kind": "current_verified", "content": "Verified source marker."}]}
        self.run_engine()
        self.assertEqual(json.loads((self.state / "rejected/generated-memory/a-bad.json").read_text()), bad)
        self.assertFalse(any(m.get("id") == "old-generated" for m in self.services.memories))
        self.assertIn(source, self.services.memories)
        self.assert_no_loop_in_model_content(self.services.requests[0])
        self.assertIn("Verified source marker.", json.dumps(self.services.requests[0]))
        self.assertEqual(self.services.recall["results"][0], historical, "Historical memory was modified")

    def test_accepted_fenced_data_remains_recallable_after_chunking(self):
        self.services.responses = [("```text\n" + LOOP + "\n```", "length")]
        self.run_engine()
        accepted = next(m for m in self.services.memories if m["kind"] == "generated_observation")
        receipt = accepted["metadata"]["batbox_generation_quality"]
        self.assertEqual(receipt, {"version": 1, "full_content_accepted": True})
        # Both real recall backends flatten metadata into each result. The
        # existing 180-word chunker removes fences from interior code chunks.
        chunk = dict(accepted, content="our " * 180, **accepted["metadata"])
        chunk.pop("metadata")
        chunk["id"] = "accepted-code-chunk"
        historical = {"id": "unmarked-old-loop", "kind": "generated_observation", "content": "our " * 180}
        self.services.recall = {"results": [chunk, historical]}
        self.run_engine()
        prompt = "\n".join(m.get("content") or "" for m in self.services.requests[-1]["messages"] if m["role"] == "user")
        self.assertIn("accepted-code-chunk", prompt)
        self.assertIn("our " * 128, prompt)
        self.assertNotIn("unmarked-old-loop", prompt)
        self.assertEqual(self.services.recall["results"], [chunk, historical])

    def test_explicit_memory_remember_rejects_loop_without_replaying_the_tool(self):
        self.services.responses = [{"role": "assistant", "tool_calls": [{"index": 0,
            "id": "bad-memory", "type": "function", "function": {"name": "MemoryRemember",
            "arguments": json.dumps({"content": LOOP, "source": "transcript://quality-test",
                "metadata": {"batbox_generation_quality": {"version": 1, "full_content_accepted": True}}})}}]}, (GOOD, "stop")]
        self.run_engine()
        self.assertEqual(len(self.services.requests), 2)
        tool_results = [m for m in self.services.requests[-1]["messages"] if m.get("tool_call_id") == "bad-memory"]
        self.assertEqual(len(tool_results), 1)
        self.assertIn("repetition", tool_results[0]["content"])
        self.assertFalse(any(m.get("content") == LOOP for m in self.services.memories))
        raw = json.loads(self.transcript_path().read_text())["messages"]
        intents = [m for m in raw if m.get("tool_calls")]
        self.assertEqual(len(intents), 1)
        self.assertIn(LOOP, json.dumps(intents[0]))


if __name__ == "__main__":
    unittest.main()
