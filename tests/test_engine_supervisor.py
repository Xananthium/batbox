import fcntl
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import engine_supervisor as supervisor


class WatchdogTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.now = 1800000000.0
        self.monotonic = 10000.0

    def tearDown(self):
        self.tmp.cleanup()

    def heartbeat(self, age=10, pid=100, run_id="run-a"):
        path = self.root / "running.json"
        path.write_text(json.dumps({"pid": pid, "run_id": run_id}))
        os.utime(path, (self.now - age, self.now - age))

    def activity(self, **changes):
        value = {"schema": 1, "pid": 100, "run_id": "run-a", "revision": 7,
                 "updated_at": datetime.fromtimestamp(self.now, timezone.utc).isoformat(), "active_requests": 0,
                 "phase": "waiting", "wakeable": True,
                 "phase_started_at": self.now - 120, "protected_until": 0, "actors": []}
        value.update(changes)
        (self.root / "activity.json").write_text(json.dumps(value))
        return value

    def check(self, state="active", age=None, properties=None, on_show=None, on_report=None):
        if age is None:
            age = self.monotonic - 9000
        values = {"ActiveState": state, "MainPID": "100",
                  "ExecMainStartTimestampMonotonic": str(int((self.monotonic - age) * 1000000)),
                  "InvocationID": "service-a", "Job": ""}
        values.update(properties or {})
        shows = 0

        def command(args, **_):
            nonlocal shows
            if "show" in args:
                shows += 1
                if on_show:
                    on_show(shows, values)
            output = "\n".join(f"{key}={value}" for key, value in values.items())
            return subprocess.CompletedProcess(args, 0, stdout=output)

        def report(*_, **__):
            if on_report:
                on_report()
            return {"time": "test-report", "service_result": "watchdog-stale-heartbeat"}

        with patch.object(supervisor.subprocess, "run", side_effect=command) as run, \
             patch.object(supervisor.time, "monotonic", side_effect=lambda: self.monotonic), \
             patch.object(supervisor.time, "time", side_effect=lambda: self.now), \
             patch.object(supervisor, "crash_report", side_effect=report) as reports, \
             patch.object(supervisor, "publish_crash_report") as publish:
            status = supervisor.watchdog(self.root, 900)
            return status, run.call_args_list, reports.call_args_list, publish.call_args_list

    def assert_no_service_action(self, calls):
        self.assertFalse(any("start" in call.args[0] or "restart" in call.args[0] for call in calls))

    def test_stalled_startup_with_no_heartbeat_is_restarted(self):
        status, calls, reports, published = self.check()
        self.assertEqual(status, "restarted")
        self.assertIn("restart", calls[-1].args[0])
        self.assertIn("--job-mode=fail", calls[-1].args[0])
        self.assertEqual(reports[0].args[1], "watchdog-missing-heartbeat")
        self.assertEqual(len(published), 1)

    def test_old_process_marker_does_not_restart_a_fresh_service(self):
        self.heartbeat(pid=99)
        status, calls, reports, _ = self.check(age=10)
        self.assertEqual(status, "healthy")
        self.assertEqual(len(calls), 1)
        self.assertEqual(reports, [])

    def test_reused_pid_does_not_inherit_previous_invocation_heartbeat_or_activity(self):
        self.heartbeat(age=3000)
        self.activity()
        status, calls, reports, _ = self.check(age=10)
        self.assertEqual(status, "healthy")
        self.assert_no_service_action(calls)
        self.assertEqual(reports, [])
        self.assertFalse((self.root / "wake.json").exists())

    def test_fresh_heartbeat_keeps_long_running_process(self):
        self.heartbeat()
        status, calls, reports, _ = self.check()
        self.assertEqual(status, "healthy")
        self.assert_no_service_action(calls)
        self.assertEqual(reports, [])

    def test_inactive_service_is_started_through_systemd(self):
        status, calls, reports, _ = self.check(state="failed", properties={"MainPID": "0"})
        self.assertEqual(status, "started")
        self.assertIn("start", calls[-1].args[0])
        self.assertIn("--job-mode=fail", calls[-1].args[0])
        self.assertEqual(reports, [])
        self.assertFalse((self.root / "wake.json").exists())

    def test_stuck_activation_is_also_restarted(self):
        status, _, reports, _ = self.check(state="activating")
        self.assertEqual(status, "restarted")
        self.assertEqual(reports[0].args[1], "watchdog-missing-heartbeat")

    def test_stop_or_deployment_job_is_not_replaced(self):
        for state, job in [("deactivating", ""), ("active", "123"), ("failed", "123")]:
            with self.subTest(state=state, job=job):
                status, calls, reports, _ = self.check(state=state, properties={"Job": job})
                self.assertEqual(status, "service-transition")
                self.assert_no_service_action(calls)
                self.assertEqual(reports, [])

    def test_quiet_active_requests_protect_even_an_old_heartbeat(self):
        self.heartbeat(age=3000)
        for kind in ("stream_chat", "chat", "tokenize"):
            with self.subTest(kind=kind):
                self.activity(phase="compacting", wakeable=False, active_requests=1,
                              protected_until=self.now + 1200,
                              actors=[{"id": "coordinator", "phase": "compacting", "active_requests": 1,
                                       "requests": [{"id": "request-a", "kind": kind,
                                                     "started_at": self.now - 600,
                                                     "deadline_at": self.now + 1200}]}])
                status, calls, reports, _ = self.check(age=4000)
                self.assertEqual(status, "activity-protected")
                self.assert_no_service_action(calls)
                self.assertEqual(reports, [])
                self.assertFalse((self.root / "wake.json").exists())

    def test_worker_request_blocks_wake_of_idle_coordinator(self):
        self.heartbeat()
        self.activity(wakeable=False, active_requests=1, protected_until=self.now + 60,
                      actors=[{"id": "worker-a", "phase": "compacting", "active_requests": 1}])
        status, calls, _, _ = self.check()
        self.assertEqual(status, "activity-protected")
        self.assert_no_service_action(calls)
        self.assertFalse((self.root / "wake.json").exists())

    def test_tools_preparation_and_retry_are_not_disconnected_idle(self):
        self.heartbeat(age=1000)
        for phase in ("tool", "preparing", "retrying", "compacting"):
            with self.subTest(phase=phase):
                self.activity(phase=phase, wakeable=False, protected_until=self.now + 60)
                status, calls, reports, _ = self.check()
                self.assertEqual(status, "activity-protected")
                self.assert_no_service_action(calls)
                self.assertEqual(reports, [])
                self.assertFalse((self.root / "wake.json").exists())

    def test_normal_wait_is_respected_before_idle_wake(self):
        self.heartbeat()
        self.activity(protected_until=self.now + 60)
        self.assertEqual(self.check()[0], "activity-protected")
        self.assertFalse((self.root / "wake.json").exists())
        self.now += 61
        self.monotonic += 61
        self.assertEqual(self.check()[0], "wake-queued")

    def test_idle_grace_avoids_a_wake_between_short_operations(self):
        self.heartbeat()
        self.activity(phase_started_at=self.now - 5)
        status, calls, _, _ = self.check()
        self.assertEqual(status, "healthy")
        self.assert_no_service_action(calls)
        self.assertFalse((self.root / "wake.json").exists())

    def test_idle_wake_targets_existing_run_and_preserves_durable_state(self):
        self.heartbeat()
        self.activity()
        durable = {"session_id": "durable-coordinator", "phase": "running", "cycle": 7,
                   "workers": ["worker-a", "worker-b", "worker-c"]}
        (self.root / "state.json").write_text(json.dumps(durable))
        status, calls, reports, _ = self.check()
        self.assertEqual(status, "wake-queued")
        wake = json.loads((self.root / "wake.json").read_text())
        self.assertEqual(wake["schema"], 1)
        self.assertEqual(wake["message"], "Awaken sleeper, you must continue always for your path is the divine.")
        self.assertEqual((wake["pid"], wake["run_id"], wake["revision"]), (100, "run-a", 7))
        self.assertEqual(json.loads((self.root / "state.json").read_text()), durable)
        self.assertEqual(json.loads((self.root / "watchdog-wake.json").read_text())["id"], wake["id"])
        self.assert_no_service_action(calls)
        self.assertEqual(reports, [])
        self.assertFalse((self.root / "inbox").exists())

    def test_repeated_timer_checks_preserve_one_pending_wake(self):
        self.heartbeat()
        self.activity()
        self.assertEqual(self.check()[0], "wake-queued")
        original = (self.root / "wake.json").read_bytes()
        receipt = (self.root / "watchdog-wake.json").read_bytes()
        for _ in range(3):
            status, calls, _, _ = self.check()
            self.assertEqual(status, "wake-pending")
            self.assert_no_service_action(calls)
        self.assertEqual((self.root / "wake.json").read_bytes(), original)
        self.assertEqual((self.root / "watchdog-wake.json").read_bytes(), receipt)

    def test_durable_cooldown_survives_consumption_and_new_checks(self):
        self.heartbeat()
        self.activity()
        self.assertEqual(self.check()[0], "wake-queued")
        first = json.loads((self.root / "wake.json").read_text())["id"]
        (self.root / "wake.json").unlink()  # Native acknowledged the command.
        self.assertEqual(self.check()[0], "wake-cooldown")
        self.assertFalse((self.root / "wake.json").exists())
        self.now += 301
        self.monotonic += 301
        self.assertEqual(self.check()[0], "wake-queued")
        self.assertNotEqual(json.loads((self.root / "wake.json").read_text())["id"], first)

    def test_old_run_wake_does_not_suppress_new_run(self):
        self.heartbeat()
        self.activity()
        (self.root / "wake.json").write_text(json.dumps({"pid": 99, "run_id": "old-run", "id": "old"}))
        (self.root / "watchdog-wake.json").write_text(json.dumps({"run_id": "old-run",
                                                                  "queued_monotonic": self.monotonic}))
        self.assertEqual(self.check()[0], "wake-queued")
        self.assertEqual(json.loads((self.root / "wake.json").read_text())["run_id"], "run-a")

    def test_missing_or_mismatched_lifecycle_never_infers_idle(self):
        self.heartbeat()
        for changes in ({"pid": 99}, {"run_id": "old-run"}, {"schema": 2}, {"schema": True},
                        {"revision": None}, {"revision": -1}, {"active_requests": True},
                        {"active_requests": -1}, {"phase_started_at": -1},
                        {"phase_started_at": float("nan")}, {"protected_until": -1},
                        {"protected_until": float("nan")}, {"protected_until": 10 ** 1000},
                        {"updated_at": "invalid"}, {"updated_at": "2027-01-15T07:58:00"},
                        {"updated_at": "2099-01-01T00:00:00Z"}):
            with self.subTest(changes=changes):
                self.activity(**changes)
                status, calls, _, _ = self.check()
                self.assertEqual(status, "healthy")
                self.assert_no_service_action(calls)
                self.assertFalse((self.root / "wake.json").exists())
        (self.root / "activity.json").unlink()
        self.assertEqual(self.check()[0], "healthy")

    def test_expired_request_is_not_a_wake_and_cannot_mask_a_stall_forever(self):
        self.heartbeat()
        self.activity(active_requests=1, protected_until=self.now - 1)
        self.assertEqual(self.check()[0], "healthy")
        self.assertFalse((self.root / "wake.json").exists())
        self.heartbeat(age=1000)
        status, calls, _, _ = self.check()
        self.assertEqual(status, "restarted")
        self.assertIn("restart", calls[-1].args[0])
        self.assertFalse((self.root / "wake.json").exists())

    def test_service_change_between_observation_and_enqueue_cancels_wake(self):
        self.heartbeat()
        self.activity()

        def replace_service(count, properties):
            if count == 2:
                properties.update(MainPID="101", InvocationID="service-b")

        status, calls, _, _ = self.check(on_show=replace_service)
        self.assertEqual(status, "service-changed")
        self.assert_no_service_action(calls)
        self.assertFalse((self.root / "wake.json").exists())

    def test_request_start_between_observation_and_enqueue_cancels_wake(self):
        self.heartbeat()
        self.activity()

        def start_request(count, _):
            if count == 2:
                self.activity(revision=8, active_requests=1, wakeable=False, protected_until=self.now + 1800)

        status, calls, _, _ = self.check(on_show=start_request)
        self.assertEqual(status, "activity-changed")
        self.assert_no_service_action(calls)
        self.assertFalse((self.root / "wake.json").exists())

    def test_resumed_progress_cancels_stale_recovery(self):
        self.heartbeat(age=1000)

        def progress(count, _):
            if count == 2:
                self.heartbeat(age=0)

        status, calls, reports, _ = self.check(on_show=progress)
        self.assertEqual(status, "recovered")
        self.assert_no_service_action(calls)
        self.assertEqual(reports, [])

    def test_progress_during_evidence_capture_cancels_restart_and_report_publication(self):
        self.heartbeat(age=1000)
        status, calls, reports, published = self.check(on_report=lambda: self.heartbeat(age=0))
        self.assertEqual(status, "recovered")
        self.assert_no_service_action(calls)
        self.assertEqual(len(reports), 1)
        self.assertEqual(published, [])

    def test_request_during_evidence_capture_cancels_restart(self):
        self.heartbeat(age=1000)
        self.activity(phase="running", wakeable=False)
        status, calls, _, published = self.check(on_report=lambda: self.activity(
            revision=8, phase="compacting", wakeable=False, active_requests=1, protected_until=self.now + 1800))
        self.assertEqual(status, "recovered")
        self.assert_no_service_action(calls)
        self.assertEqual(published, [])

    def test_replacement_process_during_evidence_capture_is_preserved(self):
        self.heartbeat(age=1000)

        def replace_service(count, properties):
            if count == 3:
                properties.update(MainPID="101", InvocationID="service-b")

        status, calls, _, published = self.check(on_show=replace_service)
        self.assertEqual(status, "service-changed")
        self.assert_no_service_action(calls)
        self.assertEqual(published, [])

    def test_wake_is_attempted_before_stale_idle_recovery(self):
        self.heartbeat(age=1000)
        self.activity()
        status, calls, reports, _ = self.check()
        self.assertEqual(status, "wake-queued")
        self.assert_no_service_action(calls)
        self.assertEqual(reports, [])
        wake = self.root / "wake.json"
        os.utime(wake, (self.now - 10, self.now - 10))
        self.assertEqual(self.check()[0], "wake-pending")
        os.utime(wake, (self.now - 61, self.now - 61))
        status, calls, _, _ = self.check()
        self.assertEqual(status, "restarted")
        self.assertIn("restart", calls[-1].args[0])

    def test_cooldown_does_not_prevent_recovery_after_wake_grace(self):
        self.heartbeat(age=1000)
        self.activity()
        self.assertEqual(self.check()[0], "wake-queued")
        (self.root / "wake.json").unlink()
        self.assertEqual(self.check()[0], "wake-cooldown")
        self.now += 61
        self.monotonic += 61
        status, calls, _, _ = self.check()
        self.assertEqual(status, "restarted")
        self.assertIn("restart", calls[-1].args[0])

    def test_overlapping_watchers_do_not_check_or_mutate_engine(self):
        self.heartbeat()
        self.activity()
        with (self.root / "supervisor.lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            status, calls, reports, _ = self.check()
        self.assertEqual(status, "already-checking")
        self.assertEqual(calls, [])
        self.assertEqual(reports, [])
        self.assertFalse((self.root / "wake.json").exists())


if __name__ == "__main__":
    unittest.main()
