import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from concurrent.futures import ThreadPoolExecutor
from urllib.error import HTTPError
from urllib.request import Request, urlopen

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
from engine_api import IssueStore, atomic_json, handler, ThreadingHTTPServer


class IssuesTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.store = IssueStore(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_accept_is_durable_and_reply_import_idempotent(self):
        issue = self.store.create({"title": "Remember the mission", "description": "Use its source URL."})
        key = issue["id"]
        mid = issue["messages"][0]["id"]
        incoming = self.store.root / "inbox" / (mid + ".json")
        self.assertEqual(json.loads(incoming.read_text())["issue_id"], key)
        incoming.unlink()  # Simulate a crash after DB commit and before inbox publish.
        restored = IssueStore(self.tmp.name)
        restored.reconcile()
        self.assertTrue(incoming.exists())
        atomic_json(restored.root / "outbox" / (mid + ".json"), {
            "id": mid, "status": "completed", "content": "Retrieved the source.", "session_id": "test"})
        restored.reconcile()
        restored.reconcile()
        result = restored.get(key)
        self.assertEqual(len(result["messages"]), 2)
        self.assertEqual(result["messages"][0]["delivery"], "replied")
        self.assertEqual(result["status"], "todo")  # A reply doesn't close the issue.
        restored.update(key, {"status": "done", "reason": "verified"})
        self.assertEqual(restored.get(key)["status"], "done")
        self.assertEqual(len(restored.get(key)["events"]), 2)

    def test_backlog_activation_and_reopen(self):
        issue = self.store.create({"title": "Later", "status": "backlog"})
        self.assertEqual(issue["messages"], [])
        self.store.update(issue["id"], {"status": "todo"})
        self.store.update(issue["id"], {"status": "done"})
        self.store.update(issue["id"], {"status": "todo"})
        self.assertEqual(len(self.store.get(issue["id"])["messages"]), 2)
        with self.assertRaises(ValueError):
            self.store.update(issue["id"], {"status": "made-up"})

    def test_api_auth_and_engine_controls_availability(self):
        server = ThreadingHTTPServer(("127.0.0.1", 0), handler(self.store))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        url = f"http://127.0.0.1:{server.server_port}"
        try:
            with self.assertRaises(HTTPError) as err:
                urlopen(url + "/v1/issues")
            self.assertEqual(err.exception.code, 401)
            headers = {"Authorization": "Bearer " + self.store.token}
            with self.assertRaises(HTTPError) as err:
                urlopen(Request(url + "/v1/issues", headers=headers))
            self.assertEqual(err.exception.code, 503)
            atomic_json(self.store.root / "communication.json", {"enabled": True})
            response = json.load(urlopen(Request(url + "/v1/issues", headers=headers)))
            self.assertEqual(response, {"issues": []})
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_status_uses_current_worker_journal_without_refreshing_coordinator(self):
        root = self.store.root
        snapshot = {"pid": 42, "phase": "running", "cycle": 6,
            "updated_at": "2026-10-23T17:35:31Z",
            "workers": [{"id": "worker-a", "name": "maintainer", "status": "running", "tokens": 0}]}
        atomic_json(root / "status.json", snapshot)
        journal = {"agent_id": "worker-a", "status": "errored", "phase": "retry_wait",
            "updated_at": "2026-10-23T17:38:06Z", "last_progress_at": "2026-10-23T17:28:06Z",
            "last_operation": "prompt_tokenized", "recovery_failures": 1, "recovery_attempts": 0,
            "retry_delay_seconds": 2, "retry_not_before_epoch_ms": 1792777088000,
            "prompt": "PRIVATE_PROMPT", "inbox": ["PRIVATE_INBOX"],
            "last_result": "PRIVATE_RESULT", "session_id": "PRIVATE_SESSION",
            "last_error": "PRIVATE_ERROR_DETAIL", "spec": {"prompt_body": "PRIVATE_ROLE"}}
        atomic_json(root / "workers/worker-a.json", journal)
        atomic_json(root / "communication.json", {"enabled": True})
        server = ThreadingHTTPServer(("127.0.0.1", 0), handler(self.store))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        url = f"http://127.0.0.1:{server.server_port}/v1/status"
        try:
            with self.assertRaises(HTTPError) as err:
                urlopen(url)
            self.assertEqual(err.exception.code, 401)
            request = Request(url, headers={"Authorization": "Bearer " + self.store.token})
            response = json.load(urlopen(request))["engine"]
            self.assertEqual(response["updated_at"], snapshot["updated_at"])
            self.assertEqual(response["coordinator_snapshot_at"], snapshot["updated_at"])
            self.assertEqual(response["phase"], "running")
            worker = response["workers"][0]
            self.assertEqual(worker["status"], "errored")
            self.assertEqual(worker["phase"], "retry_wait")
            self.assertEqual(worker["status_source"], "worker_journal")
            self.assertNotIn("tokens", worker)
            for key in ("updated_at", "last_progress_at", "last_operation", "recovery_failures",
                        "recovery_attempts", "retry_delay_seconds", "retry_not_before_epoch_ms"):
                self.assertEqual(worker[key], journal[key])
            self.assertNotIn("PRIVATE_", json.dumps(response))
            self.assertEqual(json.loads((root / "status.json").read_text()), snapshot)
            journal.update(status="running", phase="compacting", progress_sequence=7)
            atomic_json(root / "workers/worker-a.json", journal)
            refreshed = json.load(urlopen(request))["engine"]
            self.assertEqual(refreshed["workers"][0]["phase"], "compacting")
            self.assertEqual(refreshed["workers"][0]["progress_sequence"], 7)
            self.assertEqual(refreshed["updated_at"], snapshot["updated_at"])
            (root / "workers/worker-a.json").write_text("{invalid journal")
            fallback = json.load(urlopen(request))["engine"]["workers"][0]
            self.assertEqual(fallback["status_source"], "coordinator_snapshot")
            self.assertEqual(fallback["journal_error"], "unavailable")
            self.assertEqual(fallback["status"], "running")
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_status_survives_missing_corrupt_journals_and_discovers_recovering_handle(self):
        root = self.store.root
        atomic_json(root / "status.json", {"updated_at": "original-snapshot", "workers": [
            {"id": "missing", "status": "running"}, {"id": "corrupt", "status": "running"},
            {"id": "mismatch", "status": "running"}, {"id": "nonobject", "status": "running"}]})
        atomic_json(root / "workers/recovering.json", {"agent_id": "recovering", "recoverable": True,
            "status": "errored", "phase": "retry_wait", "retry_delay_seconds": 4})
        (root / "workers/corrupt.json").write_text("{broken")
        atomic_json(root / "workers/mismatch.json", {"agent_id": "someone-else", "status": "running"})
        atomic_json(root / "workers/nonobject.json", ["not a journal"])
        atomic_json(root / "workers/closed.json", {"agent_id": "closed", "recoverable": False,
            "status": "completed"})
        result = self.store.engine_status()
        workers = {w["id"]: w for w in result["workers"]}
        self.assertEqual(workers["missing"]["journal_error"], "missing")
        for agent_id in ("missing", "corrupt", "mismatch", "nonobject"):
            self.assertEqual(workers[agent_id]["status_source"], "coordinator_snapshot")
            self.assertIn("journal_error", workers[agent_id])
        self.assertEqual(workers["recovering"]["status_source"], "worker_journal")
        self.assertEqual(workers["recovering"]["phase"], "retry_wait")
        self.assertNotIn("closed", workers)
        self.assertEqual(result["updated_at"], "original-snapshot")

    def test_concurrent_comments_and_reconciliation_do_not_lose_messages(self):
        issue = self.store.create({"title": "Concurrent work"})
        def submit(i):
            if i % 3 == 0:
                self.store.reconcile()
            return self.store.comment(issue["id"], {"content": f"message {i}"})["id"]
        with ThreadPoolExecutor(max_workers=6) as pool:
            identifiers = list(pool.map(submit, range(18)))
        self.assertEqual(len(set(identifiers)), 18)
        restored = IssueStore(self.tmp.name)
        restored.reconcile()
        self.assertEqual(len(restored.get(issue["id"])["messages"]), 19)
        self.assertEqual(len(list((restored.root / "inbox").glob("*.json"))), 19)


if __name__ == "__main__":
    unittest.main()
