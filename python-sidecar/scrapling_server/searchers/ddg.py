"""DuckDuckGo HTML search backend for the BatBox Scrapling sidecar.

Scrapes the DuckDuckGo HTML endpoint (html.duckduckgo.com/html/?q=...) using
httpx (which handles compression/decompression automatically) and
BeautifulSoup4 for parsing.  No API key required.  The HTML endpoint is more
stable than the main site because DDG explicitly keeps it available for
accessibility.

Fragility note: DDG periodically tweaks CSS classes.  If titles or snippets
stop arriving, update the selectors in _SELECTORS below and run the unit test
against a recorded HTML fixture.
"""

from __future__ import annotations

import logging
import random
import time
import urllib.parse

import httpx
from bs4 import BeautifulSoup

logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

_DDG_HTML_ENDPOINT = "https://html.duckduckgo.com/html/"

# CSS selectors for result elements.  Listed as fallback chains so that if DDG
# changes one class name we still extract usable data.
_RESULT_BLOCK_SELECTORS = [".result", ".web-result", ".results_links"]
_TITLE_SELECTORS = [".result__title a", ".result__a", "h2 a"]
_SNIPPET_SELECTORS = [".result__snippet", ".result__body"]
_URL_SELECTORS = [".result__url", ".result__extras__url"]

# User-agent rotation pool — benign desktop UA strings.
_USER_AGENTS: list[str] = [
    (
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
        "AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/124.0.0.0 Safari/537.36"
    ),
    (
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/124.0.0.0 Safari/537.36"
    ),
    (
        "Mozilla/5.0 (X11; Linux x86_64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/124.0.0.0 Safari/537.36"
    ),
    (
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 14_4) "
        "AppleWebKit/605.1.15 (KHTML, like Gecko) "
        "Version/17.4 Safari/605.1.15"
    ),
]


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _pick_ua() -> str:
    """Return a random user-agent string from the rotation pool."""
    return random.choice(_USER_AGENTS)


def _select_first(soup: BeautifulSoup, selectors: list[str]) -> str:
    """Try each CSS selector in order and return the first non-empty text hit."""
    for sel in selectors:
        el = soup.select_one(sel)
        if el:
            return el.get_text(separator=" ", strip=True)
    return ""


def _resolve_result_url(block: BeautifulSoup) -> str:
    """Extract the canonical URL from a DDG result block.

    DDG encodes the real URL in the ``href`` of the result title link as a
    redirect URL of the form ``//duckduckgo.com/l/?uddg=<encoded>&rut=...``.
    We try to extract the ``uddg`` param first; fall back to plain text of the
    url element.
    """
    for sel in _TITLE_SELECTORS:
        el = block.select_one(sel)
        if not el:
            continue
        href = el.get("href", "")
        if href:
            try:
                # DDG redirect URLs look like //duckduckgo.com/l/?uddg=...
                # urllib.parse.urlparse handles protocol-relative URLs
                full_href = "https:" + href if href.startswith("//") else href
                parsed = urllib.parse.urlparse(full_href)
                params = urllib.parse.parse_qs(parsed.query)
                uddg = params.get("uddg", [])
                if uddg:
                    return urllib.parse.unquote(uddg[0])
            except Exception:
                pass
            # href is already a direct URL (no redirect wrapper)
            if href.startswith("http"):
                return href

    # Last resort: text content of the url element
    return _select_first(block, _URL_SELECTORS)


def _parse_results(html: str, count: int) -> list[dict[str, str]]:
    """Parse DDG HTML and return up to *count* result dicts."""
    soup = BeautifulSoup(html, "html.parser")

    # Find result blocks using the first selector that yields results
    blocks: list = []
    for sel in _RESULT_BLOCK_SELECTORS:
        blocks = soup.select(sel)
        if blocks:
            break

    results: list[dict[str, str]] = []
    for block in blocks:
        if len(results) >= count:
            break

        title = _select_first(block, _TITLE_SELECTORS)
        if not title:
            # Skip ad / navigational blocks that have no title text
            continue

        snippet = _select_first(block, _SNIPPET_SELECTORS)
        url = _resolve_result_url(block)

        if not url:
            url = _select_first(block, _URL_SELECTORS)

        # Skip ad results (DDG ad URLs resolve through duckduckgo.com/y.js)
        if "duckduckgo.com/y.js" in url:
            continue

        results.append({"title": title, "url": url, "snippet": snippet})

    return results


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------


def search(query: str, count: int = 10) -> list[dict[str, str]]:
    """Search DuckDuckGo via its HTML endpoint and return structured results.

    Uses httpx for the HTTP request so that gzip/brotli compression is handled
    automatically (urllib does not decompress brotli responses).

    Args:
        query: The search query string.
        count: Maximum number of results to return (1-50).

    Returns:
        A list of dicts, each containing ``title``, ``url``, and ``snippet``.
        Returns an empty list if no results are found or on network error.

    Raises:
        RuntimeError: If DDG returns a non-200 response (unexpected server error).
    """
    count = max(1, min(50, count))
    params = {"q": query, "kl": "us-en", "kp": "-2"}
    headers = {
        "User-Agent": _pick_ua(),
        "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
        "Accept-Language": "en-US,en;q=0.9",
        "DNT": "1",
        "Upgrade-Insecure-Requests": "1",
    }

    logger.info("DDG search: query=%r count=%d", query, count)

    try:
        with httpx.Client(timeout=15, follow_redirects=True) as client:
            response = client.get(
                _DDG_HTML_ENDPOINT,
                params=params,
                headers=headers,
            )
    except httpx.RequestError as exc:
        logger.error("DDG request failed: %s", exc)
        raise RuntimeError(f"DDG request failed: {exc}") from exc

    if response.status_code != 200:
        logger.warning("DDG returned HTTP %d for query %r", response.status_code, query)
        raise RuntimeError(
            f"DDG returned HTTP {response.status_code} for query {query!r}"
        )

    html = response.text  # httpx decodes and decompresses automatically
    results = _parse_results(html, count)
    logger.info("DDG search returned %d results for %r", len(results), query)

    # Small courtesy delay to avoid hammering DDG on rapid successive calls
    if len(results) > 0:
        time.sleep(0.1)

    return results
