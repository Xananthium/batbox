"""Read-only CPU checks for the final memory restoration; no HTTP/model calls."""
import argparse
import hashlib
import json
from pathlib import Path
import sqlite3
from datetime import datetime, timezone


def state(database="/home/isnotreal/memory.db"):
    with sqlite3.connect(f"file:{database}?mode=ro", uri=True) as conn:
        conn.row_factory = sqlite3.Row
        historical = [dict(row) for row in conn.execute("""
            SELECT json_extract(p.metadata,'$.source_author') AS author,
                   COUNT(*) AS records,
                   SUM(EXISTS(SELECT 1 FROM embeddings e WHERE e.node_id=p.node_id)) AS embedded,
                   SUM(COALESCE(g.content_hash=p.content_hash,0)) AS graph_ready
            FROM memory_provenance p LEFT JOIN graph_index g ON g.node_id=p.node_id
            WHERE p.active=1 AND json_extract(p.metadata,'$.historical')=1
            GROUP BY author ORDER BY author""")]
        snapshots = []
        for name in ("server-context.json", "program-map.json"):
            path = Path("/home/isnotreal/.batbox/autonomous") / name
            expected = hashlib.sha256(path.read_bytes()).hexdigest()
            row = conn.execute("""SELECT p.node_id,p.content_hash,g.content_hash AS graph_hash
                FROM memory_provenance p LEFT JOIN graph_index g ON g.node_id=p.node_id
                WHERE p.source_uri=? AND p.active=1""", (path.as_uri(),)).fetchone()
            snapshots.append({"source": path.as_uri(), "expected_hash": expected,
                              "stored_hash": row["content_hash"] if row else None,
                              "graph_hash": row["graph_hash"] if row else None,
                              "current_file_graphed": bool(row and row["content_hash"] == row["graph_hash"] == expected)})
        mission = conn.execute("""SELECT j.job_id,j.state,p.source_uri,p.content_hash,
            COALESCE(g.content_hash=p.content_hash,0) AS graph_ready
            FROM memory_jobs j LEFT JOIN memory_provenance p ON p.node_id=j.node_id
            LEFT JOIN graph_index g ON g.node_id=j.node_id WHERE j.job_id=242""").fetchone()
        pending_graph = conn.execute("""SELECT COUNT(*) FROM memory_provenance p
            LEFT JOIN graph_index g ON g.node_id=p.node_id WHERE p.active=1
            AND (g.node_id IS NULL OR g.content_hash!=p.content_hash)""").fetchone()[0]
        result = {"observed_at": datetime.now(timezone.utc).isoformat(),
                  "historical": historical, "current_snapshots": snapshots,
                  "mission": dict(mission) if mission else None,
                  "queue": dict(conn.execute("SELECT state,COUNT(*) FROM memory_jobs GROUP BY state")),
                  "graph_index_rows": conn.execute("SELECT COUNT(*) FROM graph_index").fetchone()[0],
                  "other_graph_work_pending": pending_graph}
    result["requested_targets_complete"] = (
        sum(row["records"] for row in historical) == 57
        and all(row["embedded"] == row["records"] == row["graph_ready"] for row in historical)
        and all(row["current_file_graphed"] for row in snapshots)
        and bool(mission and mission["state"] == "ready" and mission["graph_ready"]))
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = state()
    text = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(text)
    print(text)
