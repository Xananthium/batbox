"""Tests: /fetch endpoint and _html_to_markdown must not leak CSS or JavaScript.

Verifies that minified JS (<script>) and CSS (<style>) text content is fully
removed from the markdown output — not merely the tag wrappers.

Run:
    cd python-sidecar
    python -m pytest tests/test_fetch_strips_assets.py -v
"""

from __future__ import annotations

import types
from unittest.mock import patch

import httpx
import pytest
import pytest_asyncio
from httpx import ASGITransport

from scrapling_server.app import _html_to_markdown, app

# ---------------------------------------------------------------------------
# Shared fixtures
# ---------------------------------------------------------------------------

_NOISE_HTML = (
    "<html>"
    "<head>"
    "<style>body{display:none}.x{color:red}</style>"
    "<meta charset='utf-8'>"
    "</head>"
    "<body>"
    "<h1>Hello</h1>"
    "<script>alert('xss')</script>"
    "<p>World</p>"
    "</body>"
    "</html>"
)

_GOOD_STRINGS = ("Hello", "World")
_BAD_STRINGS = ("display:none", "alert", "color:red", "charset")


def _make_fake_response(html: str) -> object:
    """Build a minimal fake Scrapling response for monkeypatching _fetch_page."""
    resp = types.SimpleNamespace()
    resp.html_content = html
    resp.status = 200
    resp.url = "http://test.local/page"
    resp.headers = {"content-type": "text/html; charset=utf-8"}
    return resp


# ---------------------------------------------------------------------------
# Unit test: _html_to_markdown directly
# ---------------------------------------------------------------------------


def test_html_to_markdown_strips_script_style() -> None:
    """_html_to_markdown must remove script/style *content*, not just tags."""
    result = _html_to_markdown(_NOISE_HTML)

    for good in _GOOD_STRINGS:
        assert good in result, f"Expected {good!r} to be present in markdown output"

    for bad in _BAD_STRINGS:
        assert bad not in result, (
            f"Found {bad!r} in markdown output — CSS/JS leak detected"
        )


# ---------------------------------------------------------------------------
# Integration test: /fetch endpoint (in-process, mocked _fetch_page)
# ---------------------------------------------------------------------------


@pytest.mark.anyio
async def test_fetch_endpoint_strips_assets() -> None:
    """POST /fetch must return markdown free of script/style content."""
    fake_response = _make_fake_response(_NOISE_HTML)

    with patch("scrapling_server.app._fetch_page", return_value=fake_response):
        transport = ASGITransport(app=app)
        async with httpx.AsyncClient(
            transport=transport, base_url="http://test"
        ) as client:
            resp = await client.post(
                "/fetch",
                json={"url": "http://test.local/page", "timeout": 10.0},
            )

    assert resp.status_code == 200, f"Unexpected status: {resp.status_code}"
    data = resp.json()
    markdown = data["markdown"]

    assert not data["is_error"], f"Response flagged as error: {data.get('error_message')}"

    for good in _GOOD_STRINGS:
        assert good in markdown, f"Expected {good!r} in /fetch markdown output"

    for bad in _BAD_STRINGS:
        assert bad not in markdown, (
            f"Found {bad!r} in /fetch markdown — CSS/JS leak via endpoint"
        )


# ---------------------------------------------------------------------------
# Sanity check: empty HTML must not crash
# ---------------------------------------------------------------------------


def test_html_to_markdown_empty_string() -> None:
    """_html_to_markdown must handle empty input without raising."""
    result = _html_to_markdown("")
    assert isinstance(result, str)


def test_html_to_markdown_no_noise_tags() -> None:
    """_html_to_markdown must pass through clean HTML content correctly."""
    clean_html = "<html><body><h2>Title</h2><p>Body text here.</p></body></html>"
    result = _html_to_markdown(clean_html)
    assert "Title" in result
    assert "Body text here" in result
