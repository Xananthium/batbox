#!/usr/bin/env python3
"""Wake idle BatBox work and recover stalled processes without duplicating them."""
import argparse
from datetime import datetime, timezone
import fcntl
import json
import math
import os
from pathlib import Path
import secrets
import subprocess
import time

from engine_api import atomic_json


WAKE_MESSAGE = "Awaken sleeper, you must continue always for your path is the divine."
ENGINE_SERVICE = "batbox-engine.service"


def read_json(path, default=None):
    try:
        return json.loads(path.read_text())
    except (OSError, ValueError):
        return {} if default is None else default


def publish_crash_report(root, report):
    atomic_json(root / "crash-report.json", report)
    atomic_json(root / "crashes" / (str(time.time_ns()) + ".json"), report)


def crash_report(root, result, exit_code, exit_status, publish=True):
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
    if publish:
        publish_crash_report(root, report)
    return report


def service_properties():
    shown = subprocess.run([
        "systemctl", "--user", "show", ENGINE_SERVICE,
        "--property=ActiveState,MainPID,ExecMainStartTimestampMonotonic,InvocationID,Job"],
        check=True, capture_output=True, text=True, timeout=10)
    return dict(line.split("=", 1) for line in shown.stdout.splitlines() if "=" in line)


def service_identity(properties):
    return tuple(properties.get(key, "") for key in
                 ("MainPID", "ExecMainStartTimestampMonotonic", "InvocationID"))


def service_transition(properties):
    return (properties.get("ActiveState") in {"deactivating", "maintenance"}
            or properties.get("Job", "") not in {"", "0", "0 /"})


def heartbeat_status(root, properties):
    # An old marker from a previous PID must not kill a fresh service. A missing
    # marker still permits recovery after the actual service startup ages out.
    marker = root / "running.json"
    heartbeat = read_json(marker)
    pid = int(properties.get("MainPID") or 0)
    started = int(properties.get("ExecMainStartTimestampMonotonic") or 0) / 1_000_000
    age = max(0, time.monotonic() - started) if started else 0
    reason = "watchdog-missing-heartbeat"
    if isinstance(heartbeat, dict) and pid and heartbeat.get("pid") == pid:
        try:
            marker_age = time.time() - marker.stat().st_mtime
            # Linux can reuse a PID. A marker predating this service invocation
            # is not its heartbeat even when the numeric PID matches.
            if marker_age < -1 or (started and marker_age > age + 1):
                return {}, age, reason
            age = max(0, marker_age)
            reason = "watchdog-stale-heartbeat"
        except OSError:
            pass  # The engine may remove it during an orderly shutdown.
    return heartbeat, age, reason


def finite_number(value):
    try:
        return type(value) in {int, float} and math.isfinite(value)
    except OverflowError:
        return False


def timestamp(value):
    if not isinstance(value, str):
        return None
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
        return parsed.timestamp() if parsed.tzinfo is not None else None
    except (ValueError, OverflowError, OSError):
        return None


def activity_snapshot(root, properties, heartbeat):
    """Only native lifecycle data for this exact running process is evidence.

    TCP keepalive sockets, an old coordinator status, and a watchdog's own
    polling are deliberately not inference activity or progress.
    """
    activity = read_json(root / "activity.json")
    if not isinstance(activity, dict) or not isinstance(heartbeat, dict):
        return None
    pid = int(properties.get("MainPID") or 0)
    observed = timestamp(activity.get("updated_at"))
    if (type(activity.get("schema")) is not int or activity["schema"] != 1
            or not pid or type(activity.get("pid")) is not int or activity["pid"] != pid
            or heartbeat.get("pid") != pid
            or not isinstance(activity.get("run_id"), str) or not activity["run_id"]
            or heartbeat.get("run_id") != activity["run_id"]
            or type(activity.get("revision")) is not int or activity["revision"] < 0
            or type(activity.get("active_requests")) is not int or activity["active_requests"] < 0
            or type(activity.get("wakeable")) is not bool
            or not isinstance(activity.get("phase"), str)
            or not finite_number(activity.get("phase_started_at"))
            or activity["phase_started_at"] < 0
            or not finite_number(activity.get("protected_until")) or activity["protected_until"] < 0
            or observed is None or observed < activity["phase_started_at"] - 1
            or observed > time.time() + 5):
        return None
    return activity


def activity_protected(activity, now):
    # Native request scopes include quiet compaction/tokenization and retries.
    # Tool execution and deliberate waits also have finite protection. A stale
    # active flag cannot exempt a process forever after its deadline has passed.
    return bool(activity and activity["protected_until"] > now)


def wake_eligible(activity, now, wake_after_seconds):
    return bool(activity and activity["wakeable"]
                and activity["phase"] in {"waiting", "idle"}
                and activity["active_requests"] == 0
                and not activity_protected(activity, now)
                and now - activity["phase_started_at"] >= wake_after_seconds)


def same_activity(left, right):
    return bool(left and right and all(left[key] == right[key]
                for key in ("pid", "run_id", "revision")))


def wake_idle(root, properties, activity, wake_after_seconds, cooldown_seconds):
    """Publish at most one pending wake; native consumption rechecks revision.

    The receipt precedes publication, so a crash between these writes can only
    delay a retry. The pending command and receipt survive supervisor restarts.
    """
    pending = read_json(root / "wake.json")
    receipt = read_json(root / "watchdog-wake.json")
    now = time.time()
    if isinstance(pending, dict) and pending:
        if pending.get("pid") == activity["pid"] and pending.get("run_id") == activity["run_id"]:
            # Even an obsolete revision belongs to native consumption. Do not
            # replace a command while the coordinator may be acknowledging it.
            try:
                queued_at = (root / "wake.json").stat().st_mtime
                if queued_at > now + 1:
                    queued_at = None
            except FileNotFoundError:
                queued_at = now
            return "wake-pending", queued_at
    if (isinstance(receipt, dict) and receipt.get("run_id") == activity["run_id"]
            and finite_number(receipt.get("queued_monotonic"))
            and 0 <= time.monotonic() - receipt["queued_monotonic"] < cooldown_seconds):
        return "wake-cooldown", now - (time.monotonic() - receipt["queued_monotonic"])

    # The service or engine may have advanced after the first observation. The
    # command also carries this revision so native code closes the remaining
    # check-to-publish race without interrupting active work.
    current = service_properties()
    if (service_identity(current) != service_identity(properties)
            or current.get("ActiveState") != "active" or service_transition(current)):
        return "service-changed", None
    heartbeat, _, _ = heartbeat_status(root, current)
    latest = activity_snapshot(root, current, heartbeat)
    now = time.time()
    if not same_activity(activity, latest) or not wake_eligible(latest, now, wake_after_seconds):
        return "activity-changed", None

    wake_id = "wake_" + secrets.token_hex(16)
    command = {"schema": 1, "id": wake_id, "pid": activity["pid"],
               "run_id": activity["run_id"], "revision": activity["revision"],
               "message": WAKE_MESSAGE, "created_at": datetime.now(timezone.utc).isoformat()}
    atomic_json(root / "watchdog-wake.json", {
        "schema": 1, "id": wake_id, "pid": activity["pid"], "run_id": activity["run_id"],
        "revision": activity["revision"], "queued_at": now, "queued_monotonic": time.monotonic()})
    atomic_json(root / "wake.json", command)
    return "wake-queued", now


def recover_stalled(root, properties, stale_seconds):
    # Never replace a deployment/stop job or restart a new process that appeared
    # while checking the old one. Progress and newly started requests win too.
    current = service_properties()
    if (service_identity(current) != service_identity(properties)
            or current.get("ActiveState") not in {"active", "activating", "reloading"}
            or service_transition(current)):
        return "service-changed"
    heartbeat, age, reason = heartbeat_status(root, current)
    activity = activity_snapshot(root, current, heartbeat)
    if age <= stale_seconds or activity_protected(activity, time.time()):
        return "recovered"
    report = crash_report(root, reason, "watchdog", str(int(age)), publish=False)
    # Evidence capture can take seconds. Recheck once more before the only
    # destructive action; --job-mode=fail also preserves queued systemd jobs.
    latest = service_properties()
    if (service_identity(latest) != service_identity(current)
            or latest.get("ActiveState") not in {"active", "activating", "reloading"}
            or service_transition(latest)):
        return "service-changed"
    heartbeat, age, _ = heartbeat_status(root, latest)
    activity = activity_snapshot(root, latest, heartbeat)
    if age <= stale_seconds or activity_protected(activity, time.time()):
        return "recovered"
    publish_crash_report(root, report)
    subprocess.run(["systemctl", "--user", "--job-mode=fail", "restart", ENGINE_SERVICE],
                   check=True, timeout=45)
    return "restarted"


def check_engine(root, stale_seconds, wake_after_seconds, wake_cooldown_seconds, wake_grace_seconds):
    properties = service_properties()
    if service_transition(properties):
        return "service-transition"
    if properties.get("ActiveState") in {"inactive", "failed"}:
        # systemd owns process creation and the engine has its own instance
        # lock. The watcher never launches a second coordinator or any workers.
        subprocess.run(["systemctl", "--user", "--job-mode=fail", "start", ENGINE_SERVICE],
                       check=True, timeout=30)
        return "started"
    if properties.get("ActiveState") not in {"active", "activating", "reloading"}:
        return "service-unavailable"
    heartbeat, age, _ = heartbeat_status(root, properties)
    activity = activity_snapshot(root, properties, heartbeat)
    now = time.time()
    if activity_protected(activity, now):
        return "activity-protected"
    wake_status = "healthy"
    if properties.get("ActiveState") == "active" and wake_eligible(activity, now, wake_after_seconds):
        wake_status, queued_at = wake_idle(root, properties, activity, wake_after_seconds, wake_cooldown_seconds)
        if wake_status in {"wake-queued", "service-changed", "activity-changed"}:
            return wake_status
        if finite_number(queued_at) and now - queued_at < wake_grace_seconds:
            return wake_status
    if age <= stale_seconds:
        return wake_status
    return recover_stalled(root, properties, stale_seconds)


def watchdog(root, stale_seconds, wake_after_seconds=30, wake_cooldown_seconds=300, wake_grace_seconds=60):
    root = Path(root)
    root.mkdir(parents=True, exist_ok=True, mode=0o700)
    # A timer and a manual diagnostic can overlap. Only the holder may enqueue
    # a wake or ask systemd to recover the service.
    fd = os.open(root / "supervisor.lock", os.O_CREAT | os.O_RDWR, 0o600)
    with os.fdopen(fd, "a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return "already-checking"
        return check_engine(root, stale_seconds, wake_after_seconds, wake_cooldown_seconds, wake_grace_seconds)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("mode", choices=["record", "watchdog"])
    p.add_argument("--state-dir", default=str(Path.home() / ".batbox/autonomous"))
    p.add_argument("--stale-seconds", type=int, default=2100)
    p.add_argument("--wake-after-seconds", type=int, default=30)
    p.add_argument("--wake-cooldown-seconds", type=int, default=300)
    p.add_argument("--wake-grace-seconds", type=int, default=60)
    args = p.parse_args()
    if min(args.stale_seconds, args.wake_after_seconds, args.wake_cooldown_seconds, args.wake_grace_seconds) < 1:
        p.error("watchdog time intervals must be positive")
    root = Path(args.state_dir).expanduser()
    root.mkdir(parents=True, exist_ok=True)
    if args.mode == "record":
        crash_report(root, os.environ.get("SERVICE_RESULT", "unknown"),
                     os.environ.get("EXIT_CODE", "unknown"), os.environ.get("EXIT_STATUS", "unknown"))
        return
    result = watchdog(root, args.stale_seconds, args.wake_after_seconds,
                      args.wake_cooldown_seconds, args.wake_grace_seconds)
    print(json.dumps({"watchdog": result}), flush=True)


if __name__ == "__main__":
    main()
