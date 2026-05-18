# include/batbox/perf

Performance snapshot types and global store for latency instrumentation.

## Files

### PerfSnapshot.hpp
First-token, stream-to-paint, and frame latency tracking.

- `PerfStore::set_first_token_ms(ms)` — records time from request send to first SSE token received; stored in g_perf
- `PerfStore::set_stream_to_paint_ms(ms)` — records time from stream completion to TUI repaint; stored in g_perf
- `PerfStore::set_frame_ms(ms)` — records time for one FTXUI frame render; stored in g_perf
- `PerfStore::snapshot() -> PerfSnapshot` — atomically reads and returns the current latency values
- `g_perf` — process-global PerfStore instance; written by the inference loop and TUI render path
- `g_perf_enabled` — boolean flag; when false all set_* calls are no-ops (zero overhead in production)
