"""Real HippoRAG persistence with CPU fixtures and SIGKILL fault injection.

No model endpoint is contacted. OpenIE output and 384-dimensional embeddings
are deterministic fixtures; graph construction and on-disk stores are real.
"""
import hashlib
import json
import os
from pathlib import Path
import signal
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import Mock, patch

import numpy as np

from batbox_memory.graph import Graph
from batbox_memory.store import Store


class Encoder:
    def __init__(self, allowed=True):
        self.allowed = allowed
        self.calls = 0

    def embed(self, texts):
        if not self.allowed:
            raise AssertionError("Recovery repeated a completed embedding")
        for text in texts:
            self.calls += 1
            rng = np.random.default_rng(int(hashlib.sha256(text.encode()).hexdigest()[:8], 16))
            yield rng.normal(size=384).astype(np.float32)


def fake_openie(_self, rows):
    from hipporag.utils.misc_utils import NerRawOutput, TripleRawOutput
    entities, triples = {}, {}
    for key, row in rows.items():
        subject = "item " + hashlib.sha256(row["content"].encode()).hexdigest()[:12]
        entities[key] = NerRawOutput(key, "", [subject, "memory"], {})
        triples[key] = TripleRawOutput(key, "", [[subject, "belongs to", "memory"]], {})
    return entities, triples


def make_graph(root, encoder=None):
    store = Store(root / "memory.db", encoder=encoder or Encoder())
    graph = Graph(store, root / "graph", "http://127.0.0.1:1/v1", "recovery-test")
    return store, graph


def write_memory(store, text, node="first"):
    store.enqueue(text, "file:///recovery/" + node, node)
    assert store.process_one()


def crash_writer(root, phase):
    from hipporag.information_extraction.openie_openai import OpenIE
    with patch.object(OpenIE, "batch_openie", fake_openie):
        store, graph = make_graph(root)
        graph.initialize()
        write_memory(store, "The first memory belongs to the initial version.")
        graph.index_one()
        if phase in ("delete_entities", "metadata"):
            write_memory(store, "The replacement memory has different entities.")
        else:
            write_memory(store, "The second memory is an additional record.", "second")

        def die(*_args, **_kwargs):
            (root / "killed-at").write_text(phase)
            os.kill(os.getpid(), signal.SIGKILL)

        if phase == "before_graph":
            graph.rag.save_igraph = die
        elif phase == "delete_entities":
            original = graph.rag.entity_embedding_store.delete
            def partial_delete(*args, **kwargs):
                original(*args, **kwargs)
                die()
            graph.rag.entity_embedding_store.delete = partial_delete
        elif phase == "metadata":
            def partial_metadata():
                Path(graph.rag.chunk_metadata_path).write_text('{"incomplete":')
                die()
            graph.rag._save_chunk_metadata = partial_metadata
        elif phase == "before_sql":
            graph._commit_operation = die
        elif phase == "after_sql":
            original = graph._commit_operation
            def committed(*args, **kwargs):
                original(*args, **kwargs)
                die()
            graph._commit_operation = committed
        else:
            raise ValueError(phase)
        graph.index_one()
        raise AssertionError("Crash hook was not reached")


def crash_restore(root):
    original = os.replace
    def replace_then_die(source, target):
        original(source, target)
        if str(source).endswith(".restore"):
            os.kill(os.getpid(), signal.SIGKILL)
    _, graph = make_graph(root, Encoder(allowed=False))
    with patch("batbox_memory.graph_checkpoint.os.replace", replace_then_die):
        graph.initialize()
    raise AssertionError("Recovery crash hook was not reached")


class GraphRecoveryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def crash(self, phase):
        result = subprocess.run(
            [sys.executable, "-m", "tests.memory.test_graph_recovery", "--kill-writer", str(self.root), phase],
            cwd=Path(__file__).resolve().parents[2], capture_output=True, text=True, timeout=90,
            env=dict(os.environ, CUDA_VISIBLE_DEVICES="", OMP_NUM_THREADS="2"))
        self.assertEqual(result.returncode, -signal.SIGKILL, result.stderr[-6000:])
        self.assertEqual((self.root / "killed-at").read_text(), phase)
        self.assertTrue((self.root / "graph/pending-index.json").exists())

    def cache_rows(self):
        with sqlite3.connect(self.root / "memory.db") as conn:
            return dict(conn.execute("SELECT cache_key,vector FROM embedding_cache"))

    def assert_consistent(self, graph, count):
        rag = graph.rag
        rag.prepare_retrieval_objects()
        self.assertEqual(len(rag.chunk_embedding_store.get_all_ids()), count)
        self.assertEqual(rag.graph.vcount(), len(rag.entity_embedding_store.get_all_ids()) + count)
        self.assertEqual(set(rag.fact_embedding_store.get_all_texts()), set(rag.proc_triples_to_docs))
        with graph.store.connect() as conn:
            rows = conn.execute("SELECT g.content_hash,p.content_hash FROM graph_index g JOIN memory_provenance p ON p.node_id=g.node_id").fetchall()
        self.assertEqual(len(rows), count)
        self.assertTrue(all(row[0] == row[1] for row in rows))
        self.assertFalse(graph.checkpoint.journal.exists())
        self.assertEqual(graph.status["state"], "ready")

    def test_sigkill_insert_before_graph_and_around_sql_commit(self):
        from hipporag.information_extraction.openie_openai import OpenIE
        for phase in ("before_graph", "before_sql", "after_sql"):
            with self.subTest(phase=phase), tempfile.TemporaryDirectory() as root:
                self.root = Path(root)
                self.crash(phase)
                before = self.cache_rows()
                store, graph = make_graph(self.root, Encoder(allowed=False))
                try:
                    with patch.object(OpenIE, "batch_openie", side_effect=AssertionError("Recovery repeated completed extraction")):
                        graph.initialize()
                    self.assert_consistent(graph, 2)
                    self.assertEqual(before, self.cache_rows())
                    self.assertEqual(graph.status["replayed_operations"], 1)
                finally:
                    graph.close()

    def test_sigkill_replace_during_delete_and_metadata_write(self):
        from hipporag.information_extraction.openie_openai import OpenIE
        for phase in ("delete_entities", "metadata"):
            with self.subTest(phase=phase), tempfile.TemporaryDirectory() as root:
                self.root = Path(root)
                self.crash(phase)
                before = self.cache_rows()
                store, graph = make_graph(self.root)
                try:
                    with patch.object(OpenIE, "batch_openie", fake_openie):
                        graph.initialize()
                    self.assert_consistent(graph, 1)
                    after = self.cache_rows()
                    self.assertTrue(all(after[key] == value for key, value in before.items()))
                    self.assertIn("replacement memory", next(iter(graph.rag.chunk_embedding_store.get_all_texts())))
                    self.assertEqual(len(graph.rag.fact_embedding_store.get_all_ids()), 1)
                finally:
                    graph.close()

    def test_second_sigkill_during_restore_preserves_replay_and_caches(self):
        from hipporag.information_extraction.openie_openai import OpenIE
        self.crash("before_graph")
        before = self.cache_rows()
        result = subprocess.run(
            [sys.executable, "-m", "tests.memory.test_graph_recovery", "--kill-restore", str(self.root)],
            cwd=Path(__file__).resolve().parents[2], capture_output=True, text=True, timeout=90)
        self.assertEqual(result.returncode, -signal.SIGKILL, result.stderr[-6000:])
        _, graph = make_graph(self.root, Encoder(allowed=False))
        try:
            with patch.object(OpenIE, "batch_openie", side_effect=AssertionError("Recovery lost extraction results")):
                graph.initialize()
            self.assert_consistent(graph, 2)
            self.assertEqual(before, self.cache_rows())
        finally:
            graph.close()

    def test_legacy_stale_graph_rebuild_reuses_persisted_caches(self):
        from hipporag.information_extraction.openie_openai import OpenIE
        store, graph = make_graph(self.root)
        with patch.object(OpenIE, "batch_openie", fake_openie):
            graph.initialize()
            write_memory(store, "A legacy graph interrupted between writes.")
            graph.index_one()
        path = graph.rag._graph_pickle_filename
        graph.rag.graph.delete_vertices([0])
        graph.rag.graph.write_pickle(path)
        graph.close()
        before = self.cache_rows()
        _, graph = make_graph(self.root, Encoder(allowed=False))
        try:
            with patch.object(OpenIE, "batch_openie", side_effect=AssertionError("Extraction cache discarded")):
                graph.initialize()
            self.assert_consistent(graph, 1)
            self.assertEqual(graph.status["graph_rebuilds"], 1)
            self.assertEqual(before, self.cache_rows())
        finally:
            graph.close()

    def test_shutdown_joins_inflight_work_before_closing_graph(self):
        from batbox_memory.__main__ import Service
        service = Service.__new__(Service)
        service.stop, service.config, service.graph = threading.Event(), {}, Mock()
        finished = threading.Event()
        def worker():
            service.stop.wait()
            time.sleep(0.05)
            finished.set()
        service.threads = [threading.Thread(target=worker)]
        service.threads[0].start()
        service.close()
        self.assertTrue(finished.is_set())
        service.graph.close.assert_called_once()


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--kill-writer":
        crash_writer(Path(sys.argv[2]), sys.argv[3])
    elif len(sys.argv) > 1 and sys.argv[1] == "--kill-restore":
        crash_restore(Path(sys.argv[2]))
    else:
        unittest.main()
