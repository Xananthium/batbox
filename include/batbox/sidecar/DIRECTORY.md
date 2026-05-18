# include/batbox/sidecar

Python Scrapling sidecar headers: lifecycle controller, HTTP client, request/response protocol types, and atomic state machine.

## Files

### SidecarManager.hpp
Lifecycle controller for the Python Scrapling sidecar subprocess.

- `SidecarManager::SidecarManager(cfg)` — constructs from SidecarConfig (python_bin, venv_dir, startup_timeout_sec)
- `SidecarManager::ensure_started(ct) -> Result<void>` — transitions Cold→Starting via CAS; spawns process; polls /healthz at 100 ms intervals; returns Ok when Running; respects restart cap (3 per session)
- `SidecarManager::shutdown()` — sends POST /shutdown; waits 2 s; SIGTERM; waits 2 s; SIGKILL; waitpid; joins stderr reader thread
- `SidecarManager::request<Req, Resp>(endpoint, req, ct) -> Result<Resp>` — template; calls ensure_started(), posts req.to_json() to endpoint, parses Resp::from_json(); transitions to CrashedRestarting on child exit
- `SidecarManager::current_state() -> SidecarState` — returns current lifecycle state (Disabled/Cold/Starting/Running/CrashedRestarting)
- `SidecarManager::port() -> uint16_t` — returns the ephemeral port the sidecar is bound to; 0 before startup
- `SidecarManager::restart_count() -> int` — returns number of restart attempts this session
- `SidecarManager::prewarm_async(ct, status_cb)` — spawns std::async task calling ensure_started(); returns immediately; status_cb called with "prewarming"/"ready"/"failed:..."
- `SidecarManager::wait_prewarm() -> Result<void>` — if prewarm future in-flight, blocks until complete; returns Ok immediately if no prewarm
- `SidecarManager::abort_startup()` — sends SIGTERM to child process group; transitions to CrashedRestarting; called by double-tap Ctrl+C handler

### ScraplingClient.hpp
HTTP client for the Python Scrapling sidecar FastAPI server.

- `ScraplingClient::ScraplingClient(port, timeout_sec=30)` — constructs with base URL "http://127.0.0.1:<port>"
- `ScraplingClient::fetch(req, ct) -> Result<FetchResponse, string>` — POSTs to /fetch; cancellable; returns Err on transport/HTTP/JSON error or cancellation
- `ScraplingClient::search(req, ct) -> Result<SearchResponse, string>` — POSTs to /search; same error contract as fetch
- `ScraplingClient::select(req, ct) -> Result<SelectResponse, string>` — POSTs to /select; same error contract as fetch
- `ScraplingClient::healthz() -> bool` — GETs /healthz with 500 ms timeout; returns true on HTTP 200
- `ScraplingClient::shutdown()` — POSTs to /shutdown best-effort; ignores all errors

### ScraplingProto.hpp
Request/response structs matching the Python sidecar's Pydantic models.

- `FetchRequest::to_json() -> Json` — serialises url, timeout, stealth, respect_robots, max_bytes
- `FetchRequest::from_json(j) -> FetchRequest` — deserialises; uses field defaults for absent optional fields
- `FetchResponse::to_json() -> Json` — serialises url, markdown, status_code, content_type, truncated, is_error, error_message
- `FetchResponse::from_json(j) -> FetchResponse` — deserialises fetch response
- `SearchRequest::to_json() -> Json` — serialises query, n, engine, searxng_url
- `SearchResponse::from_json(j) -> SearchResponse` — deserialises including results vector
- `SelectRequest::to_json() -> Json` — serialises url, selector, timeout, stealth, attribute
- `SelectResponse::from_json(j) -> SelectResponse` — deserialises matches list and count
- `ErrorResponse::from_json(j) -> ErrorResponse` — deserialises error/detail/path from HTTP 4xx/5xx body

### SidecarState.hpp
Atomic state machine for the sidecar subprocess lifecycle.

- `to_string(state) -> string_view` — returns "disabled"/"cold"/"starting"/"running"/"crashed-restarting" from static storage
- `is_legal_transition(from, to) -> bool` — validates state graph edges; returns false for illegal transitions
- `SidecarStateMachine::current() -> SidecarState` — loads current state with sequential consistency
- `SidecarStateMachine::try_transition(from, to) -> bool` — validates and CAS-swaps; fires on_transition callback on success; returns true to the winning thread
- `SidecarStateMachine::set_on_transition(fn)` — registers callback void(old_state, new_state); called after every successful CAS
