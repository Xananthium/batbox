"""scrapling_server — BatBox loopback HTTP sidecar for WebFetch/WebSearch/WebSelect.

The C++ host (SidecarManager, src/sidecar/) spawns this package as a subprocess:

    python3 -m scrapling_server

Port discovery: the server prints ``LISTENING:<port>`` to stderr immediately
after binding so the C++ host can read it via the pipe attached to the child's
stderr file descriptor.

Endpoints (wired in CPP 7.7):
    GET  /healthz   — liveness probe (200 "ok")
    POST /fetch     — fetch a URL via Scrapling and return markdown
    POST /search    — DuckDuckGo HTML or SearXNG search
    POST /select    — CSS / XPath selector against a fetched page
    POST /shutdown  — graceful shutdown (calls os._exit(0) after responding)
"""

from scrapling_server.app import app  # noqa: F401 — re-exported for uvicorn

__all__ = ["app"]
