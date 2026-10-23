"""Durable memory queue, provenance, CPU embeddings, and incremental ingestion."""
from __future__ import annotations

import hashlib
import json
import re
import sqlite3
import subprocess
import threading
import time
from datetime import datetime, timezone
from pathlib import Path
from urllib.parse import urlparse
from urllib.parse import quote


def timestamp():
    return datetime.now(timezone.utc).isoformat()


def digest(text):
    return hashlib.sha256(text.encode()).hexdigest()


def chunks(text, size=180, overlap=30):
    text = re.sub(r"\A---\s*\n.*?\n---\s*\n", "", text, flags=re.S)
    words = text.split()
    return [" ".join(words[i:i + size]) for i in range(0, len(words), size - overlap)]


class Store:
    def __init__(self, path, model="BAAI/bge-small-en-v1.5", model_path=None, encoder=None):
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.model = model
        self.model_path = model_path
        self.encoder = encoder
        self.encode_lock = threading.Lock()
        if self.path.exists() and not self.path.with_suffix(".pre-batbox.db").exists():
            with sqlite3.connect(self.path) as old, sqlite3.connect(self.path.with_suffix(".pre-batbox.db")) as backup:
                old.backup(backup)
        with self.connect() as c:
            c.executescript("""
                PRAGMA journal_mode=WAL;
                CREATE TABLE IF NOT EXISTS nodes(id TEXT PRIMARY KEY, content TEXT, source TEXT, timestamp DATETIME);
                CREATE TABLE IF NOT EXISTS embeddings(node_id TEXT, vector TEXT, chunk_idx INTEGER);
                CREATE TABLE IF NOT EXISTS edges(source_id TEXT, target_id TEXT, weight FLOAT, relation TEXT);
                CREATE TABLE IF NOT EXISTS memory_jobs(
                    job_id INTEGER PRIMARY KEY AUTOINCREMENT, node_id TEXT NOT NULL,
                    content_hash TEXT NOT NULL, payload TEXT NOT NULL,
                    state TEXT NOT NULL DEFAULT 'pending', attempts INTEGER NOT NULL DEFAULT 0,
                    next_attempt REAL NOT NULL DEFAULT 0, error TEXT,
                    created_at TEXT NOT NULL, finished_at TEXT);
                CREATE TABLE IF NOT EXISTS memory_provenance(
                    node_id TEXT PRIMARY KEY, source_uri TEXT NOT NULL, kind TEXT NOT NULL,
                    captured_at TEXT NOT NULL, content_hash TEXT NOT NULL, active INTEGER NOT NULL DEFAULT 1);
                CREATE TABLE IF NOT EXISTS memory_chunks(
                    node_id TEXT NOT NULL, chunk_idx INTEGER NOT NULL, content TEXT NOT NULL,
                    content_hash TEXT NOT NULL, model TEXT NOT NULL, PRIMARY KEY(node_id,chunk_idx));
                CREATE TABLE IF NOT EXISTS embedding_cache(
                    cache_key TEXT PRIMARY KEY, model TEXT NOT NULL, vector TEXT NOT NULL, created_at TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS memory_counters(name TEXT PRIMARY KEY, value INTEGER NOT NULL);
                CREATE TABLE IF NOT EXISTS web_cache(
                    cache_key TEXT PRIMARY KEY, payload TEXT NOT NULL, expires REAL NOT NULL);
                CREATE TABLE IF NOT EXISTS graph_index(
                    node_id TEXT PRIMARY KEY, content_hash TEXT NOT NULL, chunks TEXT NOT NULL, indexed_at TEXT NOT NULL);
                CREATE INDEX IF NOT EXISTS jobs_state_due ON memory_jobs(state,next_attempt,job_id);
                CREATE INDEX IF NOT EXISTS embeddings_node ON embeddings(node_id);
            """)
            # A process interruption must leave work retryable, never silently complete.
            c.execute("UPDATE memory_jobs SET state='pending' WHERE state='processing'")
            if "metadata" not in {r[1] for r in c.execute("PRAGMA table_info(memory_provenance)")}:
                c.execute("ALTER TABLE memory_provenance ADD COLUMN metadata TEXT NOT NULL DEFAULT '{}'")
            if "priority" not in {r[1] for r in c.execute("PRAGMA table_info(memory_jobs)")}:
                c.execute("ALTER TABLE memory_jobs ADD COLUMN priority INTEGER NOT NULL DEFAULT 0")

    def connect(self):
        c = sqlite3.connect(self.path, timeout=30)
        c.row_factory = sqlite3.Row
        c.execute("PRAGMA busy_timeout=30000")
        return c

    def count(self, name, delta=1):
        with self.connect() as c:
            c.execute("INSERT INTO memory_counters VALUES (?,?) ON CONFLICT(name) DO UPDATE SET value=value+excluded.value", (name, delta))

    def enqueue(self, content, source, node_id=None, kind="observation", captured_at=None, metadata=None):
        if not isinstance(content, str) or not content.strip():
            raise ValueError("content must be a non-empty string")
        if len(content.encode()) > 512 * 1024:
            raise ValueError("memory exceeds 512 KiB; submit smaller source passages")
        if not isinstance(source, str) or not source.strip():
            raise ValueError("source is required (URL, file URI, or transcript URI)")
        if source.startswith("/"):
            source = Path(source).resolve().as_uri()
        if source.startswith("autonomous:"):
            source = "transcript://batbox/" + (node_id or digest(content))
            kind = "generated_observation"
        if urlparse(source).scheme not in {"http", "https", "file", "transcript"}:
            raise ValueError("source must use http, https, file, or transcript URI")
        node_id = node_id or "memory:" + digest(source + "\n" + content)
        metadata = dict(metadata or {})
        payload = dict(id=node_id, content=content, source=source, kind=kind,
                       captured_at=captured_at or timestamp(), content_hash=digest(content), metadata=metadata)
        with self.connect() as c:
            c.execute("BEGIN IMMEDIATE")
            row = c.execute("SELECT job_id,state,content_hash,payload FROM memory_jobs WHERE node_id=? ORDER BY job_id DESC LIMIT 1", (node_id,)).fetchone()
            old = json.loads(row["payload"]) if row else {}
            old_meta = {k: v for k, v in old.get("metadata", {}).items() if k != "imported_at"}
            new_meta = {k: v for k, v in metadata.items() if k != "imported_at"}
            if row is None or row["content_hash"] != payload["content_hash"] or old.get("source") != source or old.get("kind") != kind or old_meta != new_meta:
                payload["metadata"]["imported_at"] = timestamp()
                cur = c.execute("INSERT INTO memory_jobs(node_id,content_hash,payload,created_at,priority) VALUES (?,?,?,?,?)",
                                (node_id, payload["content_hash"], json.dumps(payload), timestamp(), int(metadata.get("priority", 0))))
                row = {"job_id": cur.lastrowid, "state": "pending"}
        return {"id": node_id, "job_id": row["job_id"], "status": row["state"], "content_hash": payload["content_hash"], "source": source}

    def ingest(self, roots):
        submitted, errors = [], []
        for root in roots:
            options = root if isinstance(root, dict) else {"path": root}
            path = Path(options["path"]).expanduser()
            # Preserve legacy notes accidentally saved with a literal trailing backtick.
            files = sorted(f for f in (path.rglob("*") if options.get("recursive") else path.iterdir()) if f.name.endswith((".md", ".md`"))) if path.is_dir() else [path]
            for f in files:
                if not f.is_file():
                    continue
                try:
                    if f.stat().st_size > 512 * 1024:
                        raise ValueError("source larger than 512 KiB")
                    body = f.read_text(encoding="utf-8")
                    if not body.strip():
                        continue
                    uri = f.resolve().as_uri()
                    metadata = {k: options[k] for k in ("source_author", "historical", "priority", "source_commit", "import_group") if k in options}
                    metadata["local_uri"] = uri
                    source = options.get("source_base", "").rstrip("/") + "/" + quote(f.relative_to(path).as_posix()) if options.get("source_base") else uri
                    if options.get("historical"):
                        dates = re.findall(r"\b20\d{2}-\d{2}-\d{2}\b", f.name)
                        if dates:
                            metadata["source_date"] = dates[0]
                        elif options.get("git_root"):
                            result = subprocess.run(["git", "-C", options["git_root"], "log", "-1", "--format=%cI", "--", str(f)], capture_output=True, text=True, timeout=5)
                            metadata["source_date"] = result.stdout.strip() or None
                        metadata["historical_warning"] = "Historical imported memory. Describes past state; server details and plans require current verification. Authorship is not the engine's identity."
                    submitted.append(self.enqueue(body, source, "file:" + digest(uri), options.get("kind", "historical_import" if options.get("historical") else "source_document"), metadata=metadata))
                except (OSError, ValueError, UnicodeError) as exc:
                    errors.append({"source": str(f), "error": str(exc)})
        return {"submitted": len(submitted), "pending": sum(x["status"] != "ready" for x in submitted), "errors": errors}

    def encode(self, texts, query=False):
        import numpy as np
        # Task type is part of the key: query and passage preprocessing can differ.
        prepared = [("Represent this sentence for searching relevant passages: " + t) if query else t for t in texts]
        keys = [digest(self.model + "\0" + t) for t in prepared]
        vectors = [None] * len(keys)
        with self.encode_lock:
            missing = []
            with self.connect() as c:
                for i, key in enumerate(keys):
                    row = c.execute("SELECT vector FROM embedding_cache WHERE cache_key=?", (key,)).fetchone()
                    if row:
                        vectors[i] = json.loads(row[0])
                    else:
                        missing.append(i)
            self.count("embedding_cache_hits", len(keys) - len(missing))
            if missing:
                if self.encoder is None:
                    from fastembed import TextEmbedding
                    options = dict(model_name=self.model, providers=["CPUExecutionProvider"], threads=4)
                    if self.model_path:
                        options["specific_model_path"] = self.model_path
                    self.encoder = TextEmbedding(**options)
                encoded = list(self.encoder.embed([prepared[i] for i in missing]))
                if len(encoded) != len(missing):
                    raise RuntimeError("encoder returned wrong number of vectors")
                with self.connect() as c:
                    for i, vec in zip(missing, encoded):
                        arr = np.asarray(vec, dtype=np.float32)
                        if arr.ndim != 1 or not np.isfinite(arr).all() or np.linalg.norm(arr) == 0:
                            raise RuntimeError("encoder returned invalid vector")
                        vectors[i] = (arr / np.linalg.norm(arr)).tolist()
                        c.execute("INSERT OR REPLACE INTO embedding_cache VALUES (?,?,?,?)",
                                  (keys[i], self.model, json.dumps(vectors[i]), timestamp()))
                self.count("embeddings_computed", len(missing))
        return np.asarray(vectors, dtype=np.float32)

    def process_one(self):
        with self.connect() as c:
            c.execute("BEGIN IMMEDIATE")
            row = c.execute("SELECT * FROM memory_jobs WHERE state IN ('pending','retry') AND next_attempt<=? ORDER BY priority DESC,job_id DESC LIMIT 1", (time.time(),)).fetchone()
            if row is None:
                return False
            c.execute("UPDATE memory_jobs SET state='processing',attempts=attempts+1 WHERE job_id=?", (row["job_id"],))
        payload = json.loads(row["payload"])
        try:
            with self.connect() as c:
                newer = c.execute("SELECT 1 FROM memory_jobs WHERE node_id=? AND job_id>? LIMIT 1", (row["node_id"], row["job_id"])).fetchone()
                if newer:
                    c.execute("UPDATE memory_jobs SET state='superseded',finished_at=? WHERE job_id=?", (timestamp(), row["job_id"]))
                    return True
            pieces = chunks(payload["content"])
            vectors = self.encode(pieces)
            with self.connect() as c:
                c.execute("INSERT INTO nodes VALUES (?,?,?,?) ON CONFLICT(id) DO UPDATE SET content=excluded.content,source=excluded.source,timestamp=excluded.timestamp",
                          (payload["id"], payload["content"], payload["source"], payload["captured_at"]))
                c.execute("INSERT OR REPLACE INTO memory_provenance(node_id,source_uri,kind,captured_at,content_hash,active,metadata) VALUES (?,?,?,?,?,1,?)",
                          (payload["id"], payload["source"], payload["kind"], payload["captured_at"], payload["content_hash"], json.dumps(payload.get("metadata", {}))))
                c.execute("DELETE FROM embeddings WHERE node_id=?", (payload["id"],))
                c.execute("DELETE FROM memory_chunks WHERE node_id=?", (payload["id"],))
                for i, (piece, vector) in enumerate(zip(pieces, vectors)):
                    c.execute("INSERT INTO embeddings VALUES (?,?,?)", (payload["id"], json.dumps(vector.tolist()), i))
                    c.execute("INSERT INTO memory_chunks VALUES (?,?,?,?,?)", (payload["id"], i, piece, digest(piece), self.model))
                c.execute("DELETE FROM edges WHERE source_id=? AND relation='wiki-link'", (payload["id"],))
                for link in re.findall(r"\[\[([^]|#]+)(?:[^]]*)\]\]", payload["content"]):
                    c.execute("INSERT INTO edges VALUES (?,?,?,?)", (payload["id"], link.strip(), 1.0, "wiki-link"))
                c.execute("UPDATE memory_jobs SET state='ready',finished_at=?,error=NULL WHERE job_id=?", (timestamp(), row["job_id"]))
            return True
        except Exception as exc:
            attempts = row["attempts"] + 1
            with self.connect() as c:
                c.execute("UPDATE memory_jobs SET state='retry',next_attempt=?,error=? WHERE job_id=?",
                          (time.time() + min(300, 2 ** min(attempts, 8)), str(exc)[:1000], row["job_id"]))
            raise

    def recall(self, query, limit=5):
        import numpy as np
        if not query.strip():
            raise ValueError("query must not be empty")
        limit = min(20, max(1, int(limit)))
        q = self.encode([query], query=True)[0]
        with self.connect() as c:
            rows = c.execute("""SELECT n.id,n.source,p.kind,p.captured_at,p.content_hash,p.metadata,c.content,e.vector
                FROM memory_chunks c JOIN embeddings e ON e.node_id=c.node_id AND e.chunk_idx=c.chunk_idx
                JOIN nodes n ON n.id=c.node_id JOIN memory_provenance p ON p.node_id=n.id
                WHERE p.active=1 AND c.model=?""", (self.model,)).fetchall()
        scored = []
        for row in rows:
            vec = np.asarray(json.loads(row["vector"]), dtype=np.float32)
            if vec.shape == q.shape:
                scored.append((float(np.dot(q, vec)), row))
        scored.sort(key=lambda pair: pair[0], reverse=True)
        results, seen = [], set()
        for score, row in scored:
            if row["id"] in seen:
                continue
            seen.add(row["id"])
            results.append({k: row[k] for k in ("id", "source", "kind", "captured_at", "content_hash", "content")} | {"score": score, **json.loads(row["metadata"])})
            if len(results) >= limit:
                break
        return {"query": query, "results": results, "backend": "cpu-vector"}

    def cache_get(self, key):
        with self.connect() as c:
            row = c.execute("SELECT payload FROM web_cache WHERE cache_key=? AND expires>?", (key, time.time())).fetchone()
        if row:
            self.count("web_cache_hits")
            return json.loads(row[0]) | {"cached": True}
        return None

    def cache_put(self, key, payload, ttl):
        with self.connect() as c:
            c.execute("INSERT OR REPLACE INTO web_cache VALUES (?,?,?)", (key, json.dumps(payload), time.time() + ttl))

    def stats(self):
        with self.connect() as c:
            counts = {name: c.execute(f"SELECT COUNT(*) FROM {name}").fetchone()[0] for name in ("nodes", "embeddings", "edges", "embedding_cache", "graph_index")}
            jobs = dict(c.execute("SELECT state,count(*) FROM memory_jobs GROUP BY state").fetchall())
            counters = dict(c.execute("SELECT name,value FROM memory_counters").fetchall())
            errors = [dict(r) for r in c.execute("SELECT job_id,node_id,attempts,error FROM memory_jobs WHERE state='retry' ORDER BY job_id DESC LIMIT 5")]
        return counts | {"queue": jobs, "counters": counters, "recent_errors": errors, "embedding_model": self.model, "embedding_device": "CPUExecutionProvider", "database": str(self.path)}
