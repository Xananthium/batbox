import contextlib
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import engine_deploy as deploy


class DeploymentTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name) / "releases-root"
        self.binary = Path(self.tmp.name) / "build-output"

    def tearDown(self):
        self.tmp.cleanup()

    def run_deploy(self, *args):
        output = io.StringIO()
        with patch.object(sys, "argv", ["engine_deploy", "--root", str(self.root), *args]), \
             contextlib.redirect_stdout(output):
            deploy.main()
        return json.loads(output.getvalue())

    def candidate(self, version, success=True):
        self.binary.write_text(f"#!/bin/sh\necho '{version}'\nexit {0 if success else 1}\n")
        self.binary.chmod(0o755)

    def test_install_rollback_and_validation_failure_preserve_releases(self):
        self.candidate("first")
        first = self.run_deploy("--binary", str(self.binary))
        self.candidate("second")
        second = self.run_deploy("--binary", str(self.binary))
        self.assertNotEqual(first["current"], second["current"])
        self.assertEqual(Path(second["previous"]), Path(first["current"]).parent)
        self.candidate("broken", success=False)
        with self.assertRaises(subprocess.CalledProcessError):
            self.run_deploy("--binary", str(self.binary))
        self.assertEqual(self.root.joinpath("current").resolve(), Path(second["current"]).parent)
        rolled_back = self.run_deploy("--rollback")
        self.assertEqual(rolled_back["current"], first["current"])

    def test_source_rebuild_during_validation_does_not_change_staged_release(self):
        self.candidate("tested bytes")
        expected = self.binary.read_bytes()
        original_run = deploy.subprocess.run

        def rebuild_after_staging(*args, **kwargs):
            self.candidate("later rebuild")
            return original_run(*args, **kwargs)

        with patch.object(deploy.subprocess, "run", side_effect=rebuild_after_staging):
            installed = self.run_deploy("--binary", str(self.binary))
        path = Path(installed["current"])
        self.assertEqual(path.read_bytes(), expected)
        self.assertEqual(path.parent.name, hashlib.sha256(expected).hexdigest()[:20])


if __name__ == "__main__":
    unittest.main()
