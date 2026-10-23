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
