#!/usr/bin/env python3
"""Install a verified BatBox binary atomically, retaining a rollback release."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def sync_directory(path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def replace_link(path, target):
    tmp = path.with_name(path.name + ".next")
    tmp.unlink(missing_ok=True)
    tmp.symlink_to(target)
    os.replace(tmp, path)
    sync_directory(path.parent)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--binary", type=Path)
    p.add_argument("--root", type=Path, default=Path.home() / ".local/lib/batbox-engine")
    p.add_argument("--rollback", action="store_true")
    p.add_argument("--restart", action="store_true")
    args = p.parse_args()
    root = args.root.expanduser().resolve()
    root.mkdir(parents=True, exist_ok=True)
    with (root / "deploy.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        current, previous = root / "current", root / "previous"
        if args.rollback:
            if not previous.exists():
                raise SystemExit("No previous release exists")
            target = previous.resolve()
            if not (target / "batbox").is_file():
                raise SystemExit("Previous release has no executable; current release was preserved")
            subprocess.run([str(target / "batbox"), "--version"], check=True, timeout=15, capture_output=True)
        else:
            if not args.binary or not args.binary.is_file():
                raise SystemExit("--binary must point to a tested executable")
            # Validate and hash the exact staged bytes. The build output can be
            # replaced while deployment runs, so validating the source then
            # copying it would not validate the executable being installed.
            with tempfile.TemporaryDirectory(prefix=".staging-", dir=root) as staging:
                staged = Path(staging) / "batbox"
                shutil.copy2(args.binary, staged)
                staged.chmod(0o755)
                with staged.open("rb") as f:
                    os.fsync(f.fileno())
                    digest = hashlib.file_digest(f, "sha256").hexdigest()[:20]
                subprocess.run([str(staged), "--version"], check=True, timeout=15, capture_output=True)
                target = root / "releases" / digest
                target.mkdir(parents=True, exist_ok=True)
                os.replace(staged, target / "batbox")
                sync_directory(target)
                sync_directory(target.parent)
                sync_directory(root)
        old = current.resolve() if current.exists() else None
        if old != target:
            if old:
                replace_link(previous, old)
            replace_link(current, target)
        print(json.dumps({"current": str(target / "batbox"), "previous": str(previous.resolve()) if previous.exists() else None}))
    if args.restart:
        # --no-block avoids killing the caller before systemd queues the restart.
        subprocess.run(["systemctl", "--user", "--no-block", "restart", "batbox-engine.service"], check=True)


if __name__ == "__main__":
    main()
