"""Exercise early quality cancellation through the real HTTP/native stack."""

import json
import shlex
import subprocess
import threading
import time
import unittest

import test_generation_quality as fixtures


class StreamingResponse:
    def __init__(self, chunks, finish="stop", delay=0.003):
        self.chunks = chunks
        self.finish = finish
        self.delay = delay
        self.started = threading.Event()
        self.closed = threading.Event()
        self.started_at = None
        self.closed_at = None
        self.sent = 0
        self.disconnected = False


class StreamingServices(fixtures.QualityServices):
    def __init__(self):
        super().__init__()
        owner = self
        original = self.server.RequestHandlerClass

        class Handler(original):
            def do_POST(self):
                if (self.path != "/v1/chat/completions" or not owner.responses or
                        not isinstance(owner.responses[0], StreamingResponse)):
                    return super().do_POST()
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                owner.requests.append(body)
                owner.request_times.append(time.monotonic())
                response = owner.responses.pop(0)
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Connection", "close")
                self.end_headers()
                self.close_connection = True
                response.started_at = time.monotonic()
                try:
                    for text in response.chunks:
                        chunk = {"choices": [{"index": 0, "delta": {"content": text},
                                               "finish_reason": None}]}
                        self.wfile.write(("data: " + json.dumps(chunk) + "\n\n").encode())
                        self.wfile.flush()
                        response.sent += 1
                        response.started.set()
                        time.sleep(response.delay)
                    terminal = {"choices": [{"index": 0, "delta": {},
                                              "finish_reason": response.finish}],
                                "usage": {"prompt_tokens": 250, "completion_tokens": 4096,
                                          "total_tokens": 4346}}
                    self.wfile.write(("data: " + json.dumps(terminal) +
                                      "\n\ndata: [DONE]\n\n").encode())
                    self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError):
                    response.disconnected = True
                finally:
                    response.closed_at = time.monotonic()
                    response.closed.set()

        self.server.RequestHandlerClass = Handler


@unittest.skipUnless(fixtures.BINARY.exists(), "Build native batbox or set BATBOX_AUTONOMOUS_BIN")
class StreamingGenerationRecovery(unittest.TestCase):
    tearDown = fixtures.GenerationRecovery.tearDown
    run_engine = fixtures.GenerationRecovery.run_engine
    checkpoint = fixtures.GenerationRecovery.checkpoint
    transcript_path = fixtures.GenerationRecovery.transcript_path
    events = fixtures.GenerationRecovery.events
    assert_no_loop_in_model_content = fixtures.GenerationRecovery.assert_no_loop_in_model_content

    def setUp(self):
        fixtures.GenerationRecovery.setUp(self)
        self.services.close()
        self.services = StreamingServices()
        self.environment["BATBOX_API_BASE_URL"] = self.services.url + "/v1"

    def test_early_abort_retains_exact_partial_text_and_completed_tool_runs_once(self):
        evidence = self.directory / "executed.txt"
        command = "printf x >> " + shlex.quote(str(evidence))
        tool = {"tool_calls": [{"index": 0, "id": "one-action", "type": "function",
                  "function": {"name": "Bash", "arguments": json.dumps({"command": command,
                      "description": "Record one verified action."})}}]}
        looping = StreamingResponse([" our"] * 4096)
        self.services.responses = [tool, looping, (fixtures.GOOD, "stop")]
        self.run_engine()
        self.assertTrue(looping.closed.wait(2))
        self.assertTrue(looping.disconnected)
        self.assertLess(looping.sent, 512)
        self.assertLess(looping.closed_at - looping.started_at, 4)
        self.assertEqual(evidence.read_text(), "x")
        self.assertEqual(len(self.services.requests), 3)
        self.assertGreaterEqual(self.services.request_times[2] - looping.closed_at, 9)
        self.assert_no_loop_in_model_content(self.services.requests[2])
        raw = json.loads(self.transcript_path().read_text())["messages"]
        rejected = [m for m in raw if m.get("generation_rejected") == "degenerate_repetition"]
        self.assertEqual(len(rejected), 1)
        content = rejected[0]["content"]
        self.assertEqual(content, " our" * (len(content) // 4))
        self.assertGreaterEqual(len(content), 512)
        self.assertLessEqual(len(content), 1024)
        quality = rejected[0]["generation_quality"]
        self.assertTrue(quality["stream_aborted"])
        self.assertEqual(quality["received_content_bytes"], len(content.encode()))
        self.assertEqual(sum(m.get("role") == "tool" and m.get("tool_call_id") == "one-action"
                             for m in raw), 1)
        failures = [e for e in self.events() if e["event"] == "cycle_failed"]
        self.assertEqual(len(failures), 1)
        self.assertIn("degenerate_generation", failures[0]["error"])
        self.assertEqual([m["content"] for m in self.services.memories
                          if m["kind"] == "generated_observation"], [fixtures.GOOD])

    def test_streamed_fenced_source_and_serialized_data_finish_unchanged(self):
        for chunks in (["```text\n"] + ["our "] * 256 + ["\n```"],
                       ['{"value":"our"}\n'] * 256):
            with self.subTest(prefix=chunks[0]):
                streamed = StreamingResponse(chunks, finish="length", delay=0.001)
                self.services.responses = [streamed]
                before = len(self.services.requests)
                self.run_engine()
                self.assertFalse(streamed.disconnected)
                self.assertEqual(len(self.services.requests), before + 1)
                self.assertEqual(self.services.memories[-1]["content"], "".join(chunks))
        self.assertFalse(any(e["event"] == "cycle_failed" for e in self.events()))

    def test_user_shutdown_is_not_reported_as_quality_failure(self):
        streamed = StreamingResponse([f"Verified measurement {i}.\n" for i in range(4096)],
                                     delay=0.01)
        self.services.responses = [streamed]
        command = [str(fixtures.BINARY), "--autonomous", "--engine-state-dir", str(self.state),
            "--engine-letter-file", str(self.letters), "--engine-project-root", str(self.directory),
            "--engine-memory-url", self.services.url, "--engine-max-cycles", "1",
            "--engine-interval", "1", "--engine-workers", "0"]
        proc = subprocess.Popen(command, env=self.environment, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, cwd=self.directory)
        try:
            self.assertTrue(streamed.started.wait(10))
            proc.terminate()
            stdout, stderr = proc.communicate(timeout=10)
            self.assertEqual(proc.returncode, 0, stdout + stderr)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.communicate()
        self.assertFalse(any("degenerate_generation" in e.get("error", "") for e in self.events()))
        self.assertFalse(any(e["event"] == "cycle_completed" for e in self.events()))
        raw = json.loads(self.transcript_path().read_text())["messages"]
        self.assertFalse(any(m.get("generation_rejected") for m in raw))


if __name__ == "__main__":
    unittest.main()
