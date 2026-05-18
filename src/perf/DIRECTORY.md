# src/perf

Performance instrumentation implementation.

## Files

### PerfSnapshot.cpp
`PerfStore::set_first_token_ms()`, `set_stream_to_paint_ms()`, `set_frame_ms()`, `snapshot()` implementations; atomic store/load on each latency field; g_perf global PerfStore instance; g_perf_enabled flag checked before every store to eliminate overhead in non-perf builds.

### CMakeLists.txt
Build rules for the perf static library.
