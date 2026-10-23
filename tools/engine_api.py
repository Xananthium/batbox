#!/usr/bin/env python3
"""Local, durable issue API for the autonomous BatBox engine (stdlib only)."""
from __future__ import annotations

import argparse
import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import secrets
import signal
import sqlite3
import threading
import time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

STATUSES = {"backlog", "todo", "in_progress", "in_review", "done", "blocked", "cancelled"}
MAX_BODY = 131072
WORKER_STATUS_FIELDS = {
    "status", "phase", "updated_at", "last_progress_at", "last_operation",
    "progress_sequence", "failed_phase", "last_error_at", "retry_delay_seconds",
    "retry_not_before_epoch_ms", "recovery_failures", "recovery_attempts",
    "completed_turns", "prompt_tokens", "compactions", "exact_compaction_tokens",
    "tool_count", "recoverable", "continuous", "managed_slot",
}


def now():
    return datetime.now(timezone.utc).isoformat()


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    temp = path.with_name(path.name + "." + secrets.token_hex(8) + ".tmp")
    fd = os.open(temp, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(fd, "w") as out:
            json.dump(value, out, ensure_ascii=False)
            out.flush()
            os.fsync(out.fileno())
        os.replace(temp, path)
        parent = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(parent)
        finally:
            os.close(parent)
    finally:
        temp.unlink(missing_ok=True)


class IssueStore:
    def __init__(self, state_dir):
        self.root = Path(state_dir).expanduser().resolve()
        self.root.mkdir(parents=True, exist_ok=True, mode=0o700)
        for name in ("inbox", "outbox", "processed"):
            (self.root / name).mkdir(exist_ok=True, mode=0o700)
        self.path = self.root / "issues.sqlite3"
        with self.connect() as db:
            db.executescript('''
                PRAGMA journal_mode=WAL;
                CREATE TABLE IF NOT EXISTS issues (
                    number INTEGER PRIMARY KEY AUTOINCREMENT,
                    title TEXT NOT NULL, description TEXT NOT NULL,
                    status TEXT NOT NULL, created_at TEXT NOT NULL, updated_at TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS messages (
                    seq INTEGER PRIMARY KEY AUTOINCREMENT,
                    id TEXT UNIQUE NOT NULL, issue_number INTEGER NOT NULL REFERENCES issues(number),
                    role TEXT NOT NULL, content TEXT NOT NULL, created_at TEXT NOT NULL,
                    reply_to TEXT, delivery TEXT NOT NULL DEFAULT 'pending', source TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS events (
                    seq INTEGER PRIMARY KEY AUTOINCREMENT,
                    issue_number INTEGER NOT NULL REFERENCES issues(number),
                    kind TEXT NOT NULL, body TEXT NOT NULL, created_at TEXT NOT NULL);
                CREATE INDEX IF NOT EXISTS messages_delivery ON messages(role,delivery);
            ''')
        os.chmod(self.path, 0o600)
        key_path = self.root / "api.key"
        try:
            fd = os.open(key_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        except FileExistsError:
            pass
        else:
            with os.fdopen(fd, "w") as f:
                f.write(secrets.token_urlsafe(48) + "\n")
                f.flush()
                os.fsync(f.fileno())
        os.chmod(key_path, 0o600)
        self.token = key_path.read_text().strip()
        if len(self.token) < 32:
            raise ValueError("api.key must contain at least 32 characters")
        if not (self.root / "communication.json").exists():
            atomic_json(self.root / "communication.json", {
                "enabled": False, "api_url": "http://127.0.0.1:8772", "updated_at": now()})

    def connect(self):
        db = sqlite3.connect(self.path, timeout=20)
        db.row_factory = sqlite3.Row
        db.execute("PRAGMA foreign_keys=ON")
        db.execute("PRAGMA synchronous=FULL")
        return db

    def communication(self):
        try:
            data = json.loads((self.root / "communication.json").read_text())
            if not isinstance(data.get("enabled"), bool):
                raise ValueError("invalid enabled flag")
            return data
        except (OSError, ValueError):
            return {"enabled": False, "error": "communication state unavailable"}

    def engine_status(self):
        """Overlay current worker receipts without refreshing coordinator time.

        Worker journals are individually atomic, not a transaction spanning all
        workers. Reading them is observation, never evidence of new progress.
        """
        path = self.root / "status.json"
        engine = json.loads(path.read_text()) if path.exists() else {"state": "not_started"}
        snapshot_at = engine.get("updated_at")
        observed_at = now()
        workers = {}
        for item in engine.get("workers", []):
            if not isinstance(item, dict) or not isinstance(item.get("id"), str):
                continue
            # Do not accidentally publish future snapshot fields containing
            # prompts, inboxes, session content or model output.
            workers[item["id"]] = {k: v for k, v in item.items()
                if k in WORKER_STATUS_FIELDS | {"id", "name", "tokens"}
                and isinstance(v, (str, int, bool))}
        directory = self.root / "workers"
        # A worker may be absent from the coordinator snapshot while backing
        # off. Include its durable handle even before the next coordinator turn.
        ids = set(workers)
        ids.update(p.stem for p in directory.glob("*.json"))
        for agent_id in sorted(ids):
            worker = workers.get(agent_id, {"id": agent_id})
            try:
                if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,127}", agent_id):
                    raise ValueError("invalid worker id")
                journal = json.loads((directory / (agent_id + ".json")).read_text())
                if (not isinstance(journal, dict) or journal.get("agent_id") != agent_id
                        or not isinstance(journal.get("status"), str)):
                    raise ValueError("invalid worker journal")
                if agent_id not in workers and not journal.get("recoverable", False):
                    continue
                # The coordinator's token counter belongs to its old snapshot;
                # do not present it alongside current journal fields as live.
                worker = {k: v for k, v in worker.items() if k in {"id", "name"}}
                worker.update({k: v for k, v in journal.items() if k in WORKER_STATUS_FIELDS
                               and isinstance(v, (str, int, bool))})
                spec = journal.get("spec")
                if isinstance(spec, dict) and isinstance(spec.get("name"), str):
                    worker["name"] = spec["name"]
                worker["status_source"] = "worker_journal"
                worker["journal_read_at"] = observed_at
            except (OSError, ValueError) as exc:
                if agent_id not in workers:
                    continue
                worker["status_source"] = "coordinator_snapshot"
                worker["journal_error"] = "missing" if isinstance(exc, FileNotFoundError) else "unavailable"
            worker["coordinator_snapshot_at"] = snapshot_at
            workers[agent_id] = worker
        engine["coordinator_snapshot_at"] = snapshot_at
        engine["worker_journals_read_at"] = observed_at
        engine["workers"] = list(workers.values())
        return engine

    @staticmethod
    def number(key):
        if not re.fullmatch(r"BB-[1-9][0-9]*", key):
            raise KeyError("unknown issue")
        return int(key[3:])

    @staticmethod
    def message_id():
        return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%f") + "_" + secrets.token_hex(8)

    def _message(self, db, number, text):
        mid = self.message_id()
        db.execute("INSERT INTO messages(id,issue_number,role,content,created_at,source) VALUES(?,?,?,?,?,?)",
                   (mid, number, "user", text, now(), f"batbox://issues/BB-{number}/messages/{mid}"))
        return mid

    def create(self, body):
        title = str(body.get("title", "")).strip()
        description = str(body.get("description", "")).strip()
        status = body.get("status", "todo")
        if not title or len(title) > 300 or status not in STATUSES:
            raise ValueError("title (1..300 characters) and a valid status are required")
        if len(description.encode()) > 100000:
            raise ValueError("description too large")
        stamp = now()
        with self.connect() as db:
            number = db.execute("INSERT INTO issues(title,description,status,created_at,updated_at) VALUES(?,?,?,?,?)",
                                (title, description, status, stamp, stamp)).lastrowid
            db.execute("INSERT INTO events(issue_number,kind,body,created_at) VALUES(?,?,?,?)",
                       (number, "created", json.dumps({"status": status}), stamp))
            if status not in {"backlog", "done", "cancelled"}:
                self._message(db, number, title + "\n\n" + description)
        self.reconcile()
        return self.get(f"BB-{number}")

    def get(self, key):
        number = self.number(key)
        with self.connect() as db:
            row = db.execute("SELECT * FROM issues WHERE number=?", (number,)).fetchone()
            if not row:
                raise KeyError("unknown issue")
            item = dict(row)
            item["id"] = key
            item["messages"] = [dict(r) for r in db.execute("SELECT * FROM messages WHERE issue_number=? ORDER BY seq", (number,))]
            item["events"] = [dict(r) for r in db.execute("SELECT * FROM events WHERE issue_number=? ORDER BY seq", (number,))]
            return item

    def list(self):
        with self.connect() as db:
            return [{**dict(r), "id": f"BB-{r['number']}"} for r in db.execute("SELECT * FROM issues ORDER BY number DESC LIMIT 200")]

    def comment(self, key, body):
        text = str(body.get("content", "")).strip()
        if not text or len(text.encode()) > 100000:
            raise ValueError("content must contain 1..100000 bytes")
        number = self.number(key)
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            if not db.execute("SELECT 1 FROM issues WHERE number=?", (number,)).fetchone():
                raise KeyError("unknown issue")
            mid = self._message(db, number, text)
            db.execute("UPDATE issues SET updated_at=? WHERE number=?", (now(), number))
        self.reconcile()
        return {"id": mid, "issue_id": key, "status": "queued"}

    def update(self, key, body):
        number = self.number(key)
        status = body.get("status")
        if status not in STATUSES:
            raise ValueError("invalid issue status")
        reason = str(body.get("reason", ""))[:20000]
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            old = db.execute("SELECT * FROM issues WHERE number=?", (number,)).fetchone()
            if not old:
                raise KeyError("unknown issue")
            db.execute("UPDATE issues SET status=?,updated_at=? WHERE number=?", (status, now(), number))
            db.execute("INSERT INTO events(issue_number,kind,body,created_at) VALUES(?,?,?,?)",
                       (number, "status", json.dumps({"from": old["status"], "to": status, "reason": reason}), now()))
            if status == "todo" and old["status"] in {"backlog", "done", "cancelled", "blocked"}:
                self._message(db, number, "Issue activated: " + old["title"] + "\n" + old["description"] + "\n" + reason)
        self.reconcile()
        return self.get(key)

    def reconcile(self):
        """Replay the transactional outbox; importing the same reply is idempotent."""
        with self.connect() as db:
            # Acquire the writer slot before reading: HTTP handlers and the
            # background reconciler otherwise race upgrading read transactions.
            db.execute("BEGIN IMMEDIATE")
            for row in db.execute("SELECT * FROM messages WHERE role='user' AND delivery!='replied'").fetchall():
                mid = row["id"]
                output = self.root / "outbox" / (mid + ".json")
                if output.exists():
                    try:
                        answer = json.loads(output.read_text())
                        if answer.get("id") != mid or answer.get("status") != "completed":
                            continue
                        content = str(answer["content"])
                    except (ValueError, KeyError, OSError):
                        continue
                    db.execute("INSERT OR IGNORE INTO messages(id,issue_number,role,content,created_at,reply_to,delivery,source) VALUES(?,?,?,?,?,?,?,?)",
                               (mid + "_reply", row["issue_number"], "assistant", content,
                                answer.get("completed_at", now()), mid, "replied",
                                f"batbox://sessions/{answer.get('session_id', 'unknown')}/replies/{mid}"))
                    db.execute("UPDATE messages SET delivery='replied' WHERE id=?", (mid,))
                    db.execute("UPDATE issues SET updated_at=? WHERE number=?", (now(), row["issue_number"]))
                    continue
                inbox = self.root / "inbox" / (mid + ".json")
                processed = self.root / "processed" / (mid + ".json")
                if not inbox.exists() and not processed.exists():
                    atomic_json(inbox, {"id": mid, "issue_id": f"BB-{row['issue_number']}",
                                        "content": row["content"], "created_at": row["created_at"], "source": row["source"]})
                db.execute("UPDATE messages SET delivery='sent' WHERE id=?", (mid,))


def handler(store):
    class Handler(BaseHTTPRequestHandler):
        server_version = "BatBoxIssueAPI/1"

        def log_message(self, *_):
            pass  # No credentials, request text, or query strings in access logs.

        def respond(self, status, value):
            raw = json.dumps(value, ensure_ascii=False).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(raw)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(raw)

        def route(self):
            path = urlsplit(self.path).path.rstrip("/") or "/"
            if self.command == "GET" and path == "/health":
                return self.respond(200, {"status": "ok", "enabled": store.communication()["enabled"]})
            expected = "Bearer " + store.token
            if not hmac.compare_digest(self.headers.get("Authorization", ""), expected):
                return self.respond(401, {"error": "authentication required"})
            control = store.communication()
            if self.command == "GET" and path in {"/v1/capabilities", "/"}:
                return self.respond(200, {"service": "BatBox autonomous issue API", "communication": control,
                                          "statuses": sorted(STATUSES), "issues": "/v1/issues",
                                          "comments": "/v1/issues/{id}/comments", "status": "/v1/status"})
            if path.startswith("/v1/") and not control["enabled"]:
                return self.respond(503, {"error": "communication is disabled by the engine", "capabilities": "/v1/capabilities"})
            body = {}
            if self.command in {"POST", "PATCH"}:
                size = int(self.headers.get("Content-Length", "0"))
                if not 0 < size <= MAX_BODY:
                    return self.respond(413, {"error": "JSON body must contain 1..131072 bytes"})
                body = json.loads(self.rfile.read(size))
                if not isinstance(body, dict):
                    raise ValueError("JSON object required")
            store.reconcile()
            if path == "/v1/status" and self.command == "GET":
                return self.respond(200, {"communication": control, "engine": store.engine_status()})
            if path == "/v1/issues":
                if self.command == "GET":
                    return self.respond(200, {"issues": store.list()})
                if self.command == "POST":
                    return self.respond(201, store.create(body))
            match = re.fullmatch(r"/(?:v1|internal)/issues/(BB-[1-9][0-9]*)(?:/(comments|status))?", path)
            if match:
                key, sub = match.groups()
                if self.command == "GET" and sub is None:
                    return self.respond(200, store.get(key))
                if self.command == "POST" and sub == "comments":
                    return self.respond(202, store.comment(key, body))
                if (self.command == "POST" and sub == "status") or (self.command == "PATCH" and sub is None):
                    return self.respond(200, store.update(key, body))
            return self.respond(404, {"error": "endpoint not found"})

        def dispatch(self):
            self.connection.settimeout(15)
            try:
                self.route()
            except KeyError as exc:
                self.respond(404, {"error": str(exc)})
            except (ValueError, TypeError) as exc:
                self.respond(400, {"error": str(exc)})
            except (BrokenPipeError, ConnectionResetError, TimeoutError):
                pass
            except Exception as exc:
                print(json.dumps({"event": "api_error", "type": type(exc).__name__, "time": now()}), flush=True)
                self.respond(500, {"error": "internal error; request remains durable if accepted"})

        do_GET = do_POST = do_PATCH = dispatch
    return Handler


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--state-dir", default=str(Path.home() / ".batbox/autonomous"))
    p.add_argument("--port", type=int, default=8772)
    args = p.parse_args()
    os.umask(0o077)
    store = IssueStore(args.state_dir)
    server = ThreadingHTTPServer(("127.0.0.1", args.port), handler(store))
    stop = threading.Event()

    def reconcile():
        while not stop.wait(0.5):
            try:
                store.reconcile()
            except Exception as exc:
                print(json.dumps({"event": "reconcile_error", "type": type(exc).__name__, "time": now()}), flush=True)

    def shutdown(*_):
        stop.set()
        threading.Thread(target=server.shutdown, daemon=True).start()

    signal.signal(signal.SIGTERM, shutdown)
    signal.signal(signal.SIGINT, shutdown)
    threading.Thread(target=reconcile, daemon=True).start()
    print(json.dumps({"event": "api_started", "port": args.port, "state_dir": str(store.root)}), flush=True)
    server.serve_forever(poll_interval=0.5)
    server.server_close()


if __name__ == "__main__":
    main()
