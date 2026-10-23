# BatBox autonomous memory

This loopback service provides persistent memory, CPU embeddings, official HippoRAG retrieval, and cached web tools. Data, authentication state and model calls remain on the user's machine. Search and fetch contact their configured destinations. Software memory and activity do not establish consciousness.

`POST /remember` durably queues content/provenance and returns HTTP 202 without waiting for embeddings or graph inference. Background workers write nodes, generate CPU embeddings, and incrementally build HippoRAG. Failed jobs remain retryable. Source versions remain in the queue archive; original files are unchanged. A SQLite backup is made before migration.

FastEmbed uses `BAAI/bge-small-en-v1.5`, 384-dimensional ONNX embeddings with `CPUExecutionProvider`. Its content-addressed persistent cache is shared by documents, queries and HippoRAG. HippoRAG separately caches LLM extraction responses. SQLite wiki-link references and HippoRAG entity/passage/synonym graph counts are reported separately.

## API

The API binds only to `127.0.0.1:8771` and is for trusted local processes. Do not expose it through a public reverse proxy.

| Endpoint | Body / purpose |
| --- | --- |
| `GET /health`, `GET /status` | Workers, queue, errors, counts, cache metrics |
| `POST /ingest` | `{}` scans configured sources and enqueues changes |
| `POST /remember` | `{content,source,id?,kind?,captured_at?,metadata?}`; returns 202 pending |
| `POST /recall` | `{query,limit?}`; attributed results and actual retrieval backend |
| `POST /search` | `{query,limit?}`; SearXNG with 1-hour cache |
| `POST /fetch` | `{url,session_id?,selector?,refresh?}`; Scrapling text/CSS extraction |
| `POST /sessions/configure` | `{session_id,allowed_origins,engine:"http" or "browser"}` |
| `POST /sessions/status` | `{session_id?}`; presence of credentials/state, never secret values |
| `POST /sessions/login` | `{session_id,url}`; login defined in the private credential file |

Recall uses official HippoRAG when its graph is available and not being updated. Otherwise it returns CPU vector retrieval explicitly labeled `cpu-vector`. `ready` in the memory queue means document embeddings are ready; `graph_index` and `hipporag` report graph progress separately. Extraction errors remain visible and retryable.

Each graph update saves a checkpoint and a durable operation record before changing HippoRAG's files. Startup restores and replays interrupted operations, including source replacements. Completed extraction and embedding caches are retained. The operation record is removed only after the graph files and SQLite progress record are committed. A stale graph from older versions can be rebuilt from compatible stored chunks and extraction results; configuration or provenance mismatches remain explicit errors.

Each memory requires an HTTP(S), file or transcript source URI, capture time and SHA-256 hash. Historical Heather/Maya imports also retain source author/date, import time, local URI and immutable repository commit URL. Retrieval visibly warns that those records describe past state and do not define the engine's identity. Generated observations remain attributed to their transcript, not treated as verified external facts.

## Durable private sessions

Named sessions persist HTTP cookie jars or browser cookies/localStorage/IndexedDB. State and credential files are mode 0600 within a mode 0700 directory. Exact allowed origins constrain redirects and browser requests. Credentials are selected per origin. Authenticated cache entries are partitioned by session and credential/cookie-state digest; authenticated captures are not automatically inserted into general memory. Login responses are not cached. Browser sessionStorage is not persisted.

Create `session_directory/secrets/NAME.json` locally with mode 0600. Tools receive only NAME. Example schema (replace placeholders locally):

```json
{"origins":{"https://service.example":{
  "headers":{"Authorization":"Bearer REPLACE_LOCALLY"},
  "login":{"url":"https://service.example/login","form":{"username":"REPLACE_LOCALLY","password":"REPLACE_LOCALLY"}},
  "browser_login":{"url":"https://service.example/login","fields":{"#username":"REPLACE_LOCALLY","#password":"REPLACE_LOCALLY"},"submit":"button[type=submit]"}
}}}
```

Ordinary fetch is GET. Only exact configured login forms may POST. Use a new session name when changing origins or switching HTTP/browser engines. Public fetches cache for 6 hours, authenticated fetches for 30 minutes. Content is capped at 40,000 characters with a visible `truncated` flag. CSS selectors can target relevant portions.

## Deployment and tests

Use an isolated Python 3.12 environment. Install `torch==2.5.1` from `https://download.pytorch.org/whl/cpu`, then `requirements.txt`. Install official HippoRAG using `--no-deps` from revision `d5c8329422e0a0b834a15874545cb6a74b4f9b26` of `https://github.com/OSU-NLP-Group/HippoRAG`. The deployed environment is `/home/isnotreal/engine-env`; `pip check` passes. Scrapling uses the separate installed Python environment and browser binaries configured in JSON.

Run from `python-sidecar`:

```bash
/home/isnotreal/engine-env/bin/python -m batbox_memory --config /home/isnotreal/.batbox/memory-service.json
/home/isnotreal/engine-env/bin/python -m unittest discover -s tests/memory -v
```

The user unit is `batbox-memory.service`. Configuration specifies database, graph/session directories, local LLM URL/model, embedding path, Scrapling Python, SearXNG URL and sources. Sources may be file/directory strings or objects with `path`, `recursive`, `source_base`, `source_author`, `historical`, `source_commit`, `git_root`, `kind` and `priority`. Explicit JSON/code files are supported. Directory scanning imports Markdown notes only, including legacy names with trailing backticks; it does not implicitly scan builds or environments.

Shutdown waits for background workers for `shutdown_timeout_seconds` (default 150 seconds). Set the service unit's `TimeoutStopSec=180` or above that configured deadline. An update that exceeds the deadline remains recoverable through its checkpoint when the process restarts.

Tests exercise actual Scrapling login/cookie restoration, browser cookies/localStorage across fresh processes, credential origin boundaries, cache isolation, file permissions, durable asynchronous queue recovery, deduplication/content reversion and embedding retries. Graph recovery tests use real HippoRAG stores and SIGKILL at insertion, deletion, metadata, SQLite commit and recovery boundaries, with deterministic CPU/extraction fixtures and no model endpoint. Graph extraction quality still depends on model and source accuracy.
