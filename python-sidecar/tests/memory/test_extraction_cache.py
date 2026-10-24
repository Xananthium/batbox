"""Exercise real cache keys/SQLite with a mock transport; no model calls."""
import json
from pathlib import Path
import sqlite3
import tempfile
import unittest
from unittest.mock import patch

from openai import DefaultHttpxClient
try:
    from openai._base_client import httpx2 as httpx
except ImportError:
    from openai._base_client import httpx
from hipporag.llm.openai_gpt import CacheOpenAI
from hipporag.utils.config_utils import BaseConfig

from batbox_memory.extraction_cache import (
    checked_extraction_infer, extraction_cache_key, extraction_error, quarantine_failed_response,
)


class CheckedLLM(CacheOpenAI):
    def infer(self, messages, **kwargs):
        return checked_extraction_infer(self, super().infer, messages, kwargs, "triples")


class ExtractionCacheTests(unittest.TestCase):
    valid = '{"triples": [["memory", "uses", "CPU"]]}'
    invalid = '{"triples": [["memory", "uses", "unfinished, text'
    messages = [{"role": "system", "content": "construct an RDF"},
                {"role": "user", "content": "memory uses CPU"}]

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.responses, self.requests = [], []
        self.llm = self.make_llm()

    def make_llm(self):
        llm = CheckedLLM(self.directory.name, BaseConfig(
            llm_name="cache-test", llm_base_url="http://127.0.0.1:1/v1", max_new_tokens=512))
        original = llm.openai_client
        llm.openai_client = original.with_options(
            http_client=DefaultHttpxClient(transport=httpx.MockTransport(self.respond)))
        original.close()
        self.addCleanup(llm.close)
        return llm

    def respond(self, request):
        body = json.loads(request.content)
        self.requests.append(body)
        message, finish = self.responses.pop(0)
        return httpx.Response(200, json={
            "id": "test", "object": "chat.completion", "created": 1, "model": "cache-test",
            "choices": [{"index": 0, "finish_reason": finish,
                         "message": {"role": "assistant", "content": message}}],
            "usage": {"prompt_tokens": 10, "completion_tokens": body["max_tokens"],
                      "total_tokens": 10 + body["max_tokens"]},
        })

    def row(self, messages=None, limit=1536):
        key = extraction_cache_key(self.llm, messages or self.messages, {"max_new_tokens": limit})
        with sqlite3.connect(self.llm.cache_file_name) as connection:
            return connection.execute("SELECT message,metadata FROM cache WHERE key=?", (key,)).fetchone()

    def test_poisoned_cache_retries_once_and_restart_reuses_larger_result(self):
        neighbor = [{"role": "user", "content": "a different valid passage"}]
        self.responses = [(self.valid, "stop"), (self.invalid, "length"), (self.valid, "stop")]
        CacheOpenAI.infer(self.llm, messages=neighbor, max_new_tokens=1536)
        valid_row = self.row(neighbor)
        CacheOpenAI.infer(self.llm, messages=self.messages, max_new_tokens=1536)
        self.assertIsNotNone(self.row())  # Reproduce the old decorator's poisoned row.
        result = self.llm.infer(self.messages, max_new_tokens=1536)
        self.assertEqual(result[0], self.valid)
        self.assertFalse(result[2])
        self.assertIsNone(self.row())
        self.assertIsNotNone(self.row(limit=3072))
        self.assertEqual(self.row(neighbor), valid_row)
        backups = list(Path(self.directory.name).glob("rejected-extractions/*.json"))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].stat().st_mode & 0o777, 0o600)
        self.assertEqual(backups[0].parent.stat().st_mode & 0o777, 0o700)
        self.assertEqual(json.loads(backups[0].read_text())["message"], self.invalid)
        restarted = self.make_llm()
        self.assertTrue(restarted.infer(self.messages, max_completion_tokens=1536)[2])
        self.assertTrue(self.llm.infer(neighbor, max_new_tokens=1536)[2])
        self.assertEqual([r["max_tokens"] for r in self.requests], [1536, 1536, 3072])

    def test_fresh_invalid_stop_is_removed_and_next_call_can_recover(self):
        self.responses = [('not JSON', "stop"), (self.valid, "stop")]
        with self.assertRaisesRegex(ValueError, "valid JSON"):
            self.llm.infer(self.messages, max_new_tokens=1536)
        self.assertIsNone(self.row())
        self.assertEqual(self.llm.infer(self.messages, max_new_tokens=1536)[0], self.valid)
        self.assertEqual([r["max_tokens"] for r in self.requests], [1536, 1536])

    def test_repairable_length_response_keeps_its_cache_hit(self):
        repaired = '{"triples": [["memory", "uses", "CPU"]], '
        self.responses = [(repaired, "length")]
        self.assertIsNone(extraction_error("triples", repaired, {"finish_reason": "length"}))
        self.llm.infer(self.messages, max_new_tokens=1536)
        self.assertTrue(self.llm.infer(self.messages, max_new_tokens=1536)[2])
        self.assertEqual(len(self.requests), 1)
        self.assertEqual(self.row()[0], repaired)
        self.assertFalse((Path(self.directory.name) / "rejected-extractions").exists())

    def test_larger_retry_is_bounded_and_remembered_after_failure(self):
        self.responses = [(self.invalid, "length")] * 3
        with self.assertRaises(ValueError):
            self.llm.infer(self.messages, max_new_tokens=3000)
        self.assertEqual([r["max_tokens"] for r in self.requests], [3000, 4096])
        with self.assertRaises(ValueError):
            self.make_llm().infer(self.messages, max_new_tokens=3000)
        self.assertEqual([r["max_tokens"] for r in self.requests], [3000, 4096, 4096])
        self.assertIsNone(self.row(limit=3000))
        self.assertIsNone(self.row(limit=4096))

    def test_concurrent_valid_replacement_is_preserved(self):
        self.responses = [(self.valid, "stop")]
        CacheOpenAI.infer(self.llm, messages=self.messages, max_new_tokens=1536)
        row = self.row()
        removed = quarantine_failed_response(self.llm, self.messages, {"max_new_tokens": 1536},
                                             self.invalid, {"finish_reason": "length"}, "invalid")
        self.assertFalse(removed)
        self.assertEqual(self.row(), row)

    def test_interrupted_quarantine_preserves_row_and_ignores_partial_temp(self):
        self.responses = [(self.invalid, "length"), (self.valid, "stop")]
        CacheOpenAI.infer(self.llm, messages=self.messages, max_new_tokens=1536)
        row = self.row()
        with patch("batbox_memory.extraction_cache.os.replace", side_effect=OSError("interrupted publish")):
            with self.assertRaisesRegex(OSError, "interrupted publish"):
                self.llm.infer(self.messages, max_new_tokens=1536)
        self.assertEqual(self.row(), row)
        directory = Path(self.directory.name) / "rejected-extractions"
        self.assertEqual(list(directory.glob("*.json")), [])
        (directory / ".quarantine-crash.tmp").write_text('{"incomplete":')
        self.assertEqual(self.llm.infer(self.messages, max_new_tokens=1536)[0], self.valid)
        self.assertIsNone(self.row())
        self.assertEqual(len(list(directory.glob("*.json"))), 1)
        self.assertEqual([r["max_tokens"] for r in self.requests], [1536, 3072])

    def test_field_schema_accepts_empty_and_bare_ner_and_rejects_bad_items(self):
        for field, message in (("named_entities", '[]'), ("named_entities", '["CPU"]'),
                               ("named_entities", '{"named_entities": []}'), ("triples", '{"triples": []}')):
            with self.subTest(field=field, message=message):
                self.assertIsNone(extraction_error(field, message, {"finish_reason": "stop"}))
        for field, message in (("named_entities", '{"named_entities": [1]}'),
                               ("triples", '{"triples": [["a", "b"]]}'),
                               ("triples", '{"triples": [["a", "b", 1]]}'), ("triples", '')):
            with self.subTest(field=field, message=message):
                self.assertIsNotNone(extraction_error(field, message, {"finish_reason": "stop"}))


if __name__ == "__main__":
    unittest.main()
