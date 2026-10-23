"""Recover this pre-checkpoint interrupted insert, with explicit invariants.

Run only with the memory service stopped. No model or embedding calls are made.
The normal adapter's write-ahead checkpoints handle subsequent interruptions.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sqlite3
from datetime import datetime, timezone

import igraph
import pandas as pd

from batbox_memory.graph_checkpoint import atomic_json, sync_directory


def recover(directory, database):
    directory, database = Path(directory), Path(database)
    chunks_path = directory / "chunk_embeddings/vdb_chunk.parquet"
    metadata_path = directory / "chunk_metadata.json"
    chunk_rows = pd.read_parquet(chunks_path)
    entity_rows = pd.read_parquet(directory / "entity_embeddings/vdb_entity.parquet")
    metadata = json.loads(metadata_path.read_text())
    extracted = json.loads((directory / "openie_state.json").read_text())["docs"]
    graph = igraph.Graph.Read_Pickle(str(directory / "graph.pickle"))
    with sqlite3.connect(database) as conn:
        committed_rows = conn.execute("SELECT node_id,content_hash,chunks FROM graph_index").fetchall()
        pending_updates = conn.execute("SELECT COUNT(*) FROM graph_index g JOIN memory_provenance p ON p.node_id=g.node_id WHERE p.active=1 AND g.content_hash!=p.content_hash").fetchone()[0]
        unindexed = conn.execute("SELECT COUNT(*) FROM memory_provenance p LEFT JOIN graph_index g ON p.node_id=g.node_id WHERE p.active=1 AND g.node_id IS NULL").fetchone()[0]
    committed = {}
    for node, content_hash, docs in committed_rows:
        for doc in json.loads(docs):
            key = "chunk-" + hashlib.md5(doc.encode()).hexdigest()
            if key in committed:
                raise RuntimeError("Duplicate chunk across committed source records")
            committed[key] = doc
            if metadata.get(key, {}).get("source_id") != node or metadata[key].get("content_hash") != content_hash:
                raise RuntimeError("Committed metadata differs from SQLite source identity/hash")
    if not committed:
        raise RuntimeError("No committed source records; cannot infer a safe recovery")
    committed_ids = set(committed)
    stored_ids = set(chunk_rows.hash_id)
    if not committed_ids <= stored_ids:
        raise RuntimeError("Committed chunks are missing from the vector store")
    if {row["idx"] for row in extracted} != committed_ids:
        raise RuntimeError("Extraction state is not exactly the committed source set")
    if set(graph.vs["name"]) != committed_ids | set(entity_rows.hash_id):
        raise RuntimeError("Graph is not exactly the committed chunks plus current entities")
    for row in chunk_rows.itertuples():
        if row.hash_id in committed and row.content != committed[row.hash_id]:
            raise RuntimeError("Committed chunk text differs from its vector row")
    extra = stored_ids - committed_ids
    if not extra:
        raise RuntimeError("No incomplete insert to repair")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    archive = directory.parent / "recovery" / ("legacy-partial-insert-" + stamp)
    archive.mkdir(parents=True)
    # Preserve every affected original vector and metadata record before changing
    # the active derived store. The canonical SQLite data/cache is not modified.
    for source in (chunks_path, metadata_path):
        target = archive / source.name
        shutil.copy2(source, target)
        with target.open("rb") as stream:
            os.fsync(stream.fileno())
    result = {
        "recovered_at": stamp, "archive": str(archive),
        "graph_index_rows": len(committed_rows), "committed_chunks": len(committed),
        "deferred_uncommitted_chunks": len(extra), "metadata_mismatches": 0,
        "pending_source_updates": pending_updates, "unindexed_active_sources": unindexed,
        "nodes": graph.vcount(), "edges": graph.ecount(),
        "entities": len(entity_rows), "extraction_records": len(extracted),
        "canonical_sqlite_modified": False, "LLM_calls": 0,
    }
    atomic_json(archive / "receipt.json", result)
    temporary = chunks_path.with_suffix(".repair.parquet")
    chunk_rows[chunk_rows.hash_id.isin(committed_ids)].to_parquet(temporary, index=False)
    with temporary.open("rb") as stream:
        os.fsync(stream.fileno())
    os.replace(temporary, chunks_path)
    sync_directory(chunks_path.parent)
    atomic_json(metadata_path, {key: value for key, value in metadata.items() if key not in extra})
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", required=True)
    parser.add_argument("--database", required=True)
    args = parser.parse_args()
    os.umask(0o077)
    print(json.dumps(recover(args.directory, args.database), indent=2))
