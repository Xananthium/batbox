"""Isolated Scrapling worker. Secrets are read locally; only sanitized text exits."""
import http.cookiejar
import json
import logging
import os
import sys
from pathlib import Path
from urllib.parse import urlsplit


def origin(url):
    p = urlsplit(url)
    port = p.port or (443 if p.scheme == "https" else 80)
    host = f"[{p.hostname}]" if ":" in p.hostname else p.hostname
    return f"{p.scheme}://{host}" + (f":{port}" if port != (443 if p.scheme == "https" else 80) else "")


def run(options):
    os.umask(0o077)
    logging.disable(logging.CRITICAL)
    url = options["url"]
    secrets = json.loads(Path(options["secret_path"]).read_text()) if options.get("secret_path") else {}
    credentials = secrets.get("origins", {}).get(origin(url), {})
    headers = credentials.get("headers", {})
    if any(k.lower() in ("host", "cookie") for k in headers):
        raise ValueError("Use persisted cookies, not Host/Cookie credential headers")
    state = Path(options["state_path"]) if options.get("state_path") else None
    sensitive = [str(v) for v in headers.values()]
    status, redirect = 0, None
    if options["engine"] == "http":
        from scrapling.fetchers import FetcherSession
        jar = http.cookiejar.MozillaCookieJar(str(state) if state else None)
        if state and state.exists():
            jar.load(ignore_discard=True, ignore_expires=False)
        with FetcherSession(timeout=30, retries=1, follow_redirects=False, headers=headers,
                            stealthy_headers=False) as session:
            session._curl_session.cookies.jar = jar
            if options.get("login"):
                login = credentials.get("login", {})
                if login.get("url") != url or not isinstance(login.get("form"), dict):
                    raise ValueError("Configure an exact login.url and login.form in the private credential file")
                sensitive.extend(str(x) for x in login["form"].values())
                response = session.post(url, data=login["form"])
            else:
                response = session.get(url)
            status = response.status
            if 300 <= status < 400:
                redirect = response.headers.get("location") or response.headers.get("Location")
            selected = response.css(options["selector"]) if options.get("selector") else [response]
            content = "\n".join(str(item.get_all_text(strip=True, ignore_tags=("script", "style", "noscript"))) for item in selected)
            sensitive.extend(c.value for c in jar if c.value)
            if state:
                jar.save(ignore_discard=True, ignore_expires=False)
                state.chmod(0o600)
    else:
        from scrapling.fetchers import StealthySession
        allowed = set(options["allowed_origins"])
        with StealthySession(headless=True, timeout=30000, disable_resources=True, google_search=False,
                            additional_args={"service_workers": "block"}) as session:
            if state and state.exists():
                session.load_state(state)
            def boundary(route):
                request_origin = origin(route.request.url)
                if request_origin not in allowed:
                    route.abort()
                    return
                scoped = secrets.get("origins", {}).get(request_origin, {}).get("headers", {})
                credential_names = {k.lower() for values in secrets.get("origins", {}).values() for k in values.get("headers", {})} | {"authorization", "proxy-authorization"}
                clean = {k: v for k, v in route.request.headers.items() if k.lower() not in credential_names}
                route.continue_(headers={**clean, **scoped})
            session.context.route("**/*", boundary)
            login = credentials.get("browser_login", {}) if options.get("login") else {}
            if options.get("login") and login.get("url") != url:
                raise ValueError("Configure an exact browser_login.url in the private credential file")
            def action(page):
                for selector, value in login.get("fields", {}).items():
                    sensitive.append(str(value))
                    page.locator(selector).fill(str(value))
                if login.get("submit"):
                    page.locator(login["submit"]).click()
                    page.wait_for_load_state("domcontentloaded")
            response = session.fetch(url, page_action=action if login else None)
            status = response.status
            selected = response.css(options["selector"]) if options.get("selector") else [response]
            content = "\n".join(str(item.get_all_text(strip=True)) for item in selected)
            if state:
                session.save_state(state)
                state.chmod(0o600)
                saved = json.loads(state.read_text())
                sensitive.extend(c.get("value", "") for c in saved.get("cookies", []))
    for secret in sensitive:
        if secret and len(secret) > 3:
            content = content.replace(secret, "[redacted credential]")
    return {"status": status, "content": content[:40000], "truncated": len(content) > 40000, "redirect": redirect}


if __name__ == "__main__":
    try:
        print(json.dumps(run(json.load(sys.stdin))))
    except Exception as exc:
        # Do not propagate third-party exceptions containing request headers or credentials.
        error = str(exc) if isinstance(exc, ValueError) else f"Scrapling {type(exc).__name__}; inspect the site/session configuration"
        print(json.dumps({"error": error}))
        sys.exit(1)
