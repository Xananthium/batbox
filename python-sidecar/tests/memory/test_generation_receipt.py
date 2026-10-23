"""Native full-content quality receipts survive real memory recall schemas."""
import tempfile
from pathlib import Path
from types import SimpleNamespace
import unittest

from batbox_memory.graph import Graph
from batbox_memory.store import Store, chunks


RECEIPT = {"version": 1, "full_content_accepted": True}
CONTENT = "```text\n" + "our " * 4096 + "\n```"


class InteriorChunkEncoder:
    def embed(self, texts):
        for text in texts:
            # Deterministic scoring selects an interior, fence-less data chunk
            # in the production Store.recall path without loading a model.
            yield [1.0, 0.0] if "```" not in text else [0.0, 1.0]


class GenerationReceiptTests(unittest.TestCase):
    def test_cpu_recall_retains_native_receipt_after_real_chunking(self):
        with tempfile.TemporaryDirectory() as temporary:
            store = Store(Path(temporary) / "memory.db", encoder=InteriorChunkEncoder())
            store.enqueue(CONTENT, "transcript://quality/accepted", "accepted-data", "generated_observation",
                          metadata={"batbox_generation_quality": RECEIPT})
            self.assertTrue(store.process_one())
            result = store.recall("interior data")["results"][0]
            self.assertEqual(result["kind"], "generated_observation")
            self.assertEqual(result["batbox_generation_quality"], RECEIPT)
            self.assertEqual(result["content"].split(), ["our"] * 180)
            with store.connect() as connection:
                original = connection.execute("SELECT content FROM nodes WHERE id='accepted-data'").fetchone()[0]
            self.assertEqual(original, CONTENT)

    def test_hipporag_recall_preserves_receipt_without_adding_one_to_old_records(self):
        with tempfile.TemporaryDirectory() as temporary:
            counts = []
            graph = Graph(SimpleNamespace(count=counts.append), temporary, "http://unused", "fixture")
            chunk = chunks(CONTENT)[1]
            self.assertEqual(chunk.split(), ["our"] * 180)
            solution = SimpleNamespace(docs=[chunk, chunk], doc_scores=[1.0, 0.9], graph_seeds=[],
                doc_metadata=[
                    {"source_id": "accepted-data", "source": "transcript://quality/accepted",
                     "kind": "generated_observation", "batbox_generation_quality": RECEIPT},
                    {"source_id": "old-unmarked", "source": "transcript://quality/old",
                     "kind": "generated_observation"}])
            graph.rag = SimpleNamespace(retrieve=lambda queries, num_to_retrieve: [solution])
            graph.status.update(state="ready", passages=2)
            result = graph.recall("interior data", 2)
            self.assertEqual(result["backend"], "HippoRAG")
            self.assertEqual(result["results"][0]["batbox_generation_quality"], RECEIPT)
            self.assertEqual(result["results"][0]["content"], chunk)
            self.assertNotIn("batbox_generation_quality", result["results"][1])
            self.assertEqual(counts, ["hipporag_retrievals"])


if __name__ == "__main__":
    unittest.main()
