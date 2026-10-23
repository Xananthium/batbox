"""Check service timeout configuration on a real HTTP request, without a model."""
import json
from pathlib import Path
import tempfile
import threading
import time
import unittest
from unittest.mock import Mock

from openai import DefaultHttpxClient
try:  # Match the SDK's HTTP transport; these stream classes are not interchangeable.
    from openai._base_client import httpx2 as httpx
except ImportError:
    from openai._base_client import httpx

from batbox_memory.__main__ import Service
from batbox_memory.graph import Graph


class GraphTimeoutTests(unittest.TestCase):
    def test_default_and_override_reach_extraction_http_request(self):
        for configured, expected in ((None, 600), (317, 317)):
            with self.subTest(configured=configured), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                config = {
                    "database": str(root / "memory.db"),
                    "graph_directory": str(root / "graph"),
                    "session_directory": str(root / "sessions"),
                    "llm_base_url": "http://127.0.0.1:1/v1",
                    "llm_model": "timeout-test",
                    "scrapling_python": "/unused",
                }
                if configured is not None:
                    config["llm_request_timeout_seconds"] = configured
                service = Service(config)
                try:
                    service.graph.initialize()
                    observed = []

                    def respond(request):
                        observed.append((dict(request.extensions["timeout"]), json.loads(request.content)))
                        return httpx.Response(200, json={
                            "id": "test-timeout", "object": "chat.completion", "created": 1,
                            "model": "timeout-test",
                            "choices": [{"index": 0, "finish_reason": "stop", "message": {
                                "role": "assistant", "content": '{"named_entities": ["memory"]}'}}],
                            "usage": {"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15},
                        })

                    llm = service.graph.llm
                    original = llm.openai_client
                    llm.openai_client = original.with_options(
                        http_client=DefaultHttpxClient(transport=httpx.MockTransport(respond)))
                    original.close()
                    messages = [{"role": "system", "content": "extract named entities"},
                                {"role": "user", "content": "memory"}]
                    llm.infer(messages, max_completion_tokens=512)
                    self.assertEqual(len(observed), 1)
                    timeout, payload = observed[0]
                    self.assertEqual(timeout, dict(connect=expected, read=expected, write=expected, pool=expected))
                    self.assertEqual(service.graph.status["llm_request_timeout_seconds"], expected)
                    self.assertEqual(llm.openai_client.max_retries, 1)
                    self.assertEqual(payload["max_tokens"], 512)
                    self.assertEqual(payload["chat_template_kwargs"], {"enable_thinking": False})
                    self.assertEqual(payload["response_format"]["json_schema"]["name"], "named_entities")
                    self.assertFalse(any("timeout" in key for key in payload))
                    self.assertEqual(service.config.get("shutdown_timeout_seconds", 150), 150)
                finally:
                    service.close()

    def test_invalid_timeout_rejected_before_initialization(self):
        for invalid in (0, -1, float("inf"), float("nan")):
            with self.subTest(invalid=invalid), self.assertRaisesRegex(ValueError, "finite and positive"):
                Graph(None, "/unused", "http://127.0.0.1:1/v1", "test", request_timeout_seconds=invalid)

    def test_slow_request_does_not_extend_shutdown_deadline(self):
        service = Service.__new__(Service)
        service.config = {"shutdown_timeout_seconds": 0.05, "llm_request_timeout_seconds": 600}
        service.stop, service.graph = threading.Event(), Mock()
        release = threading.Event()
        worker = threading.Thread(target=release.wait)
        service.threads = [worker]
        worker.start()
        try:
            start = time.monotonic()
            service.close()
            self.assertLess(time.monotonic() - start, 1)
            self.assertTrue(service.stop.is_set())
            self.assertTrue(worker.is_alive())
            service.graph.close.assert_not_called()
        finally:
            release.set()
            worker.join()


if __name__ == "__main__":
    unittest.main()
