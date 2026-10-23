import json
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from batbox_memory.store import Store
from batbox_memory.web import Web, origin, private_json


class Handler(BaseHTTPRequestHandler):
    calls = []

    def log_message(self, *_args):
        pass

    def do_POST(self):
        form = self.rfile.read(int(self.headers.get("Content-Length", 0))).decode()
        if form != "password=test-secret":
            self.send_error(403)
            return
        self.send_response(200)
        self.send_header("Set-Cookie", "login=test-session-cookie; Path=/; HttpOnly")
        self.end_headers()
        self.wfile.write(b"login established")

    def do_GET(self):
        type(self).calls.append(self.path)
        if self.path in ("/browser-seed", "/browser-check"):
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            script = "localStorage.setItem('durable','yes');document.cookie='browserstate=retained;path=/';document.body.innerText='state saved';" if self.path == "/browser-seed" else "document.body.innerText=(localStorage.getItem('durable')==='yes' && document.cookie.includes('browserstate=retained'))?'browser state restored':'missing state';"
            self.wfile.write(("<html><body><script>" + script + "</script></body></html>").encode())
            return
        if self.path == "/redirect":
            self.send_response(302)
            self.send_header("Location", "http://127.0.0.1:9/credential-leak")
            self.end_headers()
            return
        if self.headers.get("Authorization") != "Bearer test-private-token" or "login=test-session-cookie" not in self.headers.get("Cookie", ""):
            self.send_error(403)
            return
        self.send_response(200)
        self.end_headers()
        self.wfile.write(b"<html><main>authenticated private document</main></html>")


class SessionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.python = Path.home() / ".local/share/claude-private/venv/bin/python"
        if not cls.python.is_file():
            raise unittest.SkipTest("installed Scrapling environment required")
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.base = f"http://127.0.0.1:{cls.server.server_port}"

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.store = Store(Path(self.temp.name) / "memory.db")
        self.web = Web(self.store, Path(self.temp.name) / "sessions", str(self.python))
        self.web.configure("account", [self.base])
        private_json(self.web.secrets / "account.json", {"origins": {self.base: {
            "headers": {"Authorization": "Bearer test-private-token"},
            "login": {"url": self.base + "/login", "form": {"password": "test-secret"}}
        }}})

    def tearDown(self):
        self.temp.cleanup()

    def test_cookie_login_restore_cache_partition_and_origin_boundary(self):
        self.assertEqual(self.web.fetch(self.base + "/login", "account", login=True)["status"], 200)
        restored = Web(self.store, self.web.directory, str(self.python))
        result = restored.fetch(self.base + "/private", "account")
        self.assertIn("authenticated private document", result["content"])
        self.assertNotIn("test-private-token", json.dumps(result))
        self.assertTrue(restored.fetch(self.base + "/private", "account")["cached"])
        self.assertEqual((restored.directory / "account.state").stat().st_mode & 0o777, 0o600)
        with self.assertRaises(ValueError):
            restored.fetch("http://127.0.0.1:9/leak", "account")
        with self.assertRaises(ValueError):
            restored.fetch(self.base + "/redirect", "account")
        self.web.configure("different", [self.base])
        other = self.web.fetch(self.base + "/private", "different")
        self.assertEqual(other["status"], 403)
        self.assertFalse(other["cached"])

    def test_secret_permission_and_invalid_session_rejected(self):
        (self.web.secrets / "account.json").chmod(0o644)
        with self.assertRaises(ValueError):
            self.web.fetch(self.base + "/private", "account")
        with self.assertRaises(ValueError):
            self.web.configure("../../escape", [self.base])
        with self.assertRaises(ValueError):
            self.web.configure("account", ["https://different.example"])

    def test_url_embedded_credentials_rejected(self):
        with self.assertRaises(ValueError):
            origin("https://user:password@example.com")

    def test_browser_local_storage_and_cookies_survive_a_fresh_process(self):
        if not list((Path.home() / ".cache/ms-playwright").glob("chromium*")):
            self.skipTest("Playwright Chromium not installed")
        self.web.configure("browser", [self.base], "browser")
        first = self.web.fetch(self.base + "/browser-seed", "browser")
        self.assertIn("state saved", first["content"])
        restored = Web(self.store, self.web.directory, str(self.python))
        second = restored.fetch(self.base + "/browser-check", "browser")
        self.assertIn("browser state restored", second["content"])


if __name__ == "__main__":
    unittest.main()
