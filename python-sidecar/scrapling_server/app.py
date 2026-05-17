"""FastAPI application — BatBox Scrapling sidecar.

Endpoints:
    GET  /healthz             — liveness probe; returns {"status": "ok"}
    POST /fetch               — fetch a URL via Scrapling, return markdown body
    POST /search              — web search (DDG HTML default, SearXNG optional)
    POST /select              — CSS / XPath selector query against a fetched page
    POST /shutdown            — graceful shutdown

Implemented as part of task CPP 7.7.

Logging is controlled by the ``BATBOX_SIDECAR_LOG_LEVEL`` environment variable
(default: INFO).  Valid values: DEBUG, INFO, WARNING, ERROR, CRITICAL.
"""

from __future__ import annotations

import asyncio
import logging
import os
import re
from datetime import datetime, timezone
from typing import Any

from fastapi import BackgroundTasks, FastAPI, Request
from fastapi.responses import JSONResponse
from bs4 import BeautifulSoup
from markdownify import markdownify
from pydantic import BaseModel, Field
from scrapling.fetchers import Fetcher
from scrapling.parser import Selector

from scrapling_server.searchers import ddg as _ddg
from scrapling_server.searchers import searxng as _searxng

# ---------------------------------------------------------------------------
# Logging setup
# ---------------------------------------------------------------------------

_LOG_LEVEL_RAW = os.environ.get("BATBOX_SIDECAR_LOG_LEVEL", "INFO").upper()
_LOG_LEVEL = getattr(logging, _LOG_LEVEL_RAW, logging.INFO)

logging.basicConfig(
    level=_LOG_LEVEL,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    datefmt="%Y-%m-%dT%H:%M:%S",
)
logger = logging.getLogger("scrapling_server")

# Suppress the Scrapling "This logic is deprecated" warning that fires on every
# Fetcher instantiation in Scrapling < 0.5 — it's internal noise, not actionable.
logging.getLogger("scrapling").setLevel(
    max(_LOG_LEVEL, logging.WARNING)
)

# ---------------------------------------------------------------------------
# FastAPI app
# ---------------------------------------------------------------------------

app = FastAPI(
    title="scrapling_server",
    description="BatBox loopback HTTP sidecar for WebFetch / WebSearch / WebSelect",
    version="0.1.0",
    # Disable the /docs and /redoc UIs in production to reduce attack surface.
    # Toggle via BATBOX_SIDECAR_DEBUG=1 during development.
    docs_url="/docs" if os.environ.get("BATBOX_SIDECAR_DEBUG") else None,
    redoc_url="/redoc" if os.environ.get("BATBOX_SIDECAR_DEBUG") else None,
)

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

_DEFAULT_MAX_BYTES: int = 5_242_880  # 5 MiB — matches BATBOX_WEBFETCH_MAX_BYTES
_TRUNCATED_MARKER: str = "\n\n---\n[TRUNCATED: response exceeded max_bytes limit]\n"
_URL_PATTERN: re.Pattern[str] = re.compile(
    r"^(?:https?|ftp)://[^\s/$.?#].[^\s]*$", re.IGNORECASE
)

# ---------------------------------------------------------------------------
# Request / response models (contract surface for ScraplingProto.hpp)
# ---------------------------------------------------------------------------


class FetchRequest(BaseModel):
    """Body for POST /fetch."""

    url: str = Field(..., description="Target URL to fetch.")
    timeout: float = Field(30.0, description="Request timeout in seconds.")
    stealth: bool = Field(
        False,
        description="If true, use StealthyFetcher (headless Chrome). "
        "Slower but bypasses bot-detection. Default: Fetcher (curl_cffi).",
    )
    respect_robots: bool = Field(
        True,
        description="Honour robots.txt. Maps to BATBOX_RESPECT_ROBOTS.",
    )
    max_bytes: int = Field(
        _DEFAULT_MAX_BYTES,
        description="Maximum response body size in bytes before truncation.",
        ge=1,
    )


class FetchResponse(BaseModel):
    """Response for POST /fetch."""

    url: str
    markdown: str
    status_code: int
    content_type: str = ""
    content_length: int = 0
    fetched_at: str = ""
    truncated: bool = False
    is_error: bool = False
    error_message: str = ""


class SearchRequest(BaseModel):
    """Body for POST /search."""

    query: str = Field(..., description="Search query string.")
    n: int = Field(10, ge=1, le=50, description="Number of results to return.")
    engine: str = Field(
        "ddg",
        pattern="^(ddg|searxng)$",
        description="Search engine: 'ddg' (DuckDuckGo HTML) or 'searxng'.",
    )
    searxng_url: str = Field(
        "",
        description="SearXNG instance base URL. Required when engine='searxng'. "
        "Falls back to BATBOX_SEARXNG_URL env var if empty.",
    )


class SearchResult(BaseModel):
    """A single search result entry."""

    title: str
    url: str
    snippet: str


class SearchResponse(BaseModel):
    """Response for POST /search."""

    query: str
    engine: str
    results: list[SearchResult]
    is_error: bool = False
    error_message: str = ""


class SelectRequest(BaseModel):
    """Body for POST /select."""

    url: str = Field(
        ...,
        description=(
            "Page URL to fetch and query, OR raw HTML string to parse directly. "
            "If the value begins with 'http://' or 'https://', it is fetched first. "
            "Otherwise it is treated as raw HTML content."
        ),
    )
    selector: str = Field(
        ...,
        description="CSS selector or XPath expression. "
        "XPath strings must begin with '//' or '('.",
    )
    timeout: float = Field(30.0, description="Fetch timeout in seconds.")
    stealth: bool = Field(False, description="Use StealthyFetcher for the fetch step.")
    attribute: str = Field(
        "",
        description="If non-empty, return this attribute from each matched element "
        "instead of the element's text content.",
    )


class SelectResponse(BaseModel):
    """Response for POST /select."""

    url: str
    selector: str
    matches: list[str]
    count: int = 0
    is_error: bool = False
    error_message: str = ""


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _now_utc() -> str:
    """Return the current UTC timestamp as an ISO-8601 string."""
    return datetime.now(timezone.utc).isoformat()


def _is_url(value: str) -> bool:
    """Return True if *value* looks like a fetchable HTTP/HTTPS/FTP URL."""
    return bool(_URL_PATTERN.match(value.strip()))


def _html_to_markdown(html_content: str) -> str:
    """Convert an HTML string to Markdown using markdownify.

    Uses BeautifulSoup to fully decompose noise nodes (script, style,
    noscript, head, meta, link, template, svg) before conversion, so that
    their *text content* is also eliminated — not just their tags.  Plain
    ``markdownify(strip=[...])`` only removes the wrapper tags, leaving
    minified JS/CSS text in the output.

    Args:
        html_content: Raw HTML string to convert.

    Returns:
        Clean ATX-style Markdown containing only visible page content.
    """
    _DECOMPOSE_TAGS = [
        "script", "style", "noscript", "head",
        "meta", "link", "template", "svg",
    ]
    soup = BeautifulSoup(html_content, "html.parser")
    for tag_name in _DECOMPOSE_TAGS:
        for node in soup.find_all(tag_name):
            node.decompose()
    return markdownify(
        str(soup),
        heading_style="ATX",
        newline_style="backslash",
    )


def _truncate_to_bytes(text: str, max_bytes: int) -> tuple[str, bool]:
    """Truncate *text* so that its UTF-8 encoding is at most *max_bytes* bytes.

    Returns a tuple of (possibly-truncated text, was_truncated).  If
    truncation is needed, a clear marker is appended to the result.
    """
    encoded = text.encode("utf-8")
    if len(encoded) <= max_bytes:
        return text, False

    # Binary-truncate then decode safely to avoid splitting multibyte chars
    marker_bytes = _TRUNCATED_MARKER.encode("utf-8")
    target = max_bytes - len(marker_bytes)
    if target <= 0:
        # max_bytes is smaller than the marker itself; return just the marker
        return _TRUNCATED_MARKER, True
    truncated_encoded = encoded[:target]
    # Decode with errors='ignore' to drop any partial multi-byte sequence
    truncated_text = truncated_encoded.decode("utf-8", errors="ignore")
    return truncated_text + _TRUNCATED_MARKER, True


def _fetch_page(url: str, timeout: float, stealth: bool) -> Any:
    """Fetch a URL with Scrapling and return the response object.

    Args:
        url: Target URL.
        timeout: Request timeout in seconds.
        stealth: If True, use StealthyFetcher (headless Chrome).

    Returns:
        A Scrapling ``Response`` object.

    Raises:
        RuntimeError: On any network or HTTP error.
    """
    if stealth:
        # Import lazily — StealthyFetcher pulls in Playwright which is optional
        try:
            from scrapling.fetchers import StealthyFetcher  # noqa: PLC0415
            fetcher: Any = StealthyFetcher()
        except ImportError as exc:
            raise RuntimeError(
                "stealth=true requires Playwright (pip install scrapling[playwright])"
            ) from exc
    else:
        fetcher = Fetcher()

    logger.debug("Fetching URL: %s (stealth=%s, timeout=%s)", url, stealth, timeout)
    try:
        response = fetcher.get(url, timeout=timeout)
    except Exception as exc:
        raise RuntimeError(f"Fetch error for {url!r}: {exc}") from exc

    return response


def _extract_selector_matches(
    selector_obj: Any,
    selector: str,
    attribute: str,
) -> list[str]:
    """Apply a CSS or XPath selector to a Scrapling Selector object.

    Args:
        selector_obj: A ``scrapling.parser.Selector`` instance wrapping the HTML.
        selector: CSS selector string or XPath expression (must start with
            ``//`` or ``(`` to be treated as XPath).
        attribute: If non-empty, extract this attribute from each match instead
            of the element's text content.

    Returns:
        A list of strings (text or attribute values) for each matched element.
    """
    is_xpath = selector.startswith("//") or selector.startswith("(")

    if is_xpath:
        elements = selector_obj.xpath(selector)
    else:
        elements = selector_obj.css(selector)

    matches: list[str] = []
    for el in elements:
        if attribute:
            val = el.attrib.get(attribute, "")
            matches.append(val)
        else:
            matches.append(el.text or "")

    return matches


# ---------------------------------------------------------------------------
# Endpoints
# ---------------------------------------------------------------------------


@app.get("/healthz", response_class=JSONResponse, tags=["lifecycle"])
async def healthz() -> dict[str, str]:
    """Liveness probe.

    The C++ SidecarManager polls this endpoint every 100 ms after spawning the
    sidecar.  First HTTP 200 transitions state to Running.

    Returns:
        {"status": "ok"}
    """
    return {"status": "ok"}


@app.post("/fetch", response_model=FetchResponse, tags=["web"])
async def fetch(body: FetchRequest) -> FetchResponse:
    """Fetch a URL via Scrapling and return the page content as Markdown.

    Uses ``Fetcher`` (curl_cffi) by default; set ``stealth=true`` for
    ``StealthyFetcher`` (headless Chrome, slower).  The response body is
    converted from HTML to Markdown using markdownify.  If the Markdown
    content exceeds ``max_bytes``, it is truncated with a clear marker and
    ``truncated=true`` is set in the response.

    Args:
        body: FetchRequest with target URL and fetch options.

    Returns:
        FetchResponse with markdown content and metadata.
    """
    logger.info(
        "POST /fetch url=%r stealth=%s timeout=%s max_bytes=%s",
        body.url,
        body.stealth,
        body.timeout,
        body.max_bytes,
    )

    try:
        response = await asyncio.get_event_loop().run_in_executor(
            None, _fetch_page, body.url, body.timeout, body.stealth
        )
    except RuntimeError as exc:
        logger.warning("Fetch failed: %s", exc)
        return FetchResponse(
            url=body.url,
            markdown="",
            status_code=0,
            is_error=True,
            error_message=str(exc),
            fetched_at=_now_utc(),
        )

    # Convert the HTML content to Markdown
    html_str = str(response.html_content) if response.html_content else ""
    markdown_raw = _html_to_markdown(html_str)
    markdown, was_truncated = _truncate_to_bytes(markdown_raw, body.max_bytes)

    content_type = response.headers.get("content-type", "").split(";")[0].strip()
    content_length = len(html_str.encode("utf-8"))
    final_url = str(response.url) if response.url else body.url

    logger.info(
        "Fetch OK: url=%r status=%d content_type=%r markdown_len=%d truncated=%s",
        final_url,
        response.status,
        content_type,
        len(markdown),
        was_truncated,
    )

    return FetchResponse(
        url=final_url,
        markdown=markdown,
        status_code=response.status,
        content_type=content_type,
        content_length=content_length,
        fetched_at=_now_utc(),
        truncated=was_truncated,
    )


@app.post("/search", response_model=SearchResponse, tags=["web"])
async def search(body: SearchRequest) -> SearchResponse:
    """Search the web via DuckDuckGo HTML scraping or SearXNG JSON API.

    The ``engine`` field selects the backend:
    - ``"ddg"`` (default): scrapes ``html.duckduckgo.com/html/?q=...`` —
      no API key required.
    - ``"searxng"``: calls the SearXNG ``/search?format=json`` endpoint;
      the instance URL is taken from ``searxng_url`` in the request body or
      the ``BATBOX_SEARXNG_URL`` environment variable.

    Args:
        body: SearchRequest with query, result count, and engine selection.

    Returns:
        SearchResponse with a list of title/url/snippet result dicts.
    """
    logger.info(
        "POST /search query=%r engine=%r n=%d", body.query, body.engine, body.n
    )

    try:
        if body.engine == "ddg":
            raw_results = await asyncio.get_event_loop().run_in_executor(
                None, _ddg.search, body.query, body.n
            )
        else:
            # engine == "searxng" (validated by Pydantic pattern)
            raw_results = await asyncio.get_event_loop().run_in_executor(
                None, _searxng.search, body.query, body.n, body.searxng_url
            )
    except (RuntimeError, ValueError) as exc:
        logger.warning("Search failed (engine=%s): %s", body.engine, exc)
        return SearchResponse(
            query=body.query,
            engine=body.engine,
            results=[],
            is_error=True,
            error_message=str(exc),
        )

    results = [
        SearchResult(
            title=r.get("title", ""),
            url=r.get("url", ""),
            snippet=r.get("snippet", ""),
        )
        for r in raw_results
    ]

    logger.info(
        "Search OK: engine=%r query=%r results=%d", body.engine, body.query, len(results)
    )

    return SearchResponse(
        query=body.query,
        engine=body.engine,
        results=results,
    )


@app.post("/select", response_model=SelectResponse, tags=["web"])
async def select(body: SelectRequest) -> SelectResponse:
    """Fetch a URL (or parse raw HTML) and apply a CSS selector or XPath expression.

    The ``url`` field accepts either:
    - An HTTP/HTTPS URL — the page is fetched first, then the selector applied.
    - A raw HTML string — parsed directly without any network request.

    XPath expressions are detected automatically: if the selector begins with
    ``//`` or ``(``, XPath is used; otherwise CSS selector is assumed.

    Args:
        body: SelectRequest with the HTML source, selector, and options.

    Returns:
        SelectResponse with a list of matched text or attribute values.
    """
    logger.info(
        "POST /select selector=%r is_url=%s stealth=%s",
        body.selector,
        _is_url(body.url),
        body.stealth,
    )

    html_content: str = ""
    source_url: str = body.url

    if _is_url(body.url):
        # Fetch the page first
        try:
            response = await asyncio.get_event_loop().run_in_executor(
                None, _fetch_page, body.url, body.timeout, body.stealth
            )
            html_content = str(response.html_content) if response.html_content else ""
            source_url = str(response.url) if response.url else body.url
        except RuntimeError as exc:
            logger.warning("Select fetch failed: %s", exc)
            return SelectResponse(
                url=body.url,
                selector=body.selector,
                matches=[],
                count=0,
                is_error=True,
                error_message=str(exc),
            )
    else:
        # Treat the value as raw HTML
        html_content = body.url

    if not html_content.strip():
        return SelectResponse(
            url=source_url,
            selector=body.selector,
            matches=[],
            count=0,
            is_error=True,
            error_message="Empty HTML content — nothing to select from.",
        )

    try:
        page = Selector(content=html_content)
        matches = _extract_selector_matches(page, body.selector, body.attribute)
    except Exception as exc:
        logger.warning("Selector error: %s", exc)
        return SelectResponse(
            url=source_url,
            selector=body.selector,
            matches=[],
            count=0,
            is_error=True,
            error_message=f"Selector error: {exc}",
        )

    logger.info(
        "Select OK: selector=%r matches=%d", body.selector, len(matches)
    )

    return SelectResponse(
        url=source_url,
        selector=body.selector,
        matches=matches,
        count=len(matches),
    )


@app.post("/shutdown", tags=["lifecycle"])
async def shutdown(background_tasks: BackgroundTasks) -> dict[str, bool]:
    """Graceful shutdown endpoint.

    Returns HTTP 200 ``{"shutting_down": true}`` immediately, then schedules
    an ``os._exit(0)`` via a BackgroundTask so the C++ host receives the
    response before the process exits.

    The C++ SidecarManager sends POST /shutdown with a 1s timeout on batbox
    exit.  If the sidecar is still alive after 1s the host sends SIGTERM, then
    SIGKILL after 2s.
    """
    logger.info("POST /shutdown — scheduling graceful exit")

    async def _do_exit() -> None:
        # Brief delay to allow the HTTP response to be flushed to the client
        await asyncio.sleep(0.1)
        logger.info("Exiting sidecar process (os._exit(0))")
        os._exit(0)  # noqa: SLF001 — intentional; cleanest shutdown from async context

    background_tasks.add_task(_do_exit)
    return {"shutting_down": True}


# ---------------------------------------------------------------------------
# Global exception handler
# ---------------------------------------------------------------------------


@app.exception_handler(Exception)
async def generic_exception_handler(request: Request, exc: Exception) -> JSONResponse:
    """Translate unhandled exceptions into structured HTTP 500 responses.

    This prevents raw Python tracebacks from leaking to callers and ensures
    every error response is machine-parseable JSON.
    """
    logger.exception("Unhandled exception on %s: %s", request.url.path, exc)
    return JSONResponse(
        status_code=500,
        content={
            "error": type(exc).__name__,
            "detail": str(exc),
            "path": str(request.url.path),
        },
    )
