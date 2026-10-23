"""Observe memory restoration without generating any model requests."""
import json
from pathlib import Path
import time
import urllib.request

from memory_restore_checks import state

HERE = Path(__file__).resolve().parent


def main():
    previous = None
    while True:
        progress = state()
        try:
            with urllib.request.urlopen("http://127.0.0.1:8771/health", timeout=5) as response:
                health = json.load(response)
            graph = health["hipporag"]
        except (OSError, ValueError) as exc:
            health = None
            graph = {"state": "unavailable", "last_error": str(exc)}
        event = {"historical": [[r["author"], r["graph_ready"], r["records"]] for r in progress["historical"]],
                 "current_snapshots": [r["current_file_graphed"] for r in progress["current_snapshots"]],
                 "mission": progress["mission"], "graph_index_rows": progress["graph_index_rows"],
                 "pending_graph_sources": progress["other_graph_work_pending"],
                 "graph_state": graph["state"], "current_source": graph.get("current_source"),
                 "graph_error": graph.get("last_error"), "complete": progress["requested_targets_complete"]}
        if event != previous:
            event_with_time = {"at": progress["observed_at"], **event}
            with (HERE / "restoration-progress.jsonl").open("a") as output:
                output.write(json.dumps(event_with_time) + "\n")
            (HERE / "restoration-current.json").write_text(json.dumps(progress, indent=2) + "\n")
            if progress["requested_targets_complete"]:
                (HERE / "restoration-targets-complete.json").write_text(json.dumps(progress | {"health": health}, indent=2) + "\n")
            print(json.dumps(event_with_time), flush=True)
            previous = event
        time.sleep(5)


if __name__ == "__main__":
    main()
