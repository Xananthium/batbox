# python-sidecar — BatBox Scrapling sidecar

Loopback HTTP bridge between the BatBox C++ host and the upstream Python
[Scrapling](https://github.com/D4Vinci/Scrapling) library for WebFetch /
WebSearch / WebSelect.

---

## Architecture

```
batbox (C++)
  └─ SidecarManager (src/sidecar/)
       └─ posix_spawn: python3 -m scrapling_server [--port N]
            └─ FastAPI app on 127.0.0.1:<port>
                 ├── GET  /healthz   — liveness probe
                 ├── POST /fetch     — URL → markdown (via Scrapling)
                 ├── POST /search    — DDG HTML or SearXNG search
                 ├── POST /select    — CSS / XPath selector
                 └── POST /shutdown  — graceful exit
```

The C++ host picks a free port, passes it via `--port` (or the
`BATBOX_SIDECAR_PORT` env var), then reads the child's stderr for the
line:

```
LISTENING:<port>
```

Once that line appears the host polls `GET /healthz` every 100 ms until it
receives HTTP 200, then marks the sidecar state as **Running**.

---

## Install

### Option A — editable install (recommended for development)

```bash
cd python-sidecar
python3 -m venv .venv
source .venv/bin/activate
pip install -e .
```

### Option B — requirements.txt (simpler, no build backend needed)

```bash
cd python-sidecar
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

### Option C — via `batbox setup-sidecar` (end-user install)

The BatBox binary ships the `python-sidecar/` directory. On first run of the
`setup-sidecar` subcommand it:

1. Creates `~/.batbox/sidecar/.venv` via `python3 -m venv`.
2. Copies `python-sidecar/` to `~/.batbox/sidecar/python-sidecar/`.
3. Runs `pip install -r requirements.txt` inside the venv.

---

## Running manually

```bash
# With a fixed port:
python3 -m scrapling_server --port 8765

# Let the OS pick a port (prints LISTENING:<port> on stderr):
python3 -m scrapling_server

# Debug mode (enables /docs and /redoc UI):
BATBOX_SIDECAR_DEBUG=1 python3 -m scrapling_server --port 8765
```

---

## Environment variables

| Variable | Default | Description |
|---|---|---|
| `BATBOX_SIDECAR_PORT` | `0` | Override listen port (0 = OS assigns) |
| `BATBOX_SIDECAR_DEBUG` | unset | Set to `1` to enable `/docs` and `/redoc` |
| `BATBOX_SEARXNG_URL` | unset | SearXNG instance base URL for `engine=searxng` |
| `BATBOX_RESPECT_ROBOTS` | `true` | Honour robots.txt on fetch/search |

---

## How the C++ host spawns the sidecar

From `src/sidecar/SidecarManager.cpp` (task CPP 7.2):

```cpp
// 1. Pick free port (or pass 0 to let Python choose):
const int port = pick_free_port();   // bind→getsockname→close on 127.0.0.1

// 2. Build argv:
const char* argv[] = {python_bin, "-m", "scrapling_server", "--port",
                      port_str, nullptr};

// 3. Set envp (PYTHONUNBUFFERED=1, VIRTUAL_ENV, PATH prepend, SCRAPLING_PORT).

// 4. posix_spawn with stdout/stderr pipes.

// 5. Reader thread on child stderr — looks for "LISTENING:<port>" line.

// 6. Poll GET /healthz every 100 ms.  First 200 → state = Running.
```

---

## Debugging tips

- **Sidecar not starting**: check that `python3` is on `PATH` and the venv is
  activated (or that `VIRTUAL_ENV` points to the correct `.venv/` directory).

- **Import errors**: run `pip install -e .` inside the venv to ensure all deps
  are present.

- **LISTENING line missing**: `PYTHONUNBUFFERED=1` must be set in the child's
  environment — the C++ host sets this automatically.  If running manually,
  either set the env var or use `python3 -u -m scrapling_server`.

- **Port conflict**: use `--port 0` (default) and let the OS assign a free
  port; the assigned port is printed on stderr as `LISTENING:<port>`.

- **`/docs` not available**: set `BATBOX_SIDECAR_DEBUG=1` to enable the
  Swagger UI at `http://127.0.0.1:<port>/docs`.

- **Endpoint returns HTTP 500 "not_implemented"**: the handler bodies for
  `/fetch`, `/search`, `/select`, and `/shutdown` are filled in by task
  **CPP 7.7**.  The scaffolding exists so the C++ healthcheck and startup
  machinery can be developed and tested independently.

---

## Task dependency map

| Task | What it does |
|---|---|
| **CPP B.13** (this task) | Scaffolding: package layout, `__main__.py`, `app.py` with endpoint stubs |
| **CPP 7.7** | Wires handler bodies: Scrapling fetch, DDG/SearXNG search, selector query, shutdown |
