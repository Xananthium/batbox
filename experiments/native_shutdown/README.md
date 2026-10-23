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
six compactor cases, and 26 inference smoke cases also pass. This regression
uses CPU-only loopback stubs. The later production check below exercised the
installed binary while inference and memory indexing continued.

```bash
cmake --build build --target batbox -j 4
ctest --test-dir build \
  -R '^(native_shutdown_lifecycle|native_worker_lifecycle|autonomous_engine_lifecycle)$' \
  --output-on-failure -j 2
```

`20261023-result.json` records the tested binary and source hashes. The fix does
not impose a universal shutdown deadline on arbitrary tools or filesystem IO.

The repaired production engine was then stopped and restarted at
2026-10-23 17:06 UTC. Shutdown completed in **1.417 seconds** with exit status 0,
removed the running marker and produced no new crash report. The new process
resumed the same coordinator session and all three worker identities. The
inference backend remained the pinned PR141 original GGUF release throughout.
See `20261023-live-graceful-restart.json` for sanitized observations and the
tested binary hash. The current release adds completed-compaction progress
notifications; its focused lifecycle tests are separate from this live check.
