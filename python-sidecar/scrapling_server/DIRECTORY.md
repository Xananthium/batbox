# python-sidecar/scrapling_server

Python FastAPI sidecar server: HTTP endpoints for web fetch, web search, and CSS selector extraction using the Scrapling library.

## Files

### app.py
FastAPI application with four HTTP endpoints.

- `healthz() -> dict` — GET /healthz; returns {"status": "ok"}; used by SidecarManager for startup health-check polling
- `fetch(body: FetchRequest) -> FetchResponse` — POST /fetch; calls _fetch_page() and _html_to_markdown(); returns markdown-rendered content with status_code, content_type, truncated flag
- `search(body: SearchRequest) -> SearchResponse` — POST /search; dispatches to ddg.search() or searxng.search() based on body.engine; returns list of SearchResult
- `select(body: SelectRequest) -> SelectResponse` — POST /select; calls _extract_selector_matches(); returns list of matched text or attribute values
- `shutdown(background_tasks) -> dict` — POST /shutdown; schedules os._exit(0) as a background task; returns {"shutting_down": true}
- `generic_exception_handler(request, exc) -> JSONResponse` — FastAPI exception handler; returns {"error": type_name, "detail": str(exc), "path": request.url.path} as HTTP 500

### __init__.py
Package marker for the scrapling_server Python package.

### __main__.py
Entry point when the package is run with `python -m scrapling_server`; parses --port argument; calls uvicorn.run() with the FastAPI app on 127.0.0.1:<port>.
