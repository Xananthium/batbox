#!/usr/bin/env python3
"""Crash evidence and heartbeat watchdog, kept outside the engine process."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import subprocess
import time

from engine_api import atomic_json


def read_json(path, default=None):
    try:
        return json.loads(path.read_text())
    except (OSError, ValueError):
        return {} if default is None else default


def crash_report(root, result, exit_code, exit_status):
    if result == "success" and exit_status in {"", "0", "15", "TERM"}:
        return None
    stamp = datetime.now(timezone.utc).isoformat()
    report = {"time": stamp, "service_result": result, "exit_code": exit_code,
              "exit_status": exit_status, "running": read_json(root / "running.json"),
              "engine_state": read_json(root / "status.json"),
              "source_root": str(Path(__file__).resolve().parent.parent)}
    try:
        logs = subprocess.run(["journalctl", "--user", "-u", "batbox-engine.service", "-n", "80", "--no-pager", "-o", "short-iso"],
                              capture_output=True, text=True, timeout=10)
        report["journal_tail"] = logs.stdout[-24000:]
    except (OSError, subprocess.TimeoutExpired) as exc:
        report["journal_error"] = type(exc).__name__
    report["recovery_instruction"] = (
        "The previous engine process crashed. Inspect the reported error and the source repository. "
        "Reproduce the failure, repair it, run relevant tests, and use tools/engine_deploy.py to "
        "install the tested binary atomically. Preserve pending work and durable memory.")
    atomic_json(root / "crash-report.json", report)
    atomic_json(root / "crashes" / (str(time.time_ns()) + ".json"), report)
    return report


def watchdog(root, stale_seconds):
    # A missing marker can mean a process stalled during startup. Inspect the
    # actual service age as well, rather than waiting forever for its first
    # heartbeat. An old marker from a previous PID must not kill a new process.
    shown = subprocess.run([
        "systemctl", "--user", "show", "batbox-engine.service",
        "--property=ActiveState,MainPID,ExecMainStartTimestampMonotonic"],
        check=True, capture_output=True, text=True, timeout=10)
    properties = dict(line.split("=", 1) for line in shown.stdout.splitlines() if "=" in line)
    if properties.get("ActiveState") not in {"active", "activating", "reloading"}:
        subprocess.run(["systemctl", "--user", "start", "batbox-engine.service"], check=True, timeout=30)
        return "started"

    marker = root / "running.json"
    heartbeat = read_json(marker)
    pid = int(properties.get("MainPID") or 0)
    started = int(properties.get("ExecMainStartTimestampMonotonic") or 0) / 1_000_000
    age = max(0, time.monotonic() - started) if started else 0
    reason = "watchdog-missing-heartbeat"
    if isinstance(heartbeat, dict) and pid and heartbeat.get("pid") == pid:
        try:
            age = max(0, time.time() - marker.stat().st_mtime)
            reason = "watchdog-stale-heartbeat"
        except OSError:
            pass  # The engine may remove it during an orderly shutdown.
    if age <= stale_seconds:
        return "healthy"
    crash_report(root, reason, "watchdog", str(int(age)))
    subprocess.run(["systemctl", "--user", "restart", "batbox-engine.service"], check=True, timeout=45)
    return "restarted"


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("mode", choices=["record", "watchdog"])
    p.add_argument("--state-dir", default=str(Path.home() / ".batbox/autonomous"))
    p.add_argument("--stale-seconds", type=int, default=900)
    args = p.parse_args()
    if args.stale_seconds < 1:
        p.error("--stale-seconds must be positive")
    root = Path(args.state_dir).expanduser()
    root.mkdir(parents=True, exist_ok=True)
    if args.mode == "record":
        crash_report(root, os.environ.get("SERVICE_RESULT", "unknown"),
                     os.environ.get("EXIT_CODE", "unknown"), os.environ.get("EXIT_STATUS", "unknown"))
        return
    watchdog(root, args.stale_seconds)


if __name__ == "__main__":
    main()
