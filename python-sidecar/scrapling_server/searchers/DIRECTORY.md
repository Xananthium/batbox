# python-sidecar/scrapling_server/searchers

Web search backend modules: DuckDuckGo scraper and SearXNG API client.

## Files

### __init__.py
Package marker for the searchers subpackage.

### ddg.py
DuckDuckGo HTML scraper using Scrapling and BeautifulSoup.

- `search(query, count=10) -> list[dict]` — fetches DuckDuckGo HTML results page; parses result blocks via _parse_results(); returns list of {"title", "url", "snippet"} dicts up to count entries

### searxng.py
SearXNG JSON API client.

- `search(query, count=10, base_url="") -> list[dict]` — POSTs to <base_url>/search with format=json; parses results array; returns list of {"title", "url", "snippet"} dicts up to count entries
