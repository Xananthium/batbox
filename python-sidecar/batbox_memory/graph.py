"""Official HippoRAG adapter, sharing the CPU embedding cache and local LLM."""
import json
import logging
import os
import threading
import time
from pathlib import Path

from .graph_checkpoint import IndexCheckpoint
from .store import timestamp

LOG = logging.getLogger(__name__)


class Graph:
    def __init__(self, store, directory, backend, model):
        self.store = store
        self.directory = Path(directory)
        self.backend = backend
        self.model = model
        self.lock = threading.Lock()
        self.rag = None
        self.llm = None
        self.checkpoint = IndexCheckpoint(self.directory)
        self.status = {"state": "starting", "backend": "HippoRAG", "nodes": 0, "edges": 0, "last_error": None}

    def initialize(self):
        # This environment installs CPU PyTorch, and the encoder explicitly uses ONNX CPU.
        os.environ["CUDA_VISIBLE_DEVICES"] = ""
        os.environ["LITELLM_LOCAL_MODEL_COST_MAP"] = "True"
        from hipporag import HippoRAG
        from hipporag.embedding_model.base import BaseEmbeddingModel, EmbeddingConfig
        from hipporag.llm.openai_gpt import CacheOpenAI
        from hipporag.utils.config_utils import BaseConfig
        from hipporag.utils.state_utils import StateConsistencyError
        import torch
        torch.set_num_threads(4)
        owner = self.store

        class CachedCPUEmbedding(BaseEmbeddingModel):
            def __init__(self, config):
                super().__init__(config)
                self.embedding_dim = 384
                self.embedding_config = EmbeddingConfig.from_dict({"model": owner.model, "device": "cpu"})

            def batch_encode(self, texts, **kwargs):
                # HippoRAG supplies an instruction for queries; include it in cache identity.
                instruction = kwargs.get("instruction", "")
                return owner.encode([f"{instruction} {text}".strip() for text in texts])

        class LocalJSONLLM(CacheOpenAI):
            def infer(self, messages, **kwargs):
                system = messages[0].get("content", "")
                field = "named_entities" if "extract named entities" in system else "triples" if "construct an RDF" in system else None
                if field:
                    item = {"type": "string"} if field == "named_entities" else {
                        "type": "array", "items": {"type": "string"}, "minItems": 3, "maxItems": 3}
                    kwargs["response_format"] = {"type": "json_schema", "json_schema": {
                        "name": field, "strict": True, "schema": {
                            "type": "object", "properties": {field: {"type": "array", "items": item}},
                            "required": [field], "additionalProperties": False}}}
                return super().infer(messages=messages, **kwargs)

        config = BaseConfig(
            save_dir=str(self.directory), llm_name=self.model, llm_base_url=self.backend,
            embedding_model_name=self.store.model, embedding_provider="openai",
            openie_max_workers=1, openie_ner_max_tokens=512, openie_triple_max_tokens=1536,
            max_new_tokens=512, max_retry_attempts=1, retrieval_top_k=5,
            synonymy_edge_topk=32, synonymy_edge_query_batch_size=128,
            synonymy_edge_key_batch_size=1024,
        )
        llm = LocalJSONLLM(str(self.directory / "llm-cache"), config, max_retries=1)
        llm.llm_config.generate_params["extra_body"] = {"chat_template_kwargs": {"enable_thinking": False}}
        llm.openai_client = llm.openai_client.with_options(timeout=60, max_retries=1)
        self.llm = llm
        with self.lock:
            operation = self.checkpoint.read()
            if operation:
                self.status.update(state="recovering")
                self.checkpoint.restore(operation)
                config.force_index_from_scratch = operation["kind"] == "rebuild"

            def create_rag():
                return HippoRAG(global_config=config, embedding_model=CachedCPUEmbedding(config), extraction_llm=llm,
                                index_identity="batbox-cpu-bge-v1-local-openie-json-v1")

            self.rag = create_rag()
            if operation and operation["kind"] == "rebuild":
                self._rebuild_graph()
            else:
                try:
                    self.rag.prepare_retrieval_objects()
                except StateConsistencyError as exc:
                    # A pre-journal interrupted write can leave a stale graph.
                    # Configuration/provenance failures are not safe to rewrite.
                    repairable = ("Graph/store node count mismatch", "Graph state is missing",
                                  "Graph vertices are missing", "Graph vertex IDs do not match")
                    if operation or not str(exc).startswith(repairable):
                        raise
                    self.status.update(state="recovering", recovery_reason=str(exc))
                    self.checkpoint.begin(self.rag, {"kind": "rebuild", "created_at": timestamp(), "reason": str(exc)})
                    self.rag.close()
                    config.force_index_from_scratch = True
                    self.rag = create_rag()
                    self._rebuild_graph()
                if operation:
                    self._apply_operation(operation)
                    self.status["replayed_operations"] = self.status.get("replayed_operations", 0) + 1
            self.status.update(state="ready", last_error=None)
            self.refresh_stats()

    def _rebuild_graph(self):
        # Official rebuild path reuses persisted chunks, OpenIE and embeddings.
        self.rag.index([])
        if not self.rag.chunk_embedding_store.get_all_ids():
            self.rag.save_igraph()
        self.rag.global_config.force_index_from_scratch = False
        self.rag.prepare_retrieval_objects()
        self.checkpoint.sync_index(self.rag)
        self.checkpoint.finish()
        self.status.update(last_recovered_at=timestamp(),
                           graph_rebuilds=self.status.get("graph_rebuilds", 0) + 1)

    def refresh_stats(self):
        if self.rag:
            self.status.update(nodes=self.rag.graph.vcount(), edges=self.rag.graph.ecount(),
                               passages=len(self.rag.chunk_embedding_store.get_all_ids()),
                               facts=len(self.rag.fact_embedding_store.get_all_ids()),
                               entities=len(self.rag.entity_embedding_store.get_all_ids()))

    def index_one(self):
        with self.store.connect() as c:
            c.execute("BEGIN")
            node = c.execute("""SELECT n.*,p.content_hash,p.kind,p.captured_at,p.metadata FROM nodes n
                JOIN memory_provenance p ON p.node_id=n.id LEFT JOIN graph_index g ON g.node_id=n.id
                WHERE p.active=1 AND (g.node_id IS NULL OR g.content_hash!=p.content_hash)
                ORDER BY COALESCE(json_extract(p.metadata,'$.priority'),0) DESC,n.timestamp DESC LIMIT 1""").fetchone()
            if node is None:
                return False
            pieces = [r[0] for r in c.execute("SELECT content FROM memory_chunks WHERE node_id=? ORDER BY chunk_idx", (node["id"],))]
            old = c.execute("SELECT chunks FROM graph_index WHERE node_id=?", (node["id"],)).fetchone()
        with self.lock:
            self.status.update(state="indexing", current_source=node["source"])
            # Include a provenance header to distinguish identical text captured from distinct sources.
            metadata = json.loads(node["metadata"])
            historical = f"Historical author: {metadata.get('source_author')}; source date: {metadata.get('source_date')}. {metadata.get('historical_warning')}\n" if metadata.get("historical") else ""
            docs = [f"Source: {node['source']}\nKind: {node['kind']}\n{historical}\n{p}" for p in pieces]
            operation = {"kind": "index", "created_at": timestamp(), "node_id": node["id"],
                         "content_hash": node["content_hash"], "old_docs": json.loads(old[0]) if old else [],
                         "chunks": [{"content": doc, "source_id": node["id"], "metadata": {
                "source": node["source"], "kind": node["kind"], "captured_at": node["captured_at"],
                "content_hash": node["content_hash"], "chunk_idx": i, **metadata,
            }} for i, doc in enumerate(docs)]}
            operation = self.checkpoint.begin(self.rag, operation)
            self._apply_operation(operation)
            self.refresh_stats()
            self.status.update(state="ready", last_error=None, last_indexed_at=timestamp())
        return True

    def _apply_operation(self, operation):
        from hipporag.utils.misc_utils import Chunk
        docs = [chunk["content"] for chunk in operation["chunks"]]
        retained = set(docs)
        removed = [doc for doc in operation["old_docs"] if doc not in retained]
        if removed:
            self.rag.delete(removed)
        cached = self.checkpoint.recovered_extractions(self.rag, docs)
        if cached:
            existing, _ = self.rag.load_existing_openie([])
            by_id = {row["idx"]: row for row in existing}
            for row in cached:
                by_id.setdefault(row["idx"], row)
            self.rag._openie_info = list(by_id.values())
        self.rag.index([Chunk(**chunk) for chunk in operation["chunks"]])
        self.rag.prepare_retrieval_objects()
        self.checkpoint.sync_index(self.rag)
        self._commit_operation(operation, docs)
        self.checkpoint.finish()

    def _commit_operation(self, operation, docs):
        with self.store.connect() as c:
            c.execute("INSERT OR REPLACE INTO graph_index VALUES (?,?,?,?)", (
                operation["node_id"], operation["content_hash"], json.dumps(docs), timestamp()))

    def recall(self, query, limit):
        if self.status["state"] != "ready" or self.rag is None or not self.status.get("passages") or not self.lock.acquire(blocking=False):
            return None
        try:
            solutions = self.rag.retrieve([query], num_to_retrieve=limit)
            solution = solutions[0]
            results = []
            for doc, score, meta in zip(solution.docs, solution.doc_scores, solution.doc_metadata):
                results.append(dict(id=meta.get("source_id"), source=meta.get("source"),
                                    kind=meta.get("kind"), captured_at=meta.get("captured_at"),
                                    content_hash=meta.get("content_hash"), content=doc, score=float(score),
                                    **{k: v for k, v in meta.items() if k in {"source_author", "historical", "source_date", "imported_at", "local_uri", "source_commit", "import_group", "historical_warning"}}))
            self.store.count("hipporag_retrievals")
            return {"query": query, "results": results, "backend": "HippoRAG", "graph_seeds": solution.graph_seeds}
        except Exception as exc:
            self.status["last_retrieval_error"] = str(exc)[:1000]
            LOG.exception("HippoRAG retrieval failed; using CPU vector retrieval")
            return None
        finally:
            self.lock.release()

    def run(self, stop):
        while not stop.is_set():
            try:
                if self.rag is None:
                    self.initialize()
                if stop.is_set():
                    break
                if not self.index_one():
                    stop.wait(2)
                else:
                    # Release the graph lock and GPU between batches for interactive work.
                    stop.wait(0.5)
            except Exception as exc:
                self.status.update(state="retry", last_error=str(exc)[:1000], retry_at=time.time() + 30)
                LOG.exception("HippoRAG update failed; reloading its checkpoint before retry in 30s")
                self.close()
                stop.wait(30)

    def close(self):
        with self.lock:
            if self.rag is not None:
                self.rag.close()
                self.rag = None
            # The injected LLM is owned by this adapter, not by HippoRAG.
            if self.llm is not None:
                self.llm.close()
                self.llm = None
