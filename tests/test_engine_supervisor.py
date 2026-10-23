import json
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

    def tearDown(self):
        self.tmp.cleanup()

    def check(self, state="active", age=1000, marker=None):
        if marker is not None:
            (self.root / "running.json").write_text(json.dumps(marker))
        output = (f"ActiveState={state}\nMainPID=100\n"
                  f"ExecMainStartTimestampMonotonic={int((2000 - age) * 1_000_000)}\n")
        result = subprocess.CompletedProcess([], 0, stdout=output)
        with patch.object(supervisor.subprocess, "run", return_value=result) as run, \
             patch.object(supervisor.time, "monotonic", return_value=2000), \
             patch.object(supervisor, "crash_report") as report:
            status = supervisor.watchdog(self.root, 900)
            return status, run.call_args_list, report.call_args_list

    def test_stalled_startup_with_no_heartbeat_is_restarted(self):
        status, calls, reports = self.check()
        self.assertEqual(status, "restarted")
        self.assertIn("restart", calls[-1].args[0])
        self.assertEqual(reports[0].args[1], "watchdog-missing-heartbeat")

    def test_old_process_marker_does_not_restart_a_fresh_service(self):
        status, calls, reports = self.check(age=10, marker={"pid": 99})
        self.assertEqual(status, "healthy")
        self.assertEqual(len(calls), 1)
        self.assertEqual(reports, [])

    def test_fresh_heartbeat_keeps_long_running_process(self):
        status, _, reports = self.check(marker={"pid": 100})
        self.assertEqual(status, "healthy")
        self.assertEqual(reports, [])

    def test_inactive_service_is_started(self):
        status, calls, reports = self.check(state="failed")
        self.assertEqual(status, "started")
        self.assertIn("start", calls[-1].args[0])
        self.assertEqual(reports, [])

    def test_stuck_activation_is_also_restarted(self):
        status, _, reports = self.check(state="activating")
        self.assertEqual(status, "restarted")
        self.assertEqual(reports[0].args[1], "watchdog-missing-heartbeat")


if __name__ == "__main__":
    unittest.main()
