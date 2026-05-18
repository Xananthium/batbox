# include/batbox/tools/bash

forkpty-based shell command executor header.

## Files

### BashRunner.hpp
Stateless forkpty executor with watchdog, output cap, and ANSI stripping.

- `BashRunner::run(command, cwd, env_allowlist, timeout_sec, max_output_bytes, cancel_token, plan_mode) -> BashResult` — forks child via PtyBackend; scrubs environment to env_allowlist; caps output at max_output_bytes (appends truncation notice); fires SIGTERM at timeout_sec then SIGKILL 2 s later; fires SIGINT on cancel; strips ANSI escapes from output; returns BashResult::plan_mode_error() immediately when plan_mode=true
- `BashResult::plan_mode_error() -> BashResult` — static; returns error result with "BashRunner is not available in plan mode" body
