# Native worker shutdown regression

The production engine emitted `stopped` on October 23 before its worker joins
finished, then exceeded systemd's 45-second stop timeout and received SIGKILL.
One worker still marked running had a Bash result containing 456,362 tokens
under the deployed tokenizer, enough to enter model summarization. No process
backtrace was captured, so that production diagnosis is supported by the code
and reproduction rather than a measured blocked stack.

The installed `f287c2f` binary reproduced two cancellation failures against local
HTTP stubs: an in-flight compactor summary and an autonomous service tool each
prevented exit within five seconds after SIGTERM. The streaming inference
control exited normally. Non-streaming summaries did not pass a cancellation
token to the HTTP client; service-tool HTTP also ignored cancellation.

The patch passes cancellation through those calls and retains the running
marker until all worker joins finish. All three cases now stop within five
seconds. Each then restarts and verifies unchanged worker IDs, session IDs, and
earlier transcript messages. Existing native worker/autonomous lifecycle tests,
six compactor cases, and 26 inference smoke cases also pass. This is a CPU-only
loopback regression; no production service was changed or live GPU inference run.

```bash
cmake --build build --target batbox -j 4
ctest --test-dir build \
  -R '^(native_shutdown_lifecycle|native_worker_lifecycle|autonomous_engine_lifecycle)$' \
  --output-on-failure -j 2
```

`20261023-result.json` records the tested binary and source hashes. The fix does
not impose a universal shutdown deadline on arbitrary tools or filesystem IO.
