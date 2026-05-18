# src/sidecar

Python Scrapling sidecar C++ implementation: lifecycle management, HTTP client, protocol serialisation, and state machine.

## Files

### SidecarManager.cpp
`ensure_started()` implementation: CAS Cold→Starting; calls do_spawn() (pick_free_port → build_envp → posix_spawn); calls wait_for_healthy() (100 ms /healthz poll loop); restart cap enforcement. `shutdown()` sequence: POST /shutdown → SIGTERM → SIGKILL → waitpid → join stderr thread. `prewarm_async()`: launches std::async task. `sidecar_post_json_raw()` free function: raw cpr POST with cancellation bridge.

### ScraplingClient.cpp
`fetch()`, `search()`, `select()` implementations: delegate to `post_json()` with the appropriate endpoint; parse typed response via from_json(). `post_json()`: serialises body; constructs cpr::Session; bridges CancelToken to shared_ptr<atomic_bool> on_cancel callback; classifies errors.

### ScraplingProto.cpp
`to_json()` and `from_json()` implementations for all proto types: FetchRequest/Response, SearchRequest/Response/Result, SelectRequest/Response, HealthResponse, ShutdownResponse, ErrorResponse.

### SidecarState.cpp
`to_string()` implementation: static string_view table. `is_legal_transition()`: switch table of all allowed edges. `SidecarStateMachine::try_transition()`: compare_exchange_strong; fires on_transition_ callback on success.

### CMakeLists.txt
Build rules for the sidecar static library.
