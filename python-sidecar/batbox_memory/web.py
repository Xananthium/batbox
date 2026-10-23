"""Cached SearXNG and Scrapling access with private, origin-scoped sessions."""
import ipaddress
import json
import os
import re
import socket
import subprocess
import threading
import urllib.parse
import urllib.request
from pathlib import Path

from .store import digest, timestamp


def origin(url):
    p = urllib.parse.urlsplit(url)
    if p.scheme not in ("http", "https") or not p.hostname or p.username or p.password:
        raise ValueError("Expected an HTTP(S) URL without embedded credentials")
    port = p.port or (443 if p.scheme == "https" else 80)
    host = f"[{p.hostname}]" if ":" in p.hostname else p.hostname
    return f"{p.scheme}://{host}" + (f":{port}" if port != (443 if p.scheme == "https" else 80) else "")


def check_public(url):
    origin(url)
    host = urllib.parse.urlsplit(url).hostname
    addresses = socket.getaddrinfo(host, None)
    if not addresses or any(not ipaddress.ip_address(x[4][0]).is_global for x in addresses):
        raise ValueError("Public browsing cannot target private/local addresses; configure an explicit named session origin")


def private_json(path, data):
    tmp = path.with_suffix(path.suffix + ".tmp")
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        json.dump(data, f)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)
    path.chmod(0o600)


class Web:
    def __init__(self, store, directory, scrapling_python, searxng="http://127.0.0.1:8888"):
        self.store = store
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.directory.chmod(0o700)
        self.secrets = self.directory / "secrets"
        self.secrets.mkdir(exist_ok=True, mode=0o700)
        self.secrets.chmod(0o700)
        self.scrapling_python = scrapling_python
        self.searxng = searxng.rstrip("/")
        self.locks = {}
        self.lock = threading.Lock()

    def session_path(self, name):
        if not isinstance(name, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", name):
            raise ValueError("session_id must be 1–64 letters, digits, underscores or hyphens")
        return self.directory / f"{name}.json"

    def configure(self, session_id, allowed_origins, engine="http"):
        path = self.session_path(session_id)
        if engine not in ("http", "browser"):
            raise ValueError("engine must be http or browser")
        if not isinstance(allowed_origins, list) or not allowed_origins or len(allowed_origins) > 20:
            raise ValueError("Supply 1–20 exact allowed origins")
        allowed = sorted(set(origin(u) for u in allowed_origins))
        old = json.loads(path.read_text()) if path.exists() else {}
        if old and (old["allowed_origins"] != allowed or old["engine"] != engine):
            raise ValueError("Existing sessions are immutable; choose a new session_id for a different scope")
        private_json(path, {"session_id": session_id, "allowed_origins": allowed, "engine": engine,
                            "created_at": old.get("created_at", timestamp())})
        return self.session_info(session_id)

    def session_info(self, session_id):
        path = self.session_path(session_id)
        config = json.loads(path.read_text())
        secret = self.secrets / f"{session_id}.json"
        state = self.directory / f"{session_id}.state"
        return config | {"credentials_configured": secret.is_file(), "state_saved": state.is_file(),
                         "credentials_file": str(secret)}

    def sessions(self):
        return {"sessions": [self.session_info(p.stem) for p in sorted(self.directory.glob("*.json"))]}

    def search(self, query, limit=5):
        if not isinstance(query, str) or not query.strip() or len(query) > 2000:
            raise ValueError("query must contain 1–2000 characters")
        limit = min(10, max(1, int(limit)))
        key = "search:" + digest(query + str(limit))
        cached = self.store.cache_get(key)
        if cached:
            return cached
        url = self.searxng + "/search?" + urllib.parse.urlencode({"q": query, "format": "json"})
        with urllib.request.urlopen(url, timeout=25) as response:
            data = json.loads(response.read(2 * 1024 * 1024))
        result = {"query": query, "results": [{k: r.get(k, "") for k in ("title", "url", "content")} for r in data.get("results", [])[:limit]],
                  "cached": False, "fetched_at": timestamp(), "source": url, "backend": "SearXNG"}
        self.store.cache_put(key, result, 3600)
        return result

    def fetch(self, url, session_id=None, selector=None, refresh=False, login=False):
        is_login = login
        if not isinstance(url, str) or len(url) > 4096:
            raise ValueError("URL too long")
        if selector is not None and (not isinstance(selector, str) or len(selector) > 500):
            raise ValueError("Invalid CSS selector")
        config = {"engine": "http", "allowed_origins": []}
        state_path, secret_path = None, None
        if session_id:
            config = json.loads(self.session_path(session_id).read_text())
            if origin(url) not in config["allowed_origins"]:
                raise ValueError("URL origin is outside this session's allowed_origins")
            state_path = self.directory / f"{session_id}.state"
            secret_path = self.secrets / f"{session_id}.json"
            if secret_path.exists() and (secret_path.stat().st_mode & 0o077):
                raise ValueError("Credential file must have mode 0600")
        else:
            if login:
                raise ValueError("Login requires a named session")
            check_public(url)
        with self.lock:
            session_lock = self.locks.setdefault(session_id or "public", threading.Lock())
        with session_lock:
            generation = "".join(digest(p.read_text()) for p in (state_path, secret_path) if p and p.exists())
            key = "fetch:" + digest(json.dumps([url, session_id, generation, selector, config["engine"]]))
            cached = self.store.cache_get(key) if not (refresh or login) else None
            if cached:
                return cached
            current = url
            for _ in range(6):
                # Check each redirect before a request is issued. Session credentials never cross scopes.
                if session_id:
                    if origin(current) not in config["allowed_origins"]:
                        raise ValueError("Redirect origin is outside this session's allowed_origins")
                else:
                    check_public(current)
                payload = {"url": current, "engine": config["engine"], "allowed_origins": config["allowed_origins"],
                           "state_path": str(state_path) if state_path else None,
                           "secret_path": str(secret_path) if secret_path and secret_path.exists() else None,
                           "selector": selector, "login": login}
                proc = subprocess.run([self.scrapling_python, str(Path(__file__).with_name("scrapling_fetch.py"))],
                                      input=json.dumps(payload), text=True, capture_output=True, timeout=75)
                if proc.returncode:
                    # stderr may include library request details. Return only the helper's sanitized error.
                    try:
                        error = json.loads(proc.stdout).get("error", "Scrapling request failed")
                    except ValueError:
                        error = "Scrapling request failed"
                    raise RuntimeError(error)
                result = json.loads(proc.stdout)
                if result.get("redirect"):
                    current = urllib.parse.urljoin(current, result["redirect"])
                    login = False
                    continue
                break
            else:
                raise ValueError("Too many redirects")
            result.update(url=current, requested_url=url, session_id=session_id, cached=False,
                          fetched_at=timestamp(), backend="Scrapling")
            if result.get("status", 500) >= 400:
                return result
            generation = "".join(digest(p.read_text()) for p in (state_path, secret_path) if p and p.exists())
            key = "fetch:" + digest(json.dumps([url, session_id, generation, selector, config["engine"]]))
            if not is_login:
                self.store.cache_put(key, result, 1800 if session_id else 21600)
            # Authenticated pages remain private session artifacts; public captures join long-term memory.
            if not session_id and result.get("content"):
                result["memory"] = self.store.enqueue(result["content"], current, "web:" + digest(current), "web_capture", result["fetched_at"])
            return result
