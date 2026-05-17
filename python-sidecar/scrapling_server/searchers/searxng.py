"""SearXNG JSON API search backend for the BatBox Scrapling sidecar.

Calls the SearXNG ``/search`` endpoint with ``format=json``.  The SearXNG
instance URL is read from the ``BATBOX_SEARXNG_URL`` environment variable or
supplied directly to ``search()``.

SearXNG JSON response shape (as of SearXNG 2024.x):

    {
        "query": "...",
        "results": [
            {
                "title": "...",
                "url": "...",
                "content": "...",     // snippet text
                "engine": "...",
                "score": 0.0,
                ...
            },
            ...
        ],
        ...
    }
"""

from __future__ import annotations

import logging
import os

import httpx

logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

_DEFAULT_SEARXNG_URL_ENV = "BATBOX_SEARXNG_URL"

# Reasonable desktop UA — some SearXNG instances block blank UA
_UA = (
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
    "AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/124.0.0.0 Safari/537.36"
)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _resolve_base_url(base_url: str) -> str:
    """Resolve the SearXNG base URL from argument or environment variable.

    Args:
        base_url: Caller-supplied base URL (may be empty string).

    Returns:
        The resolved base URL with no trailing slash.

    Raises:
        ValueError: If no URL can be resolved.
    """
    resolved = base_url.strip() or os.environ.get(_DEFAULT_SEARXNG_URL_ENV, "").strip()
    if not resolved:
        raise ValueError(
            "SearXNG URL not configured.  Set BATBOX_SEARXNG_URL or pass "
            "searxng_url in the request body."
        )
    return resolved.rstrip("/")


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------


def search(
    query: str,
    count: int = 10,
    base_url: str = "",
) -> list[dict[str, str]]:
    """Query a SearXNG instance and return structured results.

    Uses httpx so that gzip/brotli compression is handled automatically.

    Args:
        query: The search query string.
        count: Maximum number of results to return (1-50).
        base_url: SearXNG instance base URL.  Falls back to the
            ``BATBOX_SEARXNG_URL`` environment variable if empty.

    Returns:
        A list of dicts, each containing ``title``, ``url``, and ``snippet``.

    Raises:
        ValueError: If no SearXNG URL is configured.
        RuntimeError: If the HTTP request fails or returns a non-200 status.
    """
    count = max(1, min(50, count))
    resolved_base = _resolve_base_url(base_url)

    logger.info(
        "SearXNG search: base=%r query=%r count=%d", resolved_base, query, count
    )

    params = {
        "q": query,
        "format": "json",
        "language": "en",
        "safesearch": "0",
        "pageno": "1",
    }
    headers = {
        "User-Agent": _UA,
        "Accept": "application/json, */*;q=0.8",
        "Accept-Language": "en-US,en;q=0.9",
    }

    try:
        with httpx.Client(timeout=15, follow_redirects=True) as client:
            response = client.get(
                f"{resolved_base}/search",
                params=params,
                headers=headers,
            )
    except httpx.RequestError as exc:
        logger.error("SearXNG request failed: %s", exc)
        raise RuntimeError(f"SearXNG request failed: {exc}") from exc

    if response.status_code != 200:
        logger.warning(
            "SearXNG returned HTTP %d for query %r", response.status_code, query
        )
        raise RuntimeError(
            f"SearXNG returned HTTP {response.status_code} for query {query!r}"
        )

    try:
        data = response.json()
    except Exception as exc:
        logger.error("SearXNG returned invalid JSON: %s", exc)
        raise RuntimeError(f"SearXNG returned invalid JSON: {exc}") from exc

    raw_results: list[dict] = data.get("results", [])
    results: list[dict[str, str]] = []

    for item in raw_results:
        if len(results) >= count:
            break
        title = str(item.get("title", "")).strip()
        url = str(item.get("url", "")).strip()
        snippet = str(item.get("content", "")).strip()
        if title or url:
            results.append({"title": title, "url": url, "snippet": snippet})

    logger.info("SearXNG search returned %d results for %r", len(results), query)
    return results
