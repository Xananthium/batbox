"""Check source attribution through memory recall; keep returned text private."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import time
import urllib.request

BASE = "http://127.0.0.1:8771"
HERE = Path(__file__).resolve().parent
PRIVATE = Path("/home/isnotreal/.batbox/diagnostics/memory-restoration-20261023")
CASES = (
    ("mission", "What is the Digital Disconnections mission and our story?", "https://digitaldisconnections.com/our-story.html", None),
    ("current_backend", "Current inference backend original GGUF PR141 release, model path, and 200000 context window", "file:///home/isnotreal/.batbox/autonomous/server-context.json", None),
    ("current_program", "BatBox current coordinator worker crash recovery durable sessions watchdog program map", "file:///home/isnotreal/.batbox/autonomous/program-map.json", None),
    ("historical_heather", "Heather historical Wave 3 supporting intelligence project notes from March 2026", None, "Heather"),
    ("historical_maya", "Maya historical reference notes about state privacy laws from March 2026", None, "Maya"),
)


def request(path, payload=None):
    data = None if payload is None else json.dumps(payload).encode()
    req = urllib.request.Request(BASE + path, data=data, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=650 if payload else 5) as response:
        return json.load(response)


def provenance(results, source, author):
    matches = [r for r in results if (r.get("source") == source if source else r.get("source_author") == author)]
    checked = []
    with sqlite3.connect("file:/home/isnotreal/memory.db?mode=ro", uri=True) as conn:
        conn.row_factory = sqlite3.Row
        for result in matches:
            row = conn.execute("SELECT * FROM memory_provenance WHERE source_uri=? AND active=1", (result["source"],)).fetchone()
            metadata = json.loads(row["metadata"]) if row else {}
            valid = bool(row and result.get("content_hash") == row["content_hash"] and result.get("kind") == row["kind"])
            if author:
                valid = valid and result.get("historical") is True and all(
                    result.get(key) == metadata.get(key) and bool(result.get(key))
                    for key in ("source_author", "source_date", "source_commit", "historical_warning"))
                # A scheduling-only re-ingest updates canonical import time without
                # changing the content hash or rebuilding an already indexed graph row.
                valid = valid and bool(result.get("imported_at"))
                valid = valid and "/blob/" + result.get("source_commit", "") + "/" in result["source"]
            elif source.startswith("file://"):
                valid = valid and result["content_hash"] == hashlib.sha256(Path(source[7:]).read_bytes()).hexdigest()
            checked.append({key: result.get(key) for key in (
                "source", "kind", "content_hash", "captured_at", "source_author", "source_date",
                "source_commit", "historical", "historical_warning", "imported_at")}
                | {"canonical_metadata_match": bool(valid),
                   "import_timestamp_matches_latest_ingest": result.get("imported_at") == metadata.get("imported_at")})
    return checked


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", choices=("cpu-vector", "HippoRAG"), required=True)
    parser.add_argument("--wait-targets", action="store_true", help="Wait for 57 historical records, mission, and current maps to be indexed.")
    parser.add_argument("--cases", nargs="+", choices=[case[0] for case in CASES], help="Limit live recall checks to selected sources.")
    args = parser.parse_args()
    os.umask(0o077)
    PRIVATE.mkdir(parents=True, mode=0o700, exist_ok=True)
    if args.wait_targets:
        from memory_restore_checks import state
        while True:
            progress = state()
            if progress["requested_targets_complete"]:
                break
            # Catch the final 0.5s ready interval without querying the LLM or
            # changing the indexer's scheduling; ordinary polling remains slow.
            almost_done = sum(r["graph_ready"] for r in progress["historical"]) >= 56
            time.sleep(0.1 if almost_done else 2)
    cases = [case for case in CASES if not args.cases or case[0] in args.cases]
    summary = {"started_at": datetime.now(timezone.utc).isoformat(), "requested_backend": args.backend,
               "selected_cases": [case[0] for case in cases], "cases": []}
    output = HERE / ("retrieval-" + args.backend.lower() + ".json")
    for label, query, source, author in cases:
        attempts = 0
        while True:
            health = request("/health")
            if args.backend == "HippoRAG" and health["hipporag"]["state"] != "ready":
                time.sleep(0.2)
                continue
            started = time.monotonic()
            observed_at = datetime.now(timezone.utc).isoformat()
            result = request("/recall", {"query": query, "limit": 20})
            elapsed = time.monotonic() - started
            attempts += 1
            raw = PRIVATE / f"{args.backend.lower()}-{label}-{attempts}.json"
            raw.write_text(json.dumps(result, indent=2) + "\n")
            raw.chmod(0o600)
            if result.get("backend") != args.backend and args.backend == "HippoRAG":
                print(json.dumps({"case": label, "backend": result.get("backend"), "retry": attempts}), flush=True)
                time.sleep(2)
                continue
            matches = provenance(result.get("results", []), source, author)
            row = {"case": label, "observed_at": observed_at, "elapsed_seconds": elapsed,
                   "backend": result.get("backend"), "results_count": len(result.get("results", [])),
                   "graph_seed_count": len(result.get("graph_seeds", [])),
                   "expected_source": source, "expected_historical_author": author,
                   "matching_results": matches, "raw_response_private": str(raw),
                   "passed": bool(matches) and all(r["canonical_metadata_match"] for r in matches)
                             and result.get("backend") == args.backend}
            summary["cases"].append(row)
            summary["all_passed"] = len(summary["cases"]) == len(cases) and all(r["passed"] for r in summary["cases"])
            output.write_text(json.dumps(summary, indent=2) + "\n")
            print(json.dumps({"case": label, "backend": row["backend"], "matching_sources": len(matches), "passed": row["passed"]}), flush=True)
            break


if __name__ == "__main__":
    main()
