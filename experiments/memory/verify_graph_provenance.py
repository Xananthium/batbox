"""Read-only check of requested sources against persisted HippoRAG chunk metadata."""
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import sqlite3
import time

ROOT = Path("/home/isnotreal")
SOURCES = [
    (ROOT / ".batbox/autonomous/server-context.json").as_uri(),
    (ROOT / ".batbox/autonomous/program-map.json").as_uri(),
    "https://digitaldisconnections.com/our-story.html",
]


def verify():
    config = json.loads((ROOT / ".batbox/memory-service.json").read_text())
    paths = list(Path(config["graph_directory"]).glob("*/chunk_metadata.json"))
    if len(paths) != 1:
        raise RuntimeError(f"Expected one active graph metadata file; found {len(paths)}")
    # Upstream writes this derived file in place; retry a simultaneous partial read.
    for attempt in range(10):
        try:
            metadata = json.loads(paths[0].read_text())
            break
        except json.JSONDecodeError:
            if attempt == 9:
                raise
            time.sleep(0.1)
    with sqlite3.connect("file:/home/isnotreal/memory.db?mode=ro", uri=True) as conn:
        conn.row_factory = sqlite3.Row
        rows = conn.execute("""SELECT p.*,g.content_hash graph_hash,g.chunks graph_chunks
            FROM memory_provenance p LEFT JOIN graph_index g ON p.node_id=g.node_id
            WHERE p.active=1 AND (json_extract(p.metadata,'$.historical')=1
                OR p.source_uri IN (?,?) OR p.node_id=(SELECT node_id FROM memory_jobs WHERE job_id=242))
            ORDER BY p.source_uri""", SOURCES[:2]).fetchall()
    results = []
    for row in rows:
        canonical = json.loads(row["metadata"])
        chunks = [m for m in metadata.values() if m.get("source_id") == row["node_id"]]
        expected_count = len(json.loads(row["graph_chunks"])) if row["graph_chunks"] else 0
        current_file = True
        if row["source_uri"] in SOURCES[:2]:
            current_file = hashlib.sha256(Path(row["source_uri"][7:]).read_bytes()).hexdigest() == row["content_hash"]
        valid = (row["content_hash"] == row["graph_hash"] and current_file
                 and len(chunks) == expected_count and bool(chunks))
        valid = valid and all(m.get("source") == row["source_uri"] and m.get("content_hash") == row["content_hash"]
                              and m.get("kind") == row["kind"] for m in chunks)
        if canonical.get("historical"):
            valid = valid and all(m.get("historical") is True and bool(m.get("imported_at"))
                and all(m.get(key) == canonical.get(key) and bool(m.get(key))
                        for key in ("source_author", "source_date", "source_commit", "historical_warning")) for m in chunks)
        results.append({"source": row["source_uri"], "author": canonical.get("source_author"),
                        "content_hash": row["content_hash"], "expected_chunks": expected_count,
                        "persisted_chunks": len(chunks), "passed": bool(valid)})
    return {"observed_at": datetime.now(timezone.utc).isoformat(), "sources": len(results),
            "passed_sources": sum(r["passed"] for r in results),
            "all_passed": len(results) == 60 and all(r["passed"] for r in results), "results": results}


if __name__ == "__main__":
    result = verify()
    Path(__file__).with_name("graph-provenance.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({k: v for k, v in result.items() if k != "results"}))
