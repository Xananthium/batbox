import tempfile
import unittest
from pathlib import Path

import numpy as np

from batbox_memory.store import Store


class Encoder:
    def __init__(self):
        self.calls = 0

    def embed(self, texts):
        for text in texts:
            self.calls += 1
            yield np.array([text.count("memory") + 1, text.count("server") + 1, 1], dtype=float)


class StoreTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name) / "memory.db"
        self.encoder = Encoder()
        self.store = Store(self.path, encoder=self.encoder)

    def tearDown(self):
        self.temp.cleanup()

    def test_submit_is_durable_before_embeddings_and_restart_finishes_it(self):
        response = self.store.enqueue("server memory", "transcript://batbox/session/1", "test")
        self.assertEqual(response["status"], "pending")
        self.assertEqual(self.encoder.calls, 0)
        self.assertEqual(self.store.stats()["nodes"], 0)
        restored = Store(self.path, encoder=self.encoder)
        self.assertTrue(restored.process_one())
        self.assertEqual(restored.stats()["queue"], {"ready": 1})
        result = restored.recall("memory")["results"][0]
        self.assertEqual(result["id"], "test")
        self.assertEqual(result["source"], "transcript://batbox/session/1")
        self.assertEqual(result["content_hash"], response["content_hash"])

    def test_reversion_does_not_get_mistaken_for_already_indexed_content(self):
        for text in ("first memory", "second server", "first memory"):
            self.store.enqueue(text, "file:///memory/test.md", "same")
            self.store.process_one()
        with self.store.connect() as c:
            self.assertEqual(c.execute("SELECT content FROM nodes WHERE id='same'").fetchone()[0], "first memory")
        self.assertEqual(self.store.stats()["queue"], {"ready": 3})
        self.assertEqual(self.encoder.calls, 2)

    def test_duplicate_submission_and_embedding_cache_survive_restart(self):
        first = self.store.enqueue("a memory", "file:///memory/one.md", "one")
        self.assertEqual(self.store.enqueue("a memory", "file:///memory/one.md", "one")["job_id"], first["job_id"])
        self.store.process_one()
        self.store = Store(self.path, encoder=self.encoder)
        self.store.enqueue("a memory", "file:///memory/two.md", "two")
        self.store.process_one()
        self.assertEqual(self.encoder.calls, 1)
        self.assertEqual(self.store.stats()["counters"]["embedding_cache_hits"], 1)

    def test_newer_pending_version_supersedes_older_version(self):
        self.store.enqueue("old server", "file:///memory/n.md", "n")
        self.store.enqueue("new memory", "file:///memory/n.md", "n")
        self.store.process_one()
        self.store.process_one()
        with self.store.connect() as c:
            self.assertEqual(c.execute("SELECT content FROM nodes WHERE id='n'").fetchone()[0], "new memory")
        self.assertEqual(self.store.stats()["queue"], {"ready": 1, "superseded": 1})

    def test_failed_embedding_retains_retryable_job_and_does_not_claim_ready(self):
        class Broken:
            def embed(self, texts):
                raise RuntimeError("encoder unavailable")
        self.store.encoder = Broken()
        self.store.enqueue("remember this", "file:///memory/test.md", "test")
        with self.assertRaises(RuntimeError):
            self.store.process_one()
        self.assertEqual(self.store.stats()["nodes"], 0)
        self.assertEqual(self.store.stats()["queue"], {"retry": 1})

    def test_provenance_required(self):
        with self.assertRaises(ValueError):
            self.store.enqueue("unsourced assertion", "made up")


if __name__ == "__main__":
    unittest.main()
