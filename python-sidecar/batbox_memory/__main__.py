"""Run the memory service: python -m batbox_memory --config /path/config.json."""
import argparse
import json
import logging
import os
import signal
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from .graph import Graph
from .store import Store, timestamp
from .web import Web

LOG = logging.getLogger("batbox.memory")


class Service:
    def __init__(self, config):
        self.config = config
        self.stop = threading.Event()
        self.store = Store(config["database"], config.get("embedding_model", "BAAI/bge-small-en-v1.5"), config.get("embedding_model_path"))
        self.graph = Graph(self.store, config["graph_directory"], config["llm_base_url"], config["llm_model"])
        self.web = Web(self.store, config["session_directory"], config["scrapling_python"], config.get("searxng", "http://127.0.0.1:8888"))
        self.started_at = timestamp()
        self.threads = []

    def ingest(self):
        return self.store.ingest(self.config.get("sources", []))

    def memory_worker(self):
        while not self.stop.is_set():
            try:
                if not self.store.process_one():
                    self.stop.wait(0.25)
            except Exception:
                LOG.exception("Memory indexing failed; job is durably queued for retry")
                self.stop.wait(1)

    def scan_worker(self):
        while not self.stop.is_set():
            try:
                self.ingest()
            except Exception:
                LOG.exception("Source scan failed")
            self.stop.wait(self.config.get("scan_seconds", 30))

    def start(self):
        for name, fn in (("memory", self.memory_worker), ("sources", self.scan_worker), ("hipporag", lambda: self.graph.run(self.stop))):
            thread = threading.Thread(name="batbox-" + name, target=fn, daemon=True)
            thread.start()
            self.threads.append(thread)

    def status(self):
        return self.store.stats() | {"started_at": self.started_at, "hipporag": dict(self.graph.status),
                                    "workers": {t.name: t.is_alive() for t in self.threads}}

    def dispatch(self, path, body):
        if path == "/ingest":
            return 202, self.ingest()
        if path == "/remember":
            return 202, self.store.enqueue(body["content"], body["source"], body.get("id"), body.get("kind", "observation"), body.get("captured_at"), body.get("metadata"))
        if path == "/recall":
            query, limit = body["query"], min(20, max(1, int(body.get("limit", 5))))
            if not isinstance(query, str) or not query.strip() or len(query) > 10000:
                raise ValueError("query must contain 1–10000 characters")
            # Fresh records are available through vector retrieval while the graph catches up.
            result = self.graph.recall(query, limit)
            if result is None:
                result = self.store.recall(query, limit)
                result["graph_state"] = self.graph.status["state"]
            return 200, result
        if path == "/search":
            return 200, self.web.search(body["query"], body.get("limit", 5))
        if path == "/fetch":
            return 200, self.web.fetch(body["url"], body.get("session_id"), body.get("selector"), bool(body.get("refresh", False)))
        if path == "/sessions/configure":
            return 200, self.web.configure(body["session_id"], body["allowed_origins"], body.get("engine", "http"))
        if path == "/sessions/status":
            return 200, self.web.session_info(body["session_id"]) if body.get("session_id") else self.web.sessions()
        if path == "/sessions/login":
            return 200, self.web.fetch(body["url"], body["session_id"], refresh=True, login=True)
        return 404, {"error": "Unknown endpoint"}


def handler_for(service):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass  # No request payloads, URLs, cookies, or credential values in access logs.

        def send(self, status, data):
            data = json.dumps(data, ensure_ascii=False).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            try:
                self.wfile.write(data)
            except BrokenPipeError:
                pass

        def do_GET(self):
            if self.path in ("/status", "/health"):
                self.send(200, service.status())
            elif self.path == "/sessions":
                self.send(200, service.web.sessions())
            else:
                self.send(404, {"error": "Unknown endpoint"})

        def do_POST(self):
            try:
                size = int(self.headers.get("Content-Length", "0"))
                if size < 1 or size > 600 * 1024:
                    self.send(413, {"error": "Expected JSON request no larger than 600 KiB"})
                    return
                body = json.loads(self.rfile.read(size))
                if not isinstance(body, dict):
                    raise ValueError("JSON object required")
                status, result = service.dispatch(self.path, body)
                self.send(status, result)
            except (ValueError, KeyError, TypeError, FileNotFoundError) as exc:
                self.send(400, {"error": str(exc)})
            except Exception as exc:
                LOG.exception("Memory API operation failed: %s", self.path)
                self.send(503, {"error": str(exc)[:1000]})
    return Handler


def main():
    parser = argparse.ArgumentParser(description="BatBox local memory, HippoRAG and authenticated Scrapling service")
    parser.add_argument("--config", required=True)
    args = parser.parse_args()
    os.umask(0o077)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s %(message)s")
    config = json.loads(Path(args.config).read_text())
    service = Service(config)
    server = ThreadingHTTPServer(("127.0.0.1", config.get("port", 8771)), handler_for(service))
    server.daemon_threads = True
    service.start()
    def shutdown(*_args):
        service.stop.set()
        threading.Thread(target=server.shutdown, daemon=True).start()
    signal.signal(signal.SIGTERM, shutdown)
    signal.signal(signal.SIGINT, shutdown)
    LOG.info("Memory API listening on loopback port %s", server.server_port)
    try:
        server.serve_forever()
    finally:
        service.stop.set()
        server.server_close()


if __name__ == "__main__":
    main()
