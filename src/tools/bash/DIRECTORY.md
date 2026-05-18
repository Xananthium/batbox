# src/tools/bash

forkpty-based shell execution backend with environment scrubbing and ANSI stripping.

## Files

### BashRunner.cpp
`BashRunner::run()` implementation: calls PtyBackend::spawn() to fork the child; starts a reader thread that drains the pty fd into the output buffer; starts a watchdog thread that fires SIGTERM at timeout then SIGKILL 2 s later; fires SIGINT to pgid on cancel_token; calls AnsiStrip::strip() on accumulated output; appends "\n[exit=N, duration=Xms]" trailer.

### PtyBackend.cpp
`posix_openpt`/`grantpt`/`unlockpt`/`ptsname` sequence; forks child; child calls `setsid()` + `ioctl TIOCSCTTY` to claim the pty as controlling terminal; parent retains master fd for read/write.

### PtyBackendInternal.hpp
Internal types and helpers used only by PtyBackend.cpp.

### PipesBackend.cpp
Fallback backend using POSIX pipes when pty is unavailable (CI environments); posix_spawn with stdin/stdout/stderr redirected to pipe pairs.

### PipesBackendInternal.hpp
Internal types for PipesBackend.

### EnvScrub.cpp
`EnvScrub::scrub(allowlist) -> vector<string>` — filters the current process environment to only the names in allowlist; returns null-terminated envp-style vector; default allowlist: PATH, HOME, USER, LANG, TERM, SHELL.

### EnvScrubInternal.hpp
Internal helpers for EnvScrub.

### AnsiStrip.cpp
`AnsiStrip::strip(text) -> string` — removes ANSI escape sequences (CSI, OSC, and other ESC-prefixed sequences) from text; preserves printable characters and newlines; used on pty output before returning to the model.

### AnsiStripInternal.hpp
Internal state machine types for the ANSI strip parser.

### CMakeLists.txt
Build rules for the bash tool backend static library.
